#include <slopnet.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <cute_json.h>
#include <cute_coroutine.h>
#include <cute_alloc.h>
#include <cute_time.h>
#include <SDL3/SDL_misc.h>

#include "slopnet_fetch.h"
#include "slopnet_transport.h"
#include "slopnet_oauth.h"

#define BARENA_API static
#include "barena.h"

#define SNET_URL_FMT_PREFIX "https://%s:%d%s"
#define SNET_URL_FMT_PREFIX_ARGS(snet) (snet)->config.host, (snet)->config.port, (snet)->config.path
#define SNET_MAX_COOKIE_SIZE 1024
#define SNET_TASK_ARG(TYPE, ARG) \
	TYPE ARG; \
	memcpy(&ARG, env->arg, sizeof(ARG))

typedef struct {
	void* ptr;
	size_t capacity;
} snet_buf_t;

typedef struct snet_task_env_s snet_task_env_t;
typedef void (*snet_task_fn_t)(const snet_task_env_t* env);

typedef struct {
	CF_Coroutine coro;
	bool cancelled;
	const snet_event_t* result;
	barena_t arena;
} snet_task_t;

struct snet_task_env_s {
	snet_task_t* self;
	snet_t* snet;
	void* arg;

	snet_task_fn_t entry;
};

struct snet_s {
	snet_config_t config;

	snet_auth_state_t auth_state;
	snet_lobby_state_t lobby_state;

	barena_pool_t arena_pool;
	snet_task_t auth_task;
	snet_task_t create_game_task;
	snet_task_t join_game_task;
	snet_task_t list_games_task;

	snet_transport_t* transport;
	snet_event_t current_event;

	char cookie_buf[SNET_MAX_COOKIE_SIZE];
};

static inline void
snet_log(snet_t* snet, const char* fmt, ...) {
	if (snet->config.log == NULL) { return; }

	va_list args;
	va_start(args, fmt);
	snet->config.log(fmt, args, snet->config.logctx);
	va_end(args);
}

// Task {{{

static void
snet_task_end(snet_task_t* task) {
	if (task->coro.id != 0) {
		if (cf_coroutine_state(task->coro) != CF_COROUTINE_STATE_DEAD) {
			task->cancelled = true;
		}
		while (cf_coroutine_state(task->coro) != CF_COROUTINE_STATE_DEAD) {
			cf_coroutine_resume(task->coro);
		}
		cf_destroy_coroutine(task->coro);
		task->coro.id = 0;
	}

	task->result = NULL;
	barena_reset(&task->arena);
}

static void
snet_task_wrapper(CF_Coroutine coro) {
	const snet_task_env_t* env = cf_coroutine_get_udata(coro);
	env->entry(env);
}

static void
snet_task_begin(snet_t* snet, snet_task_t* task, snet_task_fn_t fn, const void* arg, size_t arg_size) {
	snet_task_end(task);

	void* arg_copy = NULL;
	if (arg != NULL) {
		arg_copy = barena_malloc(&task->arena, arg_size);
		memcpy(arg_copy, arg, arg_size);
	}
	snet_task_env_t* env = barena_malloc(&task->arena, sizeof(arg_size));
	*env = (snet_task_env_t){
		.arg = arg_copy,
		.self = task,
		.snet = snet,
		.entry = fn,
	};
	task->coro = cf_make_coroutine(snet_task_wrapper, 0, env);
	task->cancelled = false;
	task->result = NULL;
	cf_coroutine_resume(task->coro);
}

static void
snet_task_process(snet_task_t* task) {
	if (task->coro.id != 0 && cf_coroutine_state(task->coro) != CF_COROUTINE_STATE_DEAD) {
		cf_coroutine_resume(task->coro);
	}
}

static const snet_event_t*
snet_task_reap(snet_task_t* task) {
	if (
		task->coro.id != 0
		&&
		cf_coroutine_state(task->coro) == CF_COROUTINE_STATE_DEAD
		&&
		task->result != NULL
	) {
		const snet_event_t* result = task->result;
		task->result = NULL;
		return result;
	} else {
		return NULL;
	}
}

static void
snet_task_init(snet_t* snet, snet_task_t* task) {
	barena_init(&task->arena, &snet->arena_pool);
	task->coro.id = 0;
}

