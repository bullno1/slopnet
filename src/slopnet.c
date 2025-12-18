#include <slopnet.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <cute_https.h>
#include <cute_json.h>
#include <SDL3/SDL_misc.h>
#define WBY_STATIC
#include "wby.h"

#define SNET_URL_FMT_PREFIX "https://%s:%d%s"
#define SNET_URL_FMT_PREFIX_ARGS(snet) (snet)->config.host, (snet)->config.port, (snet)->config.path
#define SNET_MAX_COOKIE_SIZE 2048

typedef struct {
	void* ptr;
	size_t capacity;
} snet_buf_t;

typedef enum {
	SNET_OP_RETURNED,
	SNET_OP_PENDING,
	SNET_OP_FINISHED,
} snet_op_state_t;

struct snet_s {
	snet_config_t config;

	snet_auth_state_t auth_state;
	snet_event_t current_event;

	snet_buf_t path_buf;
	snet_buf_t login_buf;
	snet_buf_t cookie_buf;
	snet_buf_t tmp_url_buf;
	snet_blob_t login_data;
	snet_blob_t cookie;

	char* login_body;

	struct wby_server httpd;
	void* httpd_memory;

	CF_HttpsRequest http_login_req;
	CF_HttpsResult http_login_result;
	snet_op_state_t http_login_state;

	snet_op_state_t oauth_login_state;
	snet_op_status_t oauth_login_status;
};

static inline void* snet_realloc(snet_t* snet, void* ptr, size_t size) {
	return snet->config.realloc(ptr, size, snet->config.memctx);
}

static inline void*
snet_free(snet_t* snet, void* ptr) {
	return snet_realloc(snet, ptr, 0);
}

static inline void*
snet_buf_ensure(snet_t* snet, snet_buf_t* buf, size_t capacity) {
	if (capacity > buf->capacity) {
		buf->ptr = snet_realloc(snet, buf->ptr, capacity);
		buf->capacity = capacity;
	}

	return buf->ptr;
}

static inline const char*
snet_vprintf(snet_t* snet, snet_buf_t* buf, const char* fmt, va_list args) {
	va_list args_copy;
	va_copy(args_copy, args);
	size_t size = (size_t)vsnprintf(buf->ptr, buf->capacity, fmt, args);
	if (size > buf->capacity) {
		snet_buf_ensure(snet, buf, size + 1);
		vsnprintf(buf->ptr, size + 1, fmt, args_copy);
	}
	va_end(args_copy);
	return buf->ptr;
}

static inline const char*
snet_url(snet_t* snet, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	const char* result = snet_vprintf(snet, &snet->tmp_url_buf, fmt, args);
	va_end(args);
	return result;
}

static inline snet_blob_t
snet_response_body(snet_t* snet, CF_HttpsResponse resp, snet_buf_t* buf) {
	int size = cf_https_response_content_length(resp);
	memcpy(
		snet_buf_ensure(snet, buf, size),
		cf_https_response_content(resp),
		size
	);
	return (snet_blob_t){
		.ptr = buf->ptr,
		.size = size,
	};
}

static inline const snet_event_t*
snet_event(snet_t* snet, const snet_event_t* event) {
	snet->current_event = *event;
	return &snet->current_event;
}

static inline void*
snet_stdlib_realloc(void* ptr, size_t size, void* memctx) {
	if (size != 0) {
		return realloc(ptr, size);
	} else {
		free(ptr);
		return NULL;
	}
}

static int
snet_httpd(struct wby_con* conn, void* userdata) {
	snet_t* snet = userdata;
	if (strcmp(conn->request.uri, "/succeeded") == 0) {
		int size = wby_find_query_var(
			conn->request.query_params, "data",
			snet_buf_ensure(snet, &snet->cookie_buf, SNET_MAX_COOKIE_SIZE), SNET_MAX_COOKIE_SIZE
		);
		if (size < 0) { return 1; }
		((char*)snet->cookie_buf.ptr)[size] = '\0';

		snet->cookie.ptr = snet->cookie_buf.ptr;
		snet->cookie.size = snet->login_data.size;

		struct wby_header headers[] = {
			{
				.name = "location",
				.value = snet_url(snet, SNET_URL_FMT_PREFIX "/auth/itchio/end", SNET_URL_FMT_PREFIX_ARGS(snet))
			}
		};
		wby_response_begin(conn, 303, 0, headers, sizeof(headers) / sizeof(headers[0]));
		wby_response_end(conn);
		snet->oauth_login_state = SNET_OP_FINISHED;
		snet->oauth_login_status = SNET_OK;
		snet->login_data.ptr = snet->cookie_buf.ptr;
		snet->login_data.size = size;
		snet->auth_state = SNET_AUTHORIZED;
		return 0;
	} else if (strcmp(conn->request.uri, "/failed") == 0) {
		int size = wby_find_query_var(
			conn->request.query_params, "data",
			snet_buf_ensure(snet, &snet->login_buf, SNET_MAX_COOKIE_SIZE), SNET_MAX_COOKIE_SIZE
		);
		if (size < 0) { return 1; }

		struct wby_header headers[] = {
			{
				.name = "location",
				.value = snet_url(snet, SNET_URL_FMT_PREFIX "/auth/itchio/end", SNET_URL_FMT_PREFIX_ARGS(snet))
			}
		};
		wby_response_begin(conn, 303, 0, headers, sizeof(headers) / sizeof(headers[0]));
		wby_response_end(conn);
		snet->oauth_login_state = SNET_OP_FINISHED;
		snet->oauth_login_status = SNET_ERR_REJECTED;
		snet->login_data.ptr = snet->login_buf.ptr;
		snet->login_data.size = size;
		snet->auth_state = SNET_UNAUTHORIZED;
		return 0;
	} else {
		return 1;
	}
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

	if (config.realloc == NULL) {
		config.realloc = snet_stdlib_realloc;
	}

	snet_t* snet = config.realloc(NULL, sizeof(snet_t), config.memctx);
	*snet = (snet_t){
		.config = config,
	};

	struct wby_config httpd_config = {
		.address = "127.0.0.1",
		.connection_max = 4,
		.request_buffer_size = 4096,
		.io_buffer_size = 4096,
		.dispatch = snet_httpd,
		.userdata = snet,
	};
	wby_size httpd_memory_size;
	wby_init(&snet->httpd, &httpd_config, &httpd_memory_size);
	snet->httpd_memory = snet_realloc(snet, snet->httpd_memory, httpd_memory_size);
	(void)wby_find_conn;
	(void)wby_frame_begin;
	(void)wby_frame_end;
	(void)wby_read;

	return snet;
}

