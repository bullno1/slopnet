#include "slopnet_fetch.h"

#ifndef __EMSCRIPTEN__

#include <cute_https.h>

snet_fetch_t*
snet_fetch_begin(const snet_fetch_options_t* options) {
	CF_HttpsRequest request = { 0 };
	if (options->method == SNET_FETCH_GET) {
		request = cf_https_get(
			options->host,
			options->port,
			options->path,
			options->verify_tls
		);
	} else if (options->method == SNET_FETCH_POST){
		request = cf_https_post(
			options->host,
			options->port,
			options->path,
			options->content, options->content_length,
			options->verify_tls
		);
	}

	if (request.id != 0) {
		for (int i = 0; options->headers != NULL && options->headers[i].name != NULL; ++i) {
			cf_https_add_header(request, options->headers[i].name, options->headers[i].value);
		}
	}

	return (snet_fetch_t*)request.id;
}

snet_fetch_status_t
snet_fetch_process(snet_fetch_t* fetch) {
	if (fetch == NULL) { return SNET_FETCH_ERROR; }

	CF_HttpsRequest request = { .id = (uintptr_t)fetch };
	CF_HttpsResult result = cf_https_process(request);
	switch (result) {
		case CF_HTTPS_RESULT_PENDING: return SNET_FETCH_PENDING;
		case CF_HTTPS_RESULT_OK: return SNET_FETCH_FINISHED;
		default: return SNET_FETCH_ERROR;
	}
}

int
snet_fetch_status_code(snet_fetch_t* fetch) {
	if (fetch == NULL) { return 0; }

	CF_HttpsRequest request = { .id = (uintptr_t)fetch };
	CF_HttpsResponse response = cf_https_response(request);
	return cf_https_response_code(response);
}

const void*
snet_fetch_response_body(snet_fetch_t* fetch, size_t* size) {
	if (fetch == NULL) { return NULL; }

	CF_HttpsRequest request = { .id = (uintptr_t)fetch };
	CF_HttpsResponse response = cf_https_response(request);
	if (size) { *size = cf_https_response_content_length(response); }
	return cf_https_response_content(response);
}

void
snet_fetch_end(snet_fetch_t* fetch) {
	if (fetch == NULL) { return; }

	CF_HttpsRequest request = { .id = (uintptr_t)fetch };
	cf_https_destroy(request);
}

#else
#endif