static void
snet_task_cleanup(snet_task_t* task) {
	snet_task_end(task);
}

static inline void*
snet_task_alloc(const snet_task_env_t* env, size_t size) {
	return barena_malloc(&env->self->arena, size);
}

static inline bool
snet_task_cancelled(const snet_task_env_t* env) {
	return env->self->cancelled;
}

static inline void
snet_task_yield(const snet_task_env_t* env) {
	cf_coroutine_yield(env->self->coro);
}


static inline void
snet_task_post(const snet_task_env_t* env, const snet_event_t* event) {
	void* result = snet_task_alloc(env, sizeof(*event));
	memcpy(result, event, sizeof(*event));
	env->self->result = result;
}

// }}}

static inline const char*
snet_vprintf(const snet_task_env_t* env, const char* fmt, va_list args) {
	va_list args_copy;
	va_copy(args_copy, args);
	int size = vsnprintf(NULL, 0, fmt, args);
	char* buf = snet_task_alloc(env, size + 1);
	vsnprintf(buf, size + 1, fmt, args_copy);
	va_end(args_copy);
	return buf;
}

static inline const char*
snet_printf(const snet_task_env_t* env, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	const char* result = snet_vprintf(env, fmt, args);
	va_end(args);
	return result;
}

snet_t*
snet_init(const snet_config_t* config_in) {
	snet_config_t config = { 0 };
	if (config_in != NULL) {
		config = *config_in;
	}

	if (config.path == NULL) {
		config.path = "/";
	}

	if (config.port == 0) {
		config.port = 443;
	}

	snet_t* snet = cf_alloc(sizeof(snet_t));
	*snet = (snet_t){
		.config = config,
	};

	barena_pool_init(&snet->arena_pool, 1);
	(void)barena_snapshot;

	snet_task_init(snet, &snet->auth_task);
	snet_task_init(snet, &snet->create_game_task);
	snet_task_init(snet, &snet->join_game_task);
	snet_task_init(snet, &snet->list_games_task);

	return snet;
}

void
snet_cleanup(snet_t* snet) {
	snet_task_cleanup(&snet->auth_task);
	snet_task_cleanup(&snet->create_game_task);
	snet_task_cleanup(&snet->join_game_task);
	snet_task_cleanup(&snet->list_games_task);
	barena_pool_cleanup(&snet->arena_pool);

	if (snet->transport) { snet_transport_cleanup(snet->transport); }
	cf_free(snet);
}

void
snet_update(snet_t* snet) {
	snet_task_process(&snet->auth_task);
	snet_task_process(&snet->create_game_task);
	snet_task_process(&snet->join_game_task);
	snet_task_process(&snet->list_games_task);

	if (snet->transport) {
		snet_transport_update(snet->transport);
	}
}

const snet_event_t*
snet_next_event(snet_t* snet) {
	const snet_event_t* event;
	if ((event = snet_task_reap(&snet->auth_task)) != NULL) {
		return event;
	}

	if ((event = snet_task_reap(&snet->create_game_task)) != NULL) {
		return event;
	}

	if ((event = snet_task_reap(&snet->join_game_task)) != NULL) {
		return event;
	}

	if ((event = snet_task_reap(&snet->list_games_task)) != NULL) {
		return event;
	}

	if (snet->transport != NULL) {
		size_t packet_size;
		const void* packet;
		if (snet_transport_recv(snet->transport, &packet, &packet_size)) {
			snet->current_event = (snet_event_t){
				.type = SNET_EVENT_MESSAGE,
				.message.data = {
					.ptr = packet,
					.size = packet_size,
				},
			};
			return &snet->current_event;
		}

		if (snet_transport_state(snet->transport) == SNET_TRANSPORT_DISCONNECTED) {
			snet_transport_cleanup(snet->transport);
			snet->transport = NULL;
			snet->current_event.type = SNET_EVENT_DISCONNECTED;
			snet->lobby_state = SNET_IN_LOBBY;
			return &snet->current_event;
		}
	}

	return NULL;
}

snet_auth_state_t
snet_auth_state(snet_t* snet) {
	return snet->auth_state;
}

