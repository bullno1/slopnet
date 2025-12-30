#include "slopnet_webtransport.h"
#include "reliable/reliable.h"
#include <string.h>

// These are copied from cute_net
// They should be safe enough
// Howevever, negotiation based on maxDatagramSize might be better
#define SNET_WT_SEQUENCE_BUF_SIZE 32
#define SNET_WT_RESEND_DELAY 0.2
#define SNET_WT_MAX_FRAGMENTS 32
#define SNET_WT_FRAGMENT_ABOVE 1015  // Max datagram size keeps returning 1024
									 // and the reliable header is at most 9 bytes
#define SNET_WT_FRAGMENT_SIZE 1000
#define SNET_WT_MAX_INFLIGHT_RELIABLE_MESSAGES 32

#define SNET_WT_MAX_MESSAGE_SIZE (SNET_WT_MAX_FRAGMENTS * SNET_WT_FRAGMENT_SIZE)

typedef union {
	struct {
		bool reliable: 1;
		uint8_t sequence: 7;
	};
	uint8_t u8;
} snet_wt_reliable_header_t;

typedef struct {
	int size;
	char data[];
} snet_wt_fragment_t;

typedef struct {
	double timestamp;
	uint16_t ack_sequence;
	int num_fragments;
	snet_wt_fragment_t* fragments[SNET_WT_MAX_FRAGMENTS];
} snet_wt_outgoing_reliable_message_t;

typedef struct {
	int length;
	int members[SNET_WT_MAX_INFLIGHT_RELIABLE_MESSAGES];
} snet_wt_index_list_t;

struct snet_wt_s {
	snet_wt_config_t config;
	struct reliable_endpoint_t* endpoint;
	double time;

	snet_wt_reliable_header_t next_outgoing_reliable_header;
	snet_wt_outgoing_reliable_message_t* current_outgoing_reliable_message;
	snet_wt_outgoing_reliable_message_t* outgoing_reliable_messages[SNET_WT_MAX_INFLIGHT_RELIABLE_MESSAGES];
	int num_outgoing_reliable_messages;

	snet_wt_reliable_header_t next_incoming_reliable_header;
	snet_wt_fragment_t* incoming_reliable_messages[SNET_WT_MAX_INFLIGHT_RELIABLE_MESSAGES * 2];

	bool processing;
	int deferred_send_size;
	uint8_t send_buf[SNET_WT_MAX_MESSAGE_SIZE];
};

static inline void*
snet_wt_malloc(const snet_wt_config_t* config, size_t size) {
	return config->realloc(NULL, size, config->ctx);
}

static inline void
snet_wt_free(const snet_wt_config_t* config, void* ptr) {
	config->realloc(ptr, 0, config->ctx);
}

static void
snet_wt_reliable_transmit(void* ctx, uint64_t id, uint16_t sequence, const uint8_t* packet_data, int packet_bytes) {
	snet_wt_t* swt = ctx;
	if (swt->current_outgoing_reliable_message != NULL) {  // Store for retransmission
		snet_wt_outgoing_reliable_message_t* msg = swt->current_outgoing_reliable_message;

		snet_wt_fragment_t* frag = snet_wt_malloc(&swt->config, sizeof(snet_wt_fragment_t) + packet_bytes);
		frag->size = packet_bytes;
		memcpy(frag->data, packet_data, packet_bytes);

		msg->fragments[msg->num_fragments++] = frag;
	}

	swt->config.send(packet_data, packet_bytes, swt->config.ctx);
}

