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
		return true;
	} else {
		return false;
	}
}

void
snet_transport_send(snet_transport_t* transport, const void* message, size_t size, bool reliable) {
	cf_client_send(transport->client, message, size, reliable);
}

#else

snet_transport_t*
snet_transport_init(const char* configuration) {
	return NULL;
}

void
snet_transport_cleanup(snet_transport_t* transport) {
}

void
snet_transport_update(snet_transport_t* transport) {
}

snet_transport_state_t
snet_transport_state(snet_transport_t* transport) {
	return 0;
}

bool
snet_transport_recv(snet_transport_t* transport, const void** message, size_t* size) {
	return false;
}

void
snet_transport_send(snet_transport_t* transport, const void* message, size_t size, bool reliable) {
}

#endif
