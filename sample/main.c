#include <slopnet.h>
#include <cute.h>
#include <dcimgui.h>

static void
snet_log(const char* fmt, va_list args, void* ctx) {
	fprintf(stderr, "snet: ");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
}

#ifdef __EMSCRIPTEN__

#include <emscripten.h>

EM_ASYNC_JS(void, sync_fs, (void), {
	await new Promise((resolve) => {
		FS.syncfs(false, resolve);
	});
})

EM_ASYNC_JS(void, init_fs, (void), {
	FS.mount(IDBFS, {}, "/home/web_user");
	await new Promise((resolve, reject) => {
		FS.syncfs(true, (err) => {
			if (err) { reject(err); } else { resolve(); }
		});
	});
})

#else

static void
init_fs(void) {
}

static void
sync_fs(void) {
}

#endif

int
main(int argc, const char* argv[]) {
	init_fs();

	int options = CF_APP_OPTIONS_WINDOW_POS_CENTERED_BIT | CF_APP_OPTIONS_FILE_SYSTEM_DONT_DEFAULT_MOUNT_BIT;
	if (cf_is_error(cf_make_app("slopnet demo", 0, 0, 0, 800, 600, options, argv[0]))) {
		return 1;
	}

	const char* user_dir = cf_fs_get_user_directory("bullno1", "slopnet-demo");
	cf_fs_set_write_directory(user_dir);
	cf_fs_mount(user_dir, "/user", true);

	cf_app_set_vsync(true);
	cf_app_init_imgui();

	snet_config_t snet_config = {
		.host = "snet-dev.bullno1.com",
		.path = "/snet",
		.log = snet_log,
	};
	snet_t* snet = snet_init(&snet_config);

	size_t cookie_size;
	char* cookie = cf_fs_read_entire_file_to_memory("/user/cookie", &cookie_size);
	if (cookie != NULL && cookie_size > 0) {
		fprintf(stderr, "Logging in with cookie\n");
		snet_login_with_cookie(snet, (snet_blob_t){ .ptr = cookie, .size = cookie_size });
	}

	int num_games = 0;
	const snet_game_info_t* games = NULL;

	while (cf_app_is_running()) {
		cf_app_update(NULL);

		snet_update(snet);
		const snet_event_t* snet_event;
		while ((snet_event = snet_next_event(snet)) != NULL) {
			switch (snet_event->type) {
				case SNET_EVENT_LOGIN_FINISHED:
					if (snet_event->login.status == SNET_OK) {
						fprintf(stderr, "Logged in with token: " SNET_BLOB_FMT "\n", SNET_BLOB_FMT_ARGS(snet_event->login.data));
						cf_fs_write_string_range_to_file("/cookie", (char*)snet_event->login.data.ptr, (char*)snet_event->login.data.ptr + snet_event->login.data.size);
						sync_fs();
					} else if (snet_event->login.status == SNET_ERR_IO) {
						fprintf(stderr, "Network error\n");
					} else if (snet_event->login.status == SNET_ERR_REJECTED) {
						fprintf(stderr, "Logged failed with reason: " SNET_BLOB_FMT "\n", SNET_BLOB_FMT_ARGS(snet_event->login.data));
					}
					break;
				case SNET_EVENT_CREATE_GAME_FINISHED:
					if (snet_event->create_game.status == SNET_OK) {
						fprintf(stderr, "Created game\n");
						fprintf(stderr, "Token: " SNET_BLOB_FMT "\n", SNET_BLOB_FMT_ARGS(snet_event->create_game.info.join_token));
						fprintf(stderr, "Data: " SNET_BLOB_FMT "\n", SNET_BLOB_FMT_ARGS(snet_event->create_game.info.data));

						snet_join_game(snet, snet_event->create_game.info.join_token);
					} else if (snet_event->create_game.status == SNET_ERR_IO) {
						fprintf(stderr, "Network error\n");
					} else if (snet_event->create_game.status == SNET_ERR_REJECTED) {
						fprintf(stderr, "Creating game failed with reason: " SNET_BLOB_FMT "\n", SNET_BLOB_FMT_ARGS(snet_event->create_game.error));
					}
					break;
				case SNET_EVENT_JOIN_GAME_FINISHED:
					if (snet_event->join_game.status == SNET_OK) {
						fprintf(stderr, "Joined game\n");
					} else if (snet_event->join_game.status == SNET_ERR_IO) {
						fprintf(stderr, "Network error\n");
					} else if (snet_event->join_game.status == SNET_ERR_REJECTED) {
						fprintf(stderr, "Join failed with reason: " SNET_BLOB_FMT "\n", SNET_BLOB_FMT_ARGS(snet_event->join_game.error));
					}
					break;
				case SNET_EVENT_DISCONNECTED:
					fprintf(stderr, "Disconnected\n");
					break;
				case SNET_EVENT_LIST_GAMES_FINISHED:
					if (snet_event->list_games.status == SNET_OK) {
						num_games = snet_event->list_games.num_games;
						games = snet_event->list_games.games;
					}
					break;
				case SNET_EVENT_MESSAGE:
					fprintf(stdout, "Received: %.*s\n", (int)snet_event->message.data.size, (const char*)snet_event->message.data.ptr);
					break;
				default:
					break;
			}
		}

		if (ImGui_Begin("Slopnet", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui_Separator();

			switch (snet_auth_state(snet)) {
				case SNET_UNAUTHORIZED: {
					if (ImGui_Button("Login with itch.io")) {
						snet_login_with_itchio(snet);
					}
				} break;
				case SNET_AUTHORIZING: {
					ImGui_LabelText("Status", "Logging in");
				} break;
				case SNET_AUTHORIZED: {
					switch (snet_lobby_state(snet)) {
						case SNET_IN_LOBBY: {
							ImGui_LabelText("Status", "In lobby");

							if (ImGui_Button("Create game")) {
								snet_create_game(snet, &(snet_game_options_t){
									.visibility = SNET_GAME_PUBLIC,
									.max_num_players = 4,
								});
							}

							if (ImGui_Button("Find game")) {
								num_games = 0;
								snet_list_games(snet);
							}

							if (num_games > 0) {
								ImGui_Separator();

								for (int i = 0; i < num_games; ++i) {
									if (ImGui_Button(games[i].creator.ptr)) {
										snet_join_game(snet, games[i].join_token);
									}
								}
							}
						} break;
						case SNET_LISTING_GAMES: {
							ImGui_LabelText("Status", "Finding games");
						} break;
						case SNET_CREATING_GAME: {
							ImGui_LabelText("Status", "Creating game");
						} break;
						case SNET_JOINING_GAME: {
							ImGui_LabelText("Status", "Joining game");
						} break;
						case SNET_JOINED_GAME: {
							ImGui_LabelText("Status", "In game");
							if (ImGui_Button("Send message")) {
								snet_blob_t msg = {
									.ptr = "Hello",
									.size = sizeof("Hello") - 1,
								};
								snet_send(snet, msg, false);
							}
						} break;
					}
				} break;
			}
		}
		ImGui_End();

		cf_app_draw_onto_screen(true);
	}

	cf_free(cookie);
	snet_cleanup(snet);

	cf_destroy_app();
	return 0;
}