snet_lobby_state_t
snet_lobby_state(snet_t* snet) {
	return snet->lobby_state;
}

static void
snet_task_login_with_cookie(const snet_task_env_t* env) {
	SNET_TASK_ARG(snet_blob_t, cookie);
	snet_t* snet = env->snet;

	snet->auth_state = SNET_AUTHORIZING;
	snet_fetch_t* fetch = snet_fetch_begin(&(snet_fetch_options_t){
		.method = SNET_FETCH_POST,
		.host = snet->config.host,
		.port = snet->config.port,
		.path = snet_printf(env, "%s%s", snet->config.path, "/auth/cookie"),
		.verify_tls = !snet->config.insecure_tls,

		.content = cookie.ptr, .content_length = cookie.size,
	});

	snet_fetch_status_t fetch_status;
	while (true) {
		if (snet_task_cancelled(env)) { break; }

		if ((fetch_status = snet_fetch_process(fetch)) != SNET_FETCH_PENDING) {
			break;
		}
		snet_task_yield(env);
	}

	snet_op_status_t op_status = SNET_ERR_IO;
	snet->auth_state = SNET_UNAUTHORIZED;
	size_t cookie_size = 0;
	snet_log(snet, "fetch status: %d", fetch_status);
	if (fetch_status == SNET_FETCH_FINISHED) {
		int status_code = snet_fetch_status_code(fetch);
		snet_log(snet, "status code: %d", status_code);
		op_status = status_code == 200 ? SNET_OK : SNET_ERR_REJECTED;
		if (op_status == SNET_OK) {
			snet->auth_state = SNET_AUTHORIZED;
			const void* body = snet_fetch_response_body(fetch, &cookie_size);
			if (cookie_size < sizeof(snet->cookie_buf)) {
				memcpy(snet->cookie_buf, body, cookie_size);
				snet->cookie_buf[cookie_size] = '\0';
			} else {
				cookie_size = 0;
				op_status = SNET_ERR_IO;
				snet_log(snet, "Body is too large");
			}
		}
	}

	snet_task_post(env, &(snet_event_t){
		.type = SNET_EVENT_LOGIN_FINISHED,
		.login = {
			.status = op_status,
			.data = {
				.ptr = snet->cookie_buf,
				.size = cookie_size,
			}
		},
	});

	snet_fetch_end(fetch);
}

void
snet_login_with_cookie(snet_t* snet, snet_blob_t cookie) {
	snet_task_begin(
		snet, &snet->auth_task,
		snet_task_login_with_cookie, &cookie, sizeof(cookie)
	);
}

static void*
snet_oauth_alloc(size_t size, size_t alignment, void* memctx) {
	const snet_task_env_t* env = memctx;
	return barena_memalign(&env->self->arena, size, alignment);
}

static void
snet_task_login_with_itchio(const snet_task_env_t* env) {
	SNET_TASK_ARG(snet_blob_t, cookie);
	snet_t* snet = env->snet;

	snet->auth_state = SNET_AUTHORIZING;
	snet_oauth_t* oauth = snet_oauth_begin(&(snet_oauth_config_t){
		.start_url = snet_printf(env, SNET_URL_FMT_PREFIX "/auth/itchio/start", SNET_URL_FMT_PREFIX_ARGS(snet)),
		.end_url = snet_printf(env, SNET_URL_FMT_PREFIX "/auth/itchio/end", SNET_URL_FMT_PREFIX_ARGS(snet)),
		.alloc = snet_oauth_alloc,
		.memctx = (void*)env,
	});


	snet_oauth_state_t oauth_state;
	while (true) {
		if (snet_task_cancelled(env)) { break; }

		if ((oauth_state = snet_oauth_update(oauth)) != SNET_OAUTH_PENDING) {
			break;
		}
		snet_task_yield(env);
	}

	if (oauth_state != SNET_OAUTH_PENDING) {  // We could be cancelled
		size_t data_size;
		const void* oauth_data = snet_oauth_data(oauth, &data_size);
		if (oauth_data != NULL && data_size < sizeof(snet->cookie_buf)) {
			memcpy(snet->cookie_buf, oauth_data, data_size);
			snet->cookie_buf[data_size] = '\0';
		}
		snet_task_post(env, &(snet_event_t){
			.type = SNET_EVENT_LOGIN_FINISHED,
			.login = {
				.status = oauth_state == SNET_OAUTH_SUCCESS ? SNET_OK : SNET_ERR_REJECTED,
				.data = { .ptr = snet->cookie_buf, .size = data_size },
			},
		});
	} else {
		snet_task_post(env, &(snet_event_t){
			.type = SNET_EVENT_LOGIN_FINISHED,
			.login = {
				.status = SNET_ERR_IO,
			},
		});
	}

	snet->auth_state = oauth_state == SNET_OAUTH_SUCCESS ? SNET_AUTHORIZED : SNET_UNAUTHORIZED;

	snet_oauth_end(oauth);
}

