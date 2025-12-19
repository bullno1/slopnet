#ifndef SLOPNET_FETCH_H
#define SLOPNET_FETCH_H

#include <stddef.h>
#include <stdbool.h>

typedef struct snet_fetch_s snet_fetch_t;

typedef enum {
	SNET_FETCH_GET,
	SNET_FETCH_POST
} snet_fetch_method_t;

typedef enum {
	SNET_FETCH_PENDING,
	SNET_FETCH_FINISHED,
	SNET_FETCH_ERROR
} snet_fetch_status_t;

typedef struct {
	const char* name;
	const char* value;
} snet_fetch_header_t;

typedef struct {
	snet_fetch_method_t method;

	const char* host;
	const char* path;
	int port;

	snet_fetch_header_t* headers;

	const void* content;
	size_t content_length;

	bool verify_tls;
} snet_fetch_options_t;

snet_fetch_t*
snet_fetch_begin(const snet_fetch_options_t* options);

snet_fetch_status_t
snet_fetch_process(snet_fetch_t* fetch);

int
snet_fetch_status_code(snet_fetch_t* fetch);

const void*
snet_fetch_response_body(snet_fetch_t* fetch, size_t* size);

void
snet_fetch_end(snet_fetch_t* fetch);

#endif
