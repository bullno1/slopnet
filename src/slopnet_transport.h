#ifndef SLOPNET_TRANSPORT_H
#define SLOPNET_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct snet_transport_s snet_transport_t;

typedef enum {
	SNET_TRANSPORT_DISCONNECTED,
	SNET_TRANSPORT_CONNECTING,
	SNET_TRANSPORT_CONNECTED,
} snet_transport_state_t;

snet_transport_t*
snet_transport_init(const char* configuration);

void
snet_transport_cleanup(snet_transport_t* transport);

size_t
snet_transport_max_message_size(void);

void
snet_transport_update(snet_transport_t* transport);

snet_transport_state_t
snet_transport_state(snet_transport_t* transport);

bool
snet_transport_recv(snet_transport_t* transport, const void** message, size_t* size);

void
snet_transport_send(snet_transport_t* transport, const void* message, size_t size, bool reliable);

#endif
