#include "slopnet_transport.h"

#ifndef __EMSCRIPTEN__

#include <cute_networking.h>
#include <cute_alloc.h>
#include <cute_time.h>
#include <time.h>

struct snet_transport_s {
	CF_Client* client;
	double last_update;
	void* last_packet;
};

snet_transport_t*
snet_transport_init(const char* configuration) {
	CF_Client* client = cf_make_client(0, 0, false);
	cf_client_connect(client, (const uint8_t*)configuration);

	snet_transport_t* transport = cf_alloc(sizeof(snet_transport_t));
	*transport = (snet_transport_t){
		.client = client,
	};
	return transport;
}

void
snet_transport_cleanup(snet_transport_t* transport) {
	cf_client_disconnect(transport->client);
	if (transport->last_packet) {
		cf_client_free_packet(transport->client, transport->last_packet);
		transport->last_packet = NULL;
	}
	cf_destroy_client(transport->client);
	cf_free(transport);
}

void
snet_transport_update(snet_transport_t* transport) {
	cf_client_update(transport->client, CF_SECONDS - transport->last_update, time(NULL));
	transport->last_update = CF_SECONDS;
}

snet_transport_state_t
snet_transport_state(snet_transport_t* transport) {
	CF_ClientState state = cf_client_state_get(transport->client);
	if (state == CF_CLIENT_STATE_DISCONNECTED || state < 0) {
		return SNET_TRANSPORT_DISCONNECTED;
	} else if (state == CF_CLIENT_STATE_CONNECTED) {
		return SNET_TRANSPORT_CONNECTED;
	} else {
		return SNET_TRANSPORT_CONNECTING;
	}
}

bool
snet_transport_recv(snet_transport_t* transport, const void** message, size_t* size) {
	if (transport->last_packet) {
		cf_client_free_packet(transport->client, transport->last_packet);
		transport->last_packet = NULL;
	}

	bool reliable;
	int sizei;
	if (cf_client_pop_packet(transport->client, &transport->last_packet, &sizei, &reliable)) {
		*message = transport->last_packet;
		*size = sizei;
		return true;
	} else {
		*message = NULL;
		*size = 0;
		return false;
	}
}

void
snet_transport_send(snet_transport_t* transport, const void* message, size_t size, bool reliable) {
	cf_client_send(transport->client, message, size, reliable);
}

size_t
snet_transport_max_message_size(void) {
	return 1100 * 4;
}

#else

#include <stdlib.h>
#include <cute_time.h>
#include <cute_array.h>
#include <emscripten.h>
#include "slopnet_webtransport.h"

typedef struct {
	size_t size;
	char data[];
} snet_message_t;

struct snet_transport_s {
	snet_message_t* last_incoming_message;
	int handle;
	snet_wt_t* wt;

	int next_message;
	dyna snet_message_t** incoming_messages;

	char recv_buf[SNET_WT_RECV_BUF_SIZE];
};

extern int
snet_transport_impl_connect(const char* configuration, void* recv_buf, void* ctx);

extern void
snet_transport_impl_disconnect(int handle);

extern int
snet_transport_impl_state(int handle);

extern int
snet_transport_impl_max_datagram_size(int handle);

extern void
snet_transport_impl_send(int handle, const void* message, size_t size);

static void*
snet_wt_realloc_callback(void* ptr, size_t size, void* ctx) {
	if (size == 0) {
		free(ptr);
		return NULL;
	} else {
		void* mem = realloc(ptr, size);
		return mem;
	}
}

static void
snet_wt_send_callback(const void* message, size_t size, void* ctx) {
	snet_transport_t* transport = ctx;
	snet_transport_impl_send(transport->handle, message, size);
}

static void
snet_wt_process_callback(const void* message, size_t size, void* ctx) {
	snet_transport_t* transport = ctx;

	snet_message_t* msg = malloc(sizeof(snet_message_t) + size);
	msg->size = size;
	memcpy(msg->data, message, size);

	apush(transport->incoming_messages, msg);
}

EMSCRIPTEN_KEEPALIVE void
snet_transport_process_incoming(void* ctx, size_t size) {
	snet_transport_t* transport = ctx;
	snet_wt_process_incoming(transport->wt, transport->recv_buf, size);
}

snet_transport_t*
snet_transport_init(const char* configuration) {
	snet_transport_t* transport = malloc(sizeof(snet_transport_t));
	*transport = (snet_transport_t){
		.handle = snet_transport_impl_connect(configuration, &transport->recv_buf[0], transport),
	};

	snet_wt_config_t wt_config = {
		.ctx = transport,
		.send = snet_wt_send_callback,
		.realloc = snet_wt_realloc_callback,
		.process = snet_wt_process_callback,
	};
	transport->wt = snet_wt_init(&wt_config, CF_SECONDS);

	return transport;
}

void
snet_transport_cleanup(snet_transport_t* transport) {
	snet_transport_impl_disconnect(transport->handle);

	free(transport->last_incoming_message);

	if (transport->incoming_messages) {
		for (int i = 0 ; i < alen(transport->incoming_messages); ++i) {
			free(transport->incoming_messages[i]);
		}
		afree(transport->incoming_messages);
	}

	snet_wt_cleanup(transport->wt);

	free(transport);
}

void
snet_transport_update(snet_transport_t* transport) {
	snet_wt_update(transport->wt, CF_SECONDS);
}

snet_transport_state_t
snet_transport_state(snet_transport_t* transport) {
	return snet_transport_impl_state(transport->handle);
}

bool
snet_transport_recv(snet_transport_t* transport, const void** message, size_t* size) {
	free(transport->last_incoming_message);
	transport->last_incoming_message = NULL;

	if (
		transport->incoming_messages
		&&
		transport->next_message < alen(transport->incoming_messages)
	) {  // There is at least a message
		transport->last_incoming_message = transport->incoming_messages[transport->next_message];
		transport->incoming_messages[transport->next_message] = NULL;
		++transport->next_message;

		*message = transport->last_incoming_message->data;
		*size = transport->last_incoming_message->size;
		return true;
	} else {
		if (transport->next_message > 0) {  // We reached the end of the queue
			transport->next_message = 0;
			aclear(transport->incoming_messages);
		}
		return false;
	}
}

void
snet_transport_send(snet_transport_t* transport, const void* message, size_t size, bool reliable) {
	snet_wt_send(transport->wt, message, size, reliable);
}

size_t
snet_transport_max_message_size(void) {
	return 1000 * 4;
}

#endif