static int
snet_wt_reliable_process(void* ctx, uint64_t id, uint16_t sequence, const uint8_t* packet_data, int packet_bytes) {
	if (packet_bytes == 0) { return 0; }

	snet_wt_t* swt = ctx;

	if (packet_data[0] == 0) {  // Unreliable
		swt->config.process(packet_data + 1, packet_bytes - 1, swt->config.ctx);
	} else {  // Reliable
		int num_slots = sizeof(swt->incoming_reliable_messages) / sizeof(swt->incoming_reliable_messages[0]);

		uint8_t sequence = packet_data[0] & ((uint8_t)0x7f);
		if (sequence == swt->next_incoming_reliable_header.sequence) {  // We are waiting for this
			// Immediately deliver
			swt->config.process(packet_data + 1, packet_bytes - 1, swt->config.ctx);
			swt->next_incoming_reliable_header.sequence += 1;

			// Try to deliver all queued up messages
			while (true) {
				int slot = swt->next_incoming_reliable_header.sequence % num_slots;
				snet_wt_fragment_t* frag = swt->incoming_reliable_messages[slot];
				if (frag != NULL) {
					swt->config.process(frag->data, frag->size, swt->config.ctx);
					snet_wt_free(&swt->config, frag);
					swt->incoming_reliable_messages[slot] = NULL;
					swt->next_incoming_reliable_header.sequence += 1;
				} else {
					break;
				}
			}
		} else {  // Queue it for later delivery
			int slot = sequence % num_slots;
			snet_wt_fragment_t* frag = swt->incoming_reliable_messages[slot];
			if (frag == NULL) {  // Not yet stored, could be a redundant retransmission
				frag = snet_wt_malloc(&swt->config, sizeof(snet_wt_fragment_t) + packet_bytes - 1);
				frag->size = packet_bytes - 1;
				memcpy(frag->data, packet_data + 1, packet_bytes - 1);
				swt->incoming_reliable_messages[slot] = frag;
			}
		}
	}

	return 1;  // Should ack
}

static void*
snet_wt_reliable_allocate(void* ctx, size_t size) {
	snet_wt_t* swt = ctx;
	return snet_wt_malloc(&swt->config, size);
}

static void
snet_wt_reliable_free(void* ctx, void* ptr) {
	snet_wt_t* swt = ctx;
	snet_wt_free(&swt->config, ptr);
}

static void
snet_wt_cleanup_reliable_message(snet_wt_t* swt, snet_wt_outgoing_reliable_message_t* msg) {
	for (int i = 0; i < msg->num_fragments; ++i) {
		snet_wt_free(&swt->config, msg->fragments[i]);
	}
	snet_wt_free(&swt->config, msg);
}

static void
snet_wt_flush_deferred_send(snet_wt_t* swt) {
	if (swt->deferred_send_size > 0) {
		reliable_endpoint_send_packet(swt->endpoint, swt->send_buf, swt->deferred_send_size);
		swt->deferred_send_size = 0;
	}
}

static void
snet_wt_maybe_send(snet_wt_t* swt, const void* buf, int size) {
	if (swt->processing) {
		// If this send is made right inside a processing call as a response,
		// defer sending for a bit so we can ack the same message it is responding
		// to
		swt->deferred_send_size = size;
	} else {
		reliable_endpoint_send_packet(swt->endpoint, buf, size);
	}
}

snet_wt_t*
snet_wt_init(const snet_wt_config_t* config, double time) {
	snet_wt_t* swt = snet_wt_malloc(config, sizeof(snet_wt_t));
	swt->config = *config;
	swt->time = time;

	swt->next_outgoing_reliable_header.u8 = 0;
	swt->current_outgoing_reliable_message = NULL;
	swt->num_outgoing_reliable_messages = 0;
	memset(swt->outgoing_reliable_messages, 0, sizeof(swt->outgoing_reliable_messages));

	swt->next_incoming_reliable_header.u8 = 0;
	memset(swt->incoming_reliable_messages, 0, sizeof(swt->incoming_reliable_messages));

	swt->deferred_send_size = 0;
	swt->processing = false;

	struct reliable_config_t endpoint_conf;
	reliable_default_config(&endpoint_conf);
	endpoint_conf.max_packet_size = SNET_WT_MAX_MESSAGE_SIZE;
	endpoint_conf.fragment_above = SNET_WT_FRAGMENT_ABOVE;
	endpoint_conf.max_fragments = SNET_WT_MAX_FRAGMENTS;
	endpoint_conf.fragment_size = SNET_WT_FRAGMENT_SIZE;
	endpoint_conf.transmit_packet_function = snet_wt_reliable_transmit;
	endpoint_conf.process_packet_function = snet_wt_reliable_process;
	endpoint_conf.allocate_function = snet_wt_reliable_allocate;
	endpoint_conf.free_function = snet_wt_reliable_free;
	endpoint_conf.allocator_context = swt;
	endpoint_conf.context = swt;
	swt->endpoint = reliable_endpoint_create(&endpoint_conf, time);

	return swt;
}

