#ifndef SLOPNET_WEBTRANSPORT_H
#define SLOPNET_WEBTRANSPORT_H

#include <stdbool.h>
#include <stddef.h>

#define SNET_WT_RECV_BUF_SIZE 2048

typedef struct {
	void* ctx;
	void* (*realloc)(void* ptr, size_t size, void* ctx);
	void (*send)(const void* message, size_t size, void* ctx);
	void (*process)(const void* message, size_t size, void* ctx);
} snet_wt_config_t;

typedef struct snet_wt_s snet_wt_t;

snet_wt_t*
snet_wt_init(const snet_wt_config_t* config, double time);

void
snet_wt_cleanup(snet_wt_t* swt);

bool
snet_wt_send(snet_wt_t* swt, const void* message, size_t size, bool reliable);

void
snet_wt_process_incoming(snet_wt_t* swt, const void* packet, size_t size);

void
snet_wt_update(snet_wt_t* swt, double time);

#endif