void
snet_cleanup(snet_t* snet) {
	snet_free(snet, snet->path_buf.ptr);
	snet_free(snet, snet->login_buf.ptr);
	snet_free(snet, snet->cookie_buf.ptr);
	snet_free(snet, snet->tmp_url_buf.ptr);
	snet_free(snet, snet->httpd_memory);
	sfree(snet->login_body);
	snet_free(snet, snet);
}

void
snet_update(snet_t* snet) {
	if (
		snet->http_login_state == SNET_OP_PENDING
		&&
		((snet->http_login_result = cf_https_process(snet->http_login_req)) != CF_HTTPS_RESULT_PENDING)
	) {
		snet->http_login_state = SNET_OP_FINISHED;
	}

	if (
		snet->oauth_login_state == SNET_OP_PENDING
	) {
		wby_update(&snet->httpd, 0);
	}
}

const snet_event_t*
snet_next_event(snet_t* snet) {
	if (snet->http_login_state == SNET_OP_FINISHED) {
		CF_HttpsResponse resp = cf_https_response(snet->http_login_req);
		snet_op_status_t status = SNET_ERR_IO;
		snet->auth_state = SNET_UNAUTHORIZED;
		snet->login_data.size = 0;
		if (snet->http_login_result == CF_HTTPS_RESULT_OK) {
			snet->login_data = snet_response_body(snet, resp, &snet->login_buf);
			status = cf_https_response_code(resp) == 200 ? SNET_OK : SNET_ERR_REJECTED;
			if (status == SNET_OK) {
				snet_buf_ensure(snet, &snet->cookie_buf, snet->login_data.size + 1);
				memcpy(snet->cookie_buf.ptr, snet->login_buf.ptr, snet->login_data.size);
				((uint8_t*)snet->cookie_buf.ptr)[snet->login_data.size] = 0;
				snet->cookie.ptr = snet->cookie_buf.ptr;
				snet->cookie.size = snet->login_data.size;
				snet->auth_state = SNET_AUTHORIZED;
			}
		}
		cf_https_destroy(snet->http_login_req);
		snet->http_login_req.id = 0;
		snet->http_login_state = SNET_OP_RETURNED;
		sfree(snet->login_body);

		return snet_event(snet, &(snet_event_t){
			.type = SNET_EVENT_LOGIN_FINISHED,
			.login = {
				.status = status,
				.data = snet->login_data,
			},
		});
	}

	if (snet->oauth_login_state == SNET_OP_FINISHED) {
		snet->oauth_login_state = SNET_OP_RETURNED;
		wby_stop(&snet->httpd);

		return snet_event(snet, &(snet_event_t){
			.type = SNET_EVENT_LOGIN_FINISHED,
			.login = {
				.status = snet->oauth_login_status,
				.data = snet->login_data,
			},
		});
	}

	return NULL;
}

snet_auth_state_t
snet_auth_state(snet_t* snet) {
	return snet->auth_state;
}

void
snet_login_with_cookie(snet_t* snet, snet_blob_t cookie) {
}

void
snet_login_with_itchio(snet_t* snet) {
	if (snet->auth_state == SNET_AUTHORIZING) { return; }

	snet->httpd.config.port = 0;  // Always use a random port
	if (wby_start(&snet->httpd, snet->httpd_memory) != WBY_OK) {
		return;
	}

	snet->auth_state = SNET_AUTHORIZING;
	snet->oauth_login_state = SNET_OP_PENDING;
	SDL_OpenURL(
		snet_url(
			snet,
			SNET_URL_FMT_PREFIX "/auth/itchio/start?app_port=%d",
			SNET_URL_FMT_PREFIX_ARGS(snet), snet->httpd.config.port
		)
	);
}

#define WBY_IMPLEMENTATION
#include "wby.h"
