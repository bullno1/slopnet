#include <slopnet.h>
#include <cute.h>
#include <dcimgui.h>

static void
snet_log(const char* fmt, va_list args, void* ctx) {
	fprintf(stderr, "snet: ");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
}

int
main(int argc, const char* argv[]) {
	int options = CF_APP_OPTIONS_WINDOW_POS_CENTERED_BIT;
	cf_make_app("slopnet demo", 0, 0, 0, 800, 600, options, argv[0]);
	cf_app_set_vsync(true);
	cf_fs_mount(cf_fs_get_working_directory(), "/", true);
	const char* user_dir = cf_fs_get_user_directory("bullno1", "slopnet-demo");
	cf_fs_set_write_directory(user_dir);
	cf_fs_mount(user_dir, "/user/", true);

	cf_app_init_imgui();

	snet_config_t snet_config = {
		.host = "snet-dev.bullno1.com",
		.path = "/snet",
		.log = snet_log,
	};
	snet_t* snet = snet_init(&snet_config);

	size_t cookie_size;
	char* cookie = cf_fs_read_entire_file_to_memory("/user/cookie", &cookie_size);
	if (cookie != NULL) {
		fprintf(stderr, "Logging in with cookie\n");
		snet_login_with_cookie(snet, (snet_blob_t){ .ptr = cookie, .size = cookie_size });
	}

	while (cf_app_is_running()) {
		cf_app_update(NULL);

		snet_update(snet);
		const snet_event_t* snet_event;
		while ((snet_event = snet_next_event(snet)) != NULL) {
			switch (snet_event->type) {
				case SNET_EVENT_LOGIN_FINISHED:
					if (snet_event->login.status == SNET_OK) {
						fprintf(stderr, "Logged in with token: %s\n", (char*)snet_event->login.data.ptr);
						cf_fs_write_string_range_to_file("/cookie", (char*)snet_event->login.data.ptr, (char*)snet_event->login.data.ptr + snet_event->login.data.size);
					} else if (snet_event->login.status == SNET_ERR_IO) {
						fprintf(stderr, "Network error\n");
					} else if (snet_event->login.status == SNET_ERR_REJECTED) {
						fprintf(stderr, "Login failed with reason: %s\n", (char*)snet_event->login.data.ptr);
					}
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
					ImGui_LabelText("Status", "Logged in");
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