void
snet_wt_cleanup(snet_wt_t* swt) {
	reliable_endpoint_destroy(swt->endpoint);

	snet_wt_config_t config = swt->config;

	for (int i = 0; i < swt->num_outgoing_reliable_messages; ++i) {
		snet_wt_cleanup_reliable_message(swt, swt->outgoing_reliable_messages[i]);
	}

	int num_slots = sizeof(swt->incoming_reliable_messages) / sizeof(swt->incoming_reliable_messages[0]);
	for (int i = 0; i < num_slots; ++i) {
		snet_wt_fragment_t* frag = swt->incoming_reliable_messages[i];
		snet_wt_free(&config, frag);
	}

	snet_wt_free(&config, swt);
}

bool
snet_wt_send(snet_wt_t* swt, const void* message, size_t size, bool reliable) {
	snet_wt_flush_deferred_send(swt);

	if (reliable) {
		if (size > SNET_WT_MAX_MESSAGE_SIZE - 1) { return false; }

		if (swt->num_outgoing_reliable_messages >= SNET_WT_MAX_INFLIGHT_RELIABLE_MESSAGES) {
			return false;
		}

		// Allocate a record to store fragments for retransmission
		swt->current_outgoing_reliable_message = snet_wt_malloc(&swt->config, sizeof(snet_wt_outgoing_reliable_message_t));
		swt->outgoing_reliable_messages[swt->num_outgoing_reliable_messages++] = swt->current_outgoing_reliable_message;
		swt->current_outgoing_reliable_message->timestamp = swt->time;
		swt->current_outgoing_reliable_message->ack_sequence = reliable_endpoint_next_packet_sequence(swt->endpoint);
		swt->current_outgoing_reliable_message->num_fragments = 0;

		// Get the header
		snet_wt_reliable_header_t header = swt->next_outgoing_reliable_header;
		swt->next_outgoing_reliable_header.sequence += 1;

		// Build the message
		swt->send_buf[0] = header.sequence | ((uint8_t)0x80);
		memcpy(&swt->send_buf[1], message, size);

		// Send the message
		snet_wt_maybe_send(swt, swt->send_buf, (int)(size + 1));
		return true;
	} else {
		if (size > SNET_WT_MAX_MESSAGE_SIZE - 1) { return false; }

		swt->send_buf[0] = 0;  // Unreliable
		memcpy(&swt->send_buf[1], message, size);

		swt->current_outgoing_reliable_message = NULL;  // Don't store fragments
		snet_wt_maybe_send(swt, swt->send_buf, (int)(size + 1));
		return true;
	}
}

void
snet_wt_process_incoming(snet_wt_t* swt, const void* packet, size_t size) {
	swt->processing = true;
	reliable_endpoint_receive_packet(swt->endpoint, packet, (int)size);
	swt->processing = false;
	snet_wt_flush_deferred_send(swt);

	// Check acks for reliable messages
	int num_acks;
	uint16_t* acks = reliable_endpoint_get_acks(swt->endpoint, &num_acks);
	for (
		int msg_index = 0;
		msg_index < swt->num_outgoing_reliable_messages;
	) {
		snet_wt_outgoing_reliable_message_t* msg = swt->outgoing_reliable_messages[msg_index];

		bool acked = false;
		for (int ack_index = 0; ack_index < num_acks; ++ack_index) {
			if (msg->ack_sequence == acks[ack_index]) {
				acked = true;
				break;
			}
		}

		if (acked) {
			// Delete the current message
			snet_wt_cleanup_reliable_message(swt, msg);
			swt->outgoing_reliable_messages[msg_index] = swt->outgoing_reliable_messages[--swt->num_outgoing_reliable_messages];
		} else {
			++msg_index;
		}
	}
	reliable_endpoint_clear_acks(swt->endpoint);
}

void
snet_wt_update(snet_wt_t* swt, double time) {
	reliable_endpoint_update(swt->endpoint, time);
	swt->time = time;

	// Resend unacked messages
	for (
		int msg_index = 0;
		msg_index < swt->num_outgoing_reliable_messages;
		++msg_index
	) {
		snet_wt_outgoing_reliable_message_t* msg = swt->outgoing_reliable_messages[msg_index];
		if ((time - msg->timestamp) >= SNET_WT_RESEND_DELAY) {
			for (
				int frag_index = 0;
				frag_index < msg->num_fragments;
				++frag_index
			) {
				snet_wt_fragment_t* frag = msg->fragments[frag_index];
				swt->config.send(frag->data, frag->size, swt->config.ctx);
			}

			msg->timestamp = time;
		}
	}
}