void
snet_login_with_itchio(snet_t* snet) {
	snet_task_begin(snet, &snet->auth_task, snet_task_login_with_itchio, NULL, 0);
}

static snet_fetch_header_t
snet_auth_header(const snet_task_env_t* env, snet_t* snet) {
	return (snet_fetch_header_t){
		.name = "Authorization",
		.value = snet_printf(env, "Bearer %s", snet->cookie_buf),
	};
}

static snet_blob_t
snet_strcpy(const snet_task_env_t* env, const char* str) {
	if (str == NULL) { return (snet_blob_t){ 0 }; }
	size_t len = strlen(str);
	char* copy = snet_task_alloc(env, len + 1);
	memcpy(copy, str, len + 1);
	return (snet_blob_t){
		.ptr = copy,
		.size = len,
	};
}

static void
snet_task_create_game(const snet_task_env_t* env) {
	SNET_TASK_ARG(snet_game_options_t, options);

	CF_JDoc doc = cf_make_json(NULL, 0);
	CF_JVal req = cf_json_object(doc);
	cf_json_set_root(doc, req);
	cf_json_object_add_string(doc, req, "visibility", options.visibility == SNET_GAME_PUBLIC ? "public" : "private");
	cf_json_object_add_int(doc, req, "max_num_players", options.max_num_players);
	if (options.data.ptr) {
		cf_json_object_add_string_range(doc, req, "data", options.data.ptr, (char*)options.data.ptr + options.data.size);
	}
	dyna char* req_body = cf_json_to_string_minimal(doc);
	cf_destroy_json(doc);

	snet_t* snet = env->snet;
	snet->lobby_state = SNET_CREATING_GAME;
	snet_log(snet, "Creating game");

	snet_fetch_t* fetch = snet_fetch_begin(&(snet_fetch_options_t){
		.method = SNET_FETCH_POST,
		.host = snet->config.host,
		.port = snet->config.port,
		.path = snet_printf(env, "%s%s", snet->config.path, "/game/create"),
		.verify_tls = !snet->config.insecure_tls,

		.headers = (snet_fetch_header_t[]){
			snet_auth_header(env, snet),
			{ 0 }
		},

		.content = req_body, .content_length = slen(req_body),
	});

	snet_fetch_status_t fetch_status;
	while (true) {
		if (snet_task_cancelled(env)) { break; }

		if ((fetch_status = snet_fetch_process(fetch)) != SNET_FETCH_PENDING) {
			break;
		}
		snet_task_yield(env);
	}

	snet_log(snet, "fetch status: %d", fetch_status);
	if (fetch_status == SNET_FETCH_FINISHED) {
		int status_code = snet_fetch_status_code(fetch);
		snet_log(snet, "status code: %d", status_code);
		size_t body_size;
		const void* resp_body = snet_fetch_response_body(fetch, &body_size);
		if (status_code == 200) {
			CF_JDoc resp = cf_make_json(resp_body, body_size);
			CF_JVal root = cf_json_get_root(resp);

			snet->lobby_state = SNET_IN_LOBBY;
			snet_task_post(env, &(snet_event_t){
				.type = SNET_EVENT_CREATE_GAME_FINISHED,
				.create_game = {
					.status = SNET_OK,
					.info = {
						.join_token = snet_strcpy(env, cf_json_get_string(cf_json_get(root, "join_token"))),
						.creator = snet_strcpy(env, cf_json_get_string(cf_json_get(root, "creator"))),
						.data = snet_strcpy(env, cf_json_get_string(cf_json_get(root, "data"))),
					}
				},
			});

			cf_destroy_json(resp);
		} else {
			void* body_copy = snet_task_alloc(env, body_size);
			memcpy(body_copy, resp_body, body_size);

			snet->lobby_state = SNET_IN_LOBBY;
			snet_task_post(env, &(snet_event_t){
				.type = SNET_EVENT_CREATE_GAME_FINISHED,
				.create_game = {
					.status = SNET_ERR_REJECTED,
					.error = { .ptr = body_copy, .size = body_size },
				},
			});
		}
	} else {
		snet->lobby_state = SNET_IN_LOBBY;
		snet_task_post(env, &(snet_event_t){
			.type = SNET_EVENT_CREATE_GAME_FINISHED,
			.create_game = { .status = SNET_ERR_IO },
		});
	}

	snet_fetch_end(fetch);
	sfree(req_body);
}

