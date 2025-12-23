#ifndef SLOPNET_OAUTH_H
#define SLOPNET_OAUTH_H

#include <stddef.h>

typedef struct snet_oauth_s snet_oauth_t;

typedef enum {
	SNET_OAUTH_PENDING,
	SNET_OAUTH_SUCCESS,
	SNET_OAUTH_FAILED,
} snet_oauth_state_t;

typedef struct {
	const char* start_url;
	const char* end_url;

	void* (*alloc)(size_t size, size_t alignment, void* memctx);
	void* memctx;
} snet_oauth_config_t;

snet_oauth_t*
snet_oauth_begin(const snet_oauth_config_t* config);

snet_oauth_state_t
snet_oauth_update(snet_oauth_t* oauth);

const void*
snet_oauth_data(snet_oauth_t* oauth, size_t* size);

void
snet_oauth_end(snet_oauth_t* oauth);

#endif
