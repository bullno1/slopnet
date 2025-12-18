#include <slopnet.h>
#include <cute.h>
#include <dcimgui.h>

int
main(int argc, const char* argv[]) {
	int options = CF_APP_OPTIONS_WINDOW_POS_CENTERED_BIT;
	cf_make_app("slopnet demo", 0, 0, 0, 800, 600, options, argv[0]);
	cf_fs_mount(cf_fs_get_working_directory(), "/", true);
	cf_app_set_vsync(true);

	cf_app_init_imgui();

	snet_config_t snet_config = {
		.host = "snet-dev.bullno1.com",
		.path = "/snet",
	};
	snet_t* snet = snet_init(&snet_config);

	while (cf_app_is_running()) {
		cf_app_update(NULL);

		snet_update(snet);
		const snet_event_t* snet_event;
		while ((snet_event = snet_next_event(snet)) != NULL) {
			switch (snet_event->type) {
				case SNET_EVENT_LOGIN_FINISHED:
					if (snet_event->login.status == SNET_ERR_IO) {
						fprintf(stderr, "Network error\n");
					} else {
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

	snet_cleanup(snet);

	cf_destroy_app();
	return 0;
}
