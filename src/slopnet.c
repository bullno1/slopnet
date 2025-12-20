// vim: set foldmethod=marker foldlevel=0:
#include <slopnet.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <cute_json.h>
#include <cute_coroutine.h>
#include <cute_alloc.h>
#include <cute_array.h>
#include <cute_networking.h>
#include <cute_time.h>
#include <SDL3/SDL_misc.h>
#define BARENA_API static
#include "barena.h"
#include "slopnet_fetch.h"
#define WBY_STATIC
#include "wby.h"

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
	snet_task_t list_game_task;

	CF_Client* client;

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
	snet_task_init(snet, &snet->list_game_task);

	return snet;
}

void
snet_cleanup(snet_t* snet) {
	snet_task_cleanup(&snet->auth_task);
	snet_task_cleanup(&snet->create_game_task);
	snet_task_cleanup(&snet->join_game_task);
	snet_task_cleanup(&snet->list_game_task);
	barena_pool_cleanup(&snet->arena_pool);
	if (snet->client) { cf_destroy_client(snet->client); }
	cf_free(snet);
}

void
snet_update(snet_t* snet) {
	snet_task_process(&snet->auth_task);
	snet_task_process(&snet->create_game_task);
	snet_task_process(&snet->join_game_task);
	snet_task_process(&snet->list_game_task);
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

	if ((event = snet_task_reap(&snet->list_game_task)) != NULL) {
		return event;
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

// OAuth {{{

static int
snet_oauth_httpd(struct wby_con* conn, void* userdata) {
	snet_task_env_t* env = userdata;
	snet_t* snet = env->snet;

	if (strcmp(conn->request.uri, "/finish") == 0) {
		char success[2];

		int size = wby_find_query_var(
			conn->request.query_params, "data",
			snet->cookie_buf, SNET_MAX_COOKIE_SIZE
		);
		if (size < 0) { return 1; }
		snet->cookie_buf[size] = '\0';

		if (
			wby_find_query_var(
				conn->request.query_params,
				"success",
				success, sizeof(success)
			) < 1
		) {
			return 1;
		}

		struct wby_header headers[] = {
			{
				.name = "location",
				.value = snet_printf(env, SNET_URL_FMT_PREFIX "/auth/itchio/end", SNET_URL_FMT_PREFIX_ARGS(snet))
			}
		};
		wby_response_begin(conn, 303, 0, headers, sizeof(headers) / sizeof(headers[0]));
		wby_response_end(conn);

		bool succeeded = success[0] == '1';
		snet->auth_state = succeeded ? SNET_AUTHORIZED : SNET_UNAUTHORIZED;
		snet_task_post(env, &(snet_event_t){
			.type = SNET_EVENT_LOGIN_FINISHED,
			.login = {
				.status = succeeded ? SNET_OK : SNET_ERR_REJECTED,
				.data = {
					.ptr = snet->cookie_buf,
					.size = size,
				},
			},
		});
		return 0;
	} else {
		return 1;
	}
}

static void
snet_oauth(const snet_task_env_t* env) {
	SNET_TASK_ARG(const char*, url_template);

	struct wby_config httpd_config = {
		.address = "127.0.0.1",
		.connection_max = 4,
		.request_buffer_size = 1024,
		.io_buffer_size = 1024,
		.dispatch = snet_oauth_httpd,
		.userdata = (void*)env,
	};
	wby_size httpd_memory_size;
	struct wby_server httpd;
	wby_init(&httpd, &httpd_config, &httpd_memory_size);

	(void)wby_find_conn;
	(void)wby_frame_begin;
	(void)wby_frame_end;
	(void)wby_read;

	void* httpd_memory = snet_task_alloc(env, httpd_memory_size);
	if (wby_start(&httpd, httpd_memory) != WBY_OK) {
		return;
	}

	snet_t* snet = env->snet;
	snet->auth_state = SNET_AUTHORIZING;
	const char* url = snet_printf(
		env,
		url_template,
		SNET_URL_FMT_PREFIX_ARGS(env->snet), httpd.config.port
	);
	snet_log(snet, "Opening %s", url);
	SDL_OpenURL(url);

	while (
		!snet_task_cancelled(env)
		&&
		snet->auth_state == SNET_AUTHORIZING
	) {
		wby_update(&httpd, 0);
		snet_task_yield(env);
	}

	wby_stop(&httpd);
}

void
snet_login_with_itchio(snet_t* snet) {
	static const char* url_template = SNET_URL_FMT_PREFIX "/auth/itchio/start?app_port=%d";
	snet_task_begin(snet, &snet->auth_task, snet_oauth, &url_template, sizeof(url_template));
}

/// }}}

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

	snet_fetch_t* fetch = snet_fetch_begin(&(snet_fetch_options_t){
		.method = SNET_FETCH_POST,
		.host = snet->config.host,
		.port = snet->config.port,
		.path = snet_printf(env, "%s%s", snet->config.path, "/game/join"),
		.verify_tls = !snet->config.insecure_tls,

		.headers = (snet_fetch_header_t[]){
			snet_auth_header(env, snet),
			{ 0 }
		},

		.content = join_token.ptr, .content_length = join_token.size,
	});

	const void* connect_token = NULL;

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

		if (status_code == 200 && body_size >= CF_CONNECT_TOKEN_SIZE) {
			connect_token = resp_body;
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

	snet_fetch_end(fetch);

	if (connect_token != NULL) {
		CF_Client* client = cf_make_client(0, 0, false);
		if (cf_is_error(cf_client_connect(client, connect_token))) {
			cf_destroy_client(client);
			snet->lobby_state = SNET_IN_LOBBY;
			snet_task_post(env, &(snet_event_t){
				.type = SNET_EVENT_JOIN_GAME_FINISHED,
				.join_game = { .status = SNET_ERR_IO },
			});
		} else {
			double last_time = CF_SECONDS;
			while (true) {
				if (snet_task_cancelled(env)) {
					cf_destroy_client(client);
					break;
				}

				cf_client_update(client, CF_SECONDS - last_time, time(NULL));
				last_time = CF_SECONDS;

				CF_ClientState state = cf_client_state_get(client);
				if (state == CF_CLIENT_STATE_CONNECTED) {
					snet->client = client;

					snet->lobby_state = SNET_JOINED_GAME;
					snet_task_post(env, &(snet_event_t){
						.type = SNET_EVENT_JOIN_GAME_FINISHED,
						.join_game = { .status = SNET_OK },
					});
					break;
				} else if (state < 0) {
					cf_destroy_client(client);

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
	}
}

void
snet_join_game(snet_t* snet, snet_blob_t join_token) {
	snet_task_begin(snet, &snet->join_game_task, snet_task_join_game, &join_token, sizeof(join_token));
}

#define WBY_IMPLEMENTATION
#include "wby.h"

#define BLIB_IMPLEMENTATION
#include "barena.h"
