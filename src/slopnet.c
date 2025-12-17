#include <slopnet.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <cute_https.h>
#include <cute_json.h>

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
	snet_blob_t login_data;
	snet_blob_t cookie;

	char* login_body;

	CF_HttpsRequest login_req;
	CF_HttpsResult login_result;
	snet_op_state_t login_state;
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
snet_path(snet_t* snet, const char* path) {
	int size = snprintf(snet->path_buf.ptr, snet->path_buf.capacity, "%s/%s", snet->config.path, path);
	if ((size_t)size > snet->path_buf.capacity) {
		snet_buf_ensure(snet, &snet->path_buf, size + 1);
		snprintf(snet->path_buf.ptr, snet->path_buf.capacity, "%s/%s", snet->config.path, path);
	}

	return snet->path_buf.ptr;
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

	return snet;
}

void
snet_cleanup(snet_t* snet) {
	snet_free(snet, snet->path_buf.ptr);
	snet_free(snet, snet->login_buf.ptr);
	snet_free(snet, snet->cookie_buf.ptr);
	sfree(snet->login_body);
	snet_free(snet, snet);
}

void
snet_update(snet_t* snet) {
	if (
		snet->login_state == SNET_OP_PENDING
		&&
		((snet->login_result = cf_https_process(snet->login_req)) != CF_HTTPS_RESULT_PENDING)
	) {
		snet->login_state = SNET_OP_FINISHED;
	}
}

const snet_event_t*
snet_next_event(snet_t* snet) {
	if (snet->login_state == SNET_OP_FINISHED) {
		CF_HttpsResponse resp = cf_https_response(snet->login_req);
		snet_op_status_t status = SNET_ERR_IO;
		snet->auth_state = SNET_UNAUTHORIZED;
		if (snet->login_result == CF_HTTPS_RESULT_OK) {
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
		cf_https_destroy(snet->login_req);
		snet->login_req.id = 0;
		snet->login_state = SNET_OP_RETURNED;
		sfree(snet->login_body);

		return snet_event(snet, &(snet_event_t){
			.type = SNET_EVENT_LOGIN_FINISHED,
			.login = {
				.status = status,
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
	if (snet->login_state != SNET_OP_RETURNED) {
		cf_https_destroy(snet->login_req);
	}
}

void
snet_login_with_userpass(snet_t* snet, snet_blob_t username, snet_blob_t password) {
	if (snet->login_state != SNET_OP_RETURNED) {
		cf_https_destroy(snet->login_req);
		sfree(snet->login_body);
	}

	CF_JDoc body = cf_make_json(NULL, 0);
	CF_JVal root = cf_json_object(body);
	cf_json_set_root(body, root);
	cf_json_object_add_string_range(body, root, "username", username.ptr, (char*)username.ptr + username.size);
	cf_json_object_add_string_range(body, root, "password", password.ptr, (char*)password.ptr + password.size);
	snet->login_body = cf_json_to_string_minimal(body);
	cf_destroy_json(body);

	snet->login_state = SNET_OP_PENDING;
	snet->login_req = cf_https_post(
		snet->config.host, snet->config.port,
		snet_path(snet, "login/userpass"),
		snet->login_body, strlen(snet->login_body),
		!snet->config.insecure_tls
	);
	snet->auth_state = SNET_AUTHORIZING;
}