void
snet_create_game(snet_t* snet, const snet_game_options_t* options) {
	snet_task_begin(snet, &snet->create_game_task, snet_task_create_game, options, sizeof(*options));
}

static void
snet_task_join_game(const snet_task_env_t* env) {
	SNET_TASK_ARG(snet_blob_t, join_token);

	snet_t* snet = env->snet;
	snet->lobby_state = SNET_JOINING_GAME;
	snet_log(snet, "Joining game");

#ifndef __EMSCRIPTEN__
	const char* transport = "cute_net";
#else
	const char* transport = "webtransport";
#endif

	snet_fetch_t* fetch = snet_fetch_begin(&(snet_fetch_options_t){
		.method = SNET_FETCH_POST,
		.host = snet->config.host,
		.port = snet->config.port,
		.path = snet_printf(env, "%s%s?transport=%s", snet->config.path, "/game/join", transport),
		.verify_tls = !snet->config.insecure_tls,

		.headers = (snet_fetch_header_t[]){
			snet_auth_header(env, snet),
			{ 0 }
		},

		.content = join_token.ptr, .content_length = join_token.size,
	});

	const void* transport_config = NULL;

	snet_fetch_status_t fetch_status;
	while (true) {
		if (snet_task_cancelled(env)) { break; }

		if ((fetch_status = snet_fetch_process(fetch)) != SNET_FETCH_PENDING) {
			break;
		}
		snet_task_yield(env);
	}

	snet_log(snet, "fetch status: %d", fetch_status);
	if (fetch_status == SNET_FETCH_FINISHED) {
		int status_code = snet_fetch_status_code(fetch);
		snet_log(snet, "status code: %d", status_code);
		size_t body_size;
		const void* resp_body = snet_fetch_response_body(fetch, &body_size);

		if (status_code == 200) {
			transport_config = resp_body;
		} else {
			void* body_copy = snet_task_alloc(env, body_size);
			memcpy(body_copy, resp_body, body_size);

			snet->lobby_state = SNET_IN_LOBBY;
			snet_task_post(env, &(snet_event_t){
				.type = SNET_EVENT_JOIN_GAME_FINISHED,
				.join_game = {
					.status = SNET_ERR_REJECTED,
					.error = { .ptr = body_copy, .size = body_size },
				},
			});
		}
	} else {
		snet->lobby_state = SNET_IN_LOBBY;
		snet_task_post(env, &(snet_event_t){
			.type = SNET_EVENT_JOIN_GAME_FINISHED,
			.join_game = { .status = SNET_ERR_IO },
		});
	}

	if (transport_config != NULL) {
		snet_transport_t* transport = snet_transport_init(transport_config);
		while (true) {
			if (snet_task_cancelled(env)) {
				snet_transport_cleanup(transport);
				break;
			}

			snet_transport_update(transport);

			snet_transport_state_t state = snet_transport_state(transport);
			if (state == SNET_TRANSPORT_CONNECTED) {
				snet->transport = transport;

				snet->lobby_state = SNET_JOINED_GAME;
				snet_task_post(env, &(snet_event_t){
					.type = SNET_EVENT_JOIN_GAME_FINISHED,
					.join_game = { .status = SNET_OK },
				});
				break;
			} else if (state == SNET_TRANSPORT_DISCONNECTED) {
				snet_transport_cleanup(transport);

				snet->lobby_state = SNET_IN_LOBBY;
				snet_task_post(env, &(snet_event_t){
					.type = SNET_EVENT_JOIN_GAME_FINISHED,
					.join_game = { .status = SNET_ERR_IO },
				});
				break;
			}

			snet_task_yield(env);
		}
	}

	snet_fetch_end(fetch);
}

