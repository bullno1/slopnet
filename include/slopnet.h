#ifndef SLOPNET_H
#define SLOPNET_H

#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

typedef struct snet_s snet_t;

typedef struct {
	const char* host;
	const char* path;
	int port;

	void* (*realloc)(void* ptr, size_t size, void* memctx);
	void* memctx;

	void (*log)(const char* fmt, va_list args, void* logctx);
	void* logctx;

	bool insecure_tls;
} snet_config_t;

typedef struct {
	const void* ptr;
	size_t size;
} snet_blob_t;

typedef enum {
	SNET_EVENT_LOGIN_FINISHED,
	SNET_EVENT_CREATE_GAME_FINISHED,
	SNET_EVENT_LIST_GAMES_FINISHED,
	SNET_EVENT_JOIN_GAME_FINISHED,
	SNET_EVENT_EXIT_GAME_FINISHED,
	SNET_EVENT_PLAYER_JOINED,
	SNET_EVENT_PLAYER_LEFT,
	SNET_EVENT_MESSAGE,
	SNET_EVENT_DISCONNECTED,
} snet_event_type_t;

typedef enum {
	SNET_UNAUTHORIZED,
	SNET_AUTHORIZING,
	SNET_AUTHORIZED,
} snet_auth_state_t;

typedef enum {
	SNET_OK,
	SNET_ERR_IO,
	SNET_ERR_REJECTED,
} snet_op_status_t;

typedef struct {
	snet_op_status_t status;
	snet_blob_t data;
} snet_login_result_t;

typedef struct {
	snet_op_status_t status;
	snet_blob_t data;
} snet_create_game_result_t;

typedef struct {
	int id;
	snet_blob_t host;
	snet_blob_t data;
} snet_game_info_t;

typedef struct {
	snet_op_status_t status;
	size_t num_games;
	const snet_game_info_t* games;
} snet_list_games_result_t;

typedef struct {
	snet_op_status_t status;
} snet_join_game_result_t;

typedef struct {
	int id;
	snet_blob_t name;
	snet_blob_t data;
} snet_player_joined_t;

typedef struct {
	int id;
} snet_player_left_t;

typedef struct {
	int sender;
	snet_blob_t data;
} snet_message_t;

typedef enum {
	SNET_DISCONNECT_EXIT,
	SNET_DISCONNECT_KICKED,
	SNET_DISCONNECT_ERR,
} snet_disconnect_reason_t;

typedef struct {
	snet_disconnect_reason_t reason;
} snet_disconnected_t;

typedef struct {
	snet_event_type_t type;

	union {
		snet_login_result_t login;
		snet_create_game_result_t create_game;
		snet_list_games_result_t list_games;
		snet_join_game_result_t join_game;
		snet_player_joined_t player_joined;
		snet_player_left_t player_left;
		snet_message_t message;
		snet_disconnected_t disconnected;
	};
} snet_event_t;

snet_t*
snet_init(const snet_config_t* config);

void
snet_cleanup(snet_t* snet);

void
snet_update(snet_t* snet);

const snet_event_t*
snet_next_event(snet_t* snet);

snet_auth_state_t
snet_auth_state(snet_t* snet);

void
snet_login_with_cookie(snet_t* snet, snet_blob_t cookie);

void
snet_login_with_steam(snet_t* snet);

void
snet_login_with_itchio(snet_t* snet);

void
snet_create_game(snet_t* snet, int max_players, snet_blob_t data);

void
snet_list_games(snet_t* snet, snet_blob_t filter);

void
snet_join_game(snet_t* snet, int id);

void
snet_exit_game(snet_t* snet);

void
snet_send(snet_t* snet, snet_blob_t message, bool reliable);

#endif
