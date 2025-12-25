#include "slopnet_oauth.h"

static void*
snet_oauth_alloc(size_t size, size_t alignment, const snet_oauth_config_t* config) {
	return config->alloc(size, alignment, config->memctx);
}

#ifndef __EMSCRIPTEN__

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <SDL3/SDL_misc.h>

#define WBY_STATIC
#include "wby.h"

struct snet_oauth_s {
	snet_oauth_state_t state;
	snet_oauth_config_t config;
	struct wby_server httpd;
	char data_buf[1024];
	size_t data_size;
};

static const char*
snet_oauth_vprintf(const snet_oauth_config_t* config, const char* fmt, va_list args) {
	va_list args_copy;
	va_copy(args_copy, args);
	int size = vsnprintf(NULL, 0, fmt, args);
	char* buf = snet_oauth_alloc(size + 1, _Alignof(char), config);
	vsnprintf(buf, size + 1, fmt, args_copy);
	va_end(args_copy);
	return buf;
}

static const char*
snet_oauth_printf(const snet_oauth_config_t* config, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	const char* result = snet_oauth_vprintf(config, fmt, args);
	va_end(args);
	return result;
}

static int
snet_oauth_httpd(struct wby_con* conn, void* userdata) {
	snet_oauth_t* oauth = userdata;

	if (strcmp(conn->request.uri, "/oauth_callback") == 0) {
		char success[2];

		int size = wby_find_query_var(
			conn->request.query_params, "data",
			oauth->data_buf, sizeof(oauth->data_buf)
		);
		if (size < 0) { return 1; }
		oauth->data_buf[size] = '\0';
		oauth->data_size = size;

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
				.value = oauth->config.end_url,
			}
		};
		wby_response_begin(conn, 303, 0, headers, sizeof(headers) / sizeof(headers[0]));
		wby_response_end(conn);

		bool succeeded = success[0] == '1';
		oauth->state = succeeded ? SNET_OAUTH_SUCCESS : SNET_OAUTH_FAILED;
		return 0;
	} else {
		return 1;
	}
}

snet_oauth_t*
snet_oauth_begin(const snet_oauth_config_t* config) {
	snet_oauth_t* oauth = snet_oauth_alloc(sizeof(snet_oauth_t), _Alignof(snet_oauth_t), config);
	*oauth = (snet_oauth_t){
		.state = SNET_OAUTH_PENDING,
		.config = *config,
	};

	struct wby_config httpd_config = {
		.address = "127.0.0.1",
		.connection_max = 4,
		.request_buffer_size = 1024,
		.io_buffer_size = 1024,
		.dispatch = snet_oauth_httpd,
		.userdata = oauth,
	};
	wby_size httpd_memory_size;
	wby_init(&oauth->httpd, &httpd_config, &httpd_memory_size);
	(void)wby_find_conn;
	(void)wby_frame_begin;
	(void)wby_frame_end;
	(void)wby_read;

	void* httpd_memory = snet_oauth_alloc(httpd_memory_size, _Alignof(double), config);
	if (wby_start(&oauth->httpd, httpd_memory) != WBY_OK) {
		oauth->state = SNET_OAUTH_FAILED;
	}

	const char* url = snet_oauth_printf(
		config,
		"%s?origin=http://localhost:%d", config->start_url, oauth->httpd.config.port
	);
	SDL_OpenURL(url);

	return oauth;
}

snet_oauth_state_t
snet_oauth_update(snet_oauth_t* oauth) {
	wby_update(&oauth->httpd, 0);
	return oauth->state;
}

const void*
snet_oauth_data(snet_oauth_t* oauth, size_t* size) {
	if (oauth->data_size > 0) {
		*size = oauth->data_size;
		return oauth->data_buf;
	} else {
		return NULL;
	}
}

void
snet_oauth_end(snet_oauth_t* oauth) {
	wby_stop(&oauth->httpd);
}

#define WBY_IMPLEMENTATION
#include "wby.h"

#else

struct snet_oauth_s {
	snet_oauth_config_t config;
};

extern void
snet_oauth_begin_js(const char* url);

extern size_t
snet_oauth_data_size(void);

extern void
snet_oauth_copy_data(void* ptr, size_t max_size);

snet_oauth_t*
snet_oauth_begin(const snet_oauth_config_t* config) {
	snet_oauth_t* oauth = snet_oauth_alloc(sizeof(snet_oauth_t), _Alignof(snet_oauth_t), config);
	*oauth = (snet_oauth_t){
		.config = *config,
	};

	snet_oauth_begin_js(config->start_url);
	return oauth;
}

const void*
snet_oauth_data(snet_oauth_t* oauth, size_t* size) {
	*size =  snet_oauth_data_size();
	if (*size == 0) { return NULL; }

	void* data = snet_oauth_alloc(*size + 1, _Alignof(char), &oauth->config);
	snet_oauth_copy_data(data, *size + 1);
	return data;
}

void
snet_oauth_end(snet_oauth_t* oauth) {
}

#endif