void
snet_join_game(snet_t* snet, snet_blob_t join_token) {
	snet_task_begin(snet, &snet->join_game_task, snet_task_join_game, &join_token, sizeof(join_token));
}

void
snet_send(snet_t* snet, snet_blob_t message, bool reliable) {
	if (snet->transport) {
		snet_transport_send(snet->transport, message.ptr, message.size, reliable);
	}
}

void
snet_exit_game(snet_t* snet) {
	if (snet->transport) {
		snet_transport_cleanup(snet->transport);
		snet->transport = NULL;
	}
}

static void
snet_task_list_games(const snet_task_env_t* env) {
	snet_t* snet = env->snet;
	snet_log(snet, "Listing game");

	snet_fetch_t* fetch = snet_fetch_begin(&(snet_fetch_options_t){
		.method = SNET_FETCH_GET,
		.host = snet->config.host,
		.port = snet->config.port,
		.path = snet_printf(env, "%s%s", snet->config.path, "/game/list"),
		.verify_tls = !snet->config.insecure_tls,

		.headers = (snet_fetch_header_t[]){
			snet_auth_header(env, snet),
			{ 0 }
		},
	});

	snet_fetch_status_t fetch_status;
	while (true) {
		if (snet_task_cancelled(env)) { break; }

		if ((fetch_status = snet_fetch_process(fetch)) != SNET_FETCH_PENDING) {
			break;
		}
		snet_task_yield(env);
	}

	snet_log(snet, "fetch status: %d", fetch_status);
	if (fetch_status == SNET_FETCH_FINISHED) {
		int status_code = snet_fetch_status_code(fetch);
		snet_log(snet, "status code: %d", status_code);
		size_t body_size;
		const void* resp_body = snet_fetch_response_body(fetch, &body_size);

		if (status_code == 200) {
			CF_JDoc doc = cf_make_json(resp_body, body_size);
			CF_JVal resp = cf_json_get_root(doc);
			CF_JVal games = cf_json_get(resp, "games");
			int num_entries = cf_json_get_len(games);
			snet_game_info_t* entries = snet_task_alloc(env, sizeof(snet_game_info_t) * num_entries);
			for (int i = 0; i < num_entries; ++i) {
				CF_JVal entry = cf_json_array_get(games, i);
				entries[i] = (snet_game_info_t){
					.creator = snet_strcpy(env, cf_json_get_string(cf_json_get(entry, "creator"))),
					.join_token = snet_strcpy(env, cf_json_get_string(cf_json_get(entry, "join_token"))),
					.data = snet_strcpy(env, cf_json_get_string(cf_json_get(entry, "data"))),
				};
			}
			cf_destroy_json(doc);

			snet_task_post(env, &(snet_event_t){
				.type = SNET_EVENT_LIST_GAMES_FINISHED,
				.list_games = {
					.status = SNET_OK,
					.num_games = num_entries,
					.games = entries,
				},
			});
		} else {
			void* body_copy = snet_task_alloc(env, body_size);
			memcpy(body_copy, resp_body, body_size);

			snet_task_post(env, &(snet_event_t){
				.type = SNET_EVENT_LIST_GAMES_FINISHED,
				.list_games = {
					.status = SNET_ERR_REJECTED,
				},
			});
		}
	} else {
		snet_task_post(env, &(snet_event_t){
			.type = SNET_EVENT_LIST_GAMES_FINISHED,
			.list_games = { .status = SNET_ERR_IO },
		});
	}

	snet_fetch_end(fetch);
}

void
snet_list_games(snet_t* snet) {
	snet_task_begin(snet, &snet->list_games_task, snet_task_list_games, NULL, 0);
}

#define BLIB_IMPLEMENTATION
#include "barena.h"
