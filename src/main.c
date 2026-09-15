/*
 * monodywm - a minimal floating Wayland compositor built on wlroots 0.19
 *
 * Entry point: creates the display, backend, renderer and scene, creates
 * every protocol global (through wlroots, which implements them
 * server-side; the XML descriptions live in Protocol/ and are turned into
 * .h/.c by wayland-scanner at build time), registers the module listeners
 * and runs the event loop.
 *
 * Modules:
 *   ipc.c      - status-bar socket (JSON events)
 *   scene.c    - scene-graph tagging / hit-testing
 *   toplevel.c - xdg-shell windows, window state
 *   layer.c    - wlr-layer-shell surfaces + work area
 *   output.c   - monitors + wlr-output-management
 *   input.c    - seat, keyboard, shortcuts
 *   pointer.c  - cursor interaction (move / resize / gestures)
 */

#include "server.h"

#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/types/wlr_xdg_output_v1.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/util/log.h>

/* run a command in a detached child process:
 *   - setsid(): new session, no controlling terminal, immune to terminal
 *     signals (Ctrl+C, SIGHUP when the tty closes), keeps running even if
 *     the compositor's terminal goes away
 *   - stdin/stdout/stderr -> /dev/null: no output polluting the compositor's
 *     tty, and the process can never block on a terminal read/write
 *   - _exit(127) if exec fails: never fall through into the compositor */
void spawn_command(const char *cmd) {
	if (cmd == NULL || cmd[0] == '\0') {
		return;
	}

	pid_t pid = fork();

	if (pid < 0) {
		wlr_log(WLR_ERROR, "spawn: fork failed for \"%s\": %s", cmd, strerror(errno));
		return;
	}

	if (pid > 0) {
		wlr_log(WLR_DEBUG, "spawn: spawned \"%s\" (pid %ld)", cmd, (long)pid);
		return;
	}

	if (setsid() == -1) {
		_exit(127);
	}

	/* never leave the child in the compositor's working directory: the
	 * compositor's CWD can be deleted while it runs (e.g. the build
	 * directory is removed and rebuilt), and some apps - foot in
	 * particular, which starts a shell - hang before mapping their
	 * window when the CWD no longer exists.  A stable directory also
	 * keeps apps from inheriting a weird location. */
	const char *home = getenv("HOME");
	if (home == NULL || home[0] == '\0' || chdir(home) == -1) {
		chdir("/");
	}

	int devnull = open("/dev/null", O_RDWR);

	if (devnull < 0) {
		_exit(127);
	}

	if (dup2(devnull, STDIN_FILENO) == -1) {
		close(devnull);
		_exit(127);
	}

	if (dup2(devnull, STDOUT_FILENO) == -1) {
		close(devnull);
		_exit(127);
	}

	if (dup2(devnull, STDERR_FILENO) == -1) {
		close(devnull);
		_exit(127);
	}

	if (devnull > STDERR_FILENO) {
		close(devnull);
	}

	execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);

	_exit(127);
}

static void reap_children(int sig) {
	(void)sig;

	int saved_errno = errno;
	while (waitpid(-1, NULL, WNOHANG) > 0) {
	}

	errno = saved_errno;
}

/* install the SIGCHLD handler (called once from main()) */
static void init_reaper(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = reap_children;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigemptyset(&sa.sa_mask);

	if (sigaction(SIGCHLD, &sa, NULL) == -1) {
		wlr_log(WLR_ERROR,
			"init_reaper: failed to install SIGCHLD handler: %s",
			strerror(errno));
	}

	/* never die from a write to a disconnected IPC client (a status bar
	 * that exited): the IPC layer cleans the client up and reports the
	 * failure instead of letting SIGPIPE kill the whole compositor */
	signal(SIGPIPE, SIG_IGN);
}

static void run_startup_file(void) {
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");

	char path[4096];

	if (xdg != NULL && xdg[0] != '\0') {
		int ret = snprintf(path, sizeof(path), "%s/monodywm/run", xdg);

		if (ret < 0 || (size_t)ret >= sizeof(path)) {
			wlr_log(WLR_ERROR, "run_startup_file: configuration path is too long");
			return;
		}
	} else if (home != NULL && home[0] != '\0') {
		int ret = snprintf(path, sizeof(path), "%s/.config/monodywm/run", home);
		if (ret < 0 || (size_t)ret >= sizeof(path)) { wlr_log(WLR_ERROR,
				"run_startup_file: configuration path is too long");
			return;
		}
	} else {
		wlr_log(WLR_DEBUG,
			"run_startup_file: HOME and XDG_CONFIG_HOME are not set");
		return;
	}

	FILE *f = fopen(path, "r");

	if (f == NULL) {
		if (errno == ENOENT) {
			wlr_log(WLR_DEBUG, "run_startup_file: no %s, nothing to run", path);
		} else {
			wlr_log(WLR_ERROR, "run_startup_file: failed to open %s: %s", path, strerror(errno));
		}

		return;
	}

	char *line = NULL;
	size_t cap = 0;
	ssize_t len;

	while ((len = getline(&line, &cap, f)) != -1) {
		/* strip the trailing newline / carriage return */
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			line[--len] = '\0';
		}

		/* skip leading whitespace, blank lines and comments */
		char *cmd = line;
		while (*cmd == ' ' || *cmd == '\t') {
			cmd++;
		}
		if (*cmd == '\0' || *cmd == '#') {
			continue;
		}

		wlr_log(WLR_INFO, "run_startup_file: executing: %s", cmd);
		spawn_command(cmd);
	}
	free(line);
	fclose(f);
}

int main(int argc, char *argv[]) {
	/* WLR_DEBUG=1 switches to debug logging (the bundled tests verify cursor
	 * decisions through the "cursor: ..." WLR_DEBUG lines) */
	const char *dbg = getenv("WLR_DEBUG");
	wlr_log_init(dbg != NULL && dbg[0] != '\0' ? WLR_DEBUG : WLR_INFO, NULL);
	/* never leave spawned helpers as zombies */
	init_reaper();

	char *startup_cmd = NULL;
	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			fprintf(stderr, "usage: %s [-s startup-command]\n", argv[0]);
			return EXIT_FAILURE;
		}
	}

	struct server server = {0};

	server.display = wl_display_create();
	if (server.display == NULL) {
		return EXIT_FAILURE;
	}
	server.backend = wlr_backend_autocreate(
		wl_display_get_event_loop(server.display), NULL);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create backend");
		return EXIT_FAILURE;
	}
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create renderer");
		return EXIT_FAILURE;
	}
	wlr_renderer_init_wl_display(server.renderer, server.display);
	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create allocator");
		return EXIT_FAILURE;
	}
	server.scene = wlr_scene_create();
	if (server.scene == NULL) {
		wlr_log(WLR_ERROR, "failed to create scene");
		return EXIT_FAILURE;
	}
	server.output_layout = wlr_output_layout_create(server.display);
	server.output_manager = wlr_output_manager_v1_create(server.display);
	/* xdg-output-unstable-v1: tells clients (grim, xwayland, ...) the
	 * logical geometry/name of each output.  grim refuses to trust the
	 * core wl_output geometry for capture and falls back to guessing when
	 * this global is missing, which produces 0x0 captures on this layout. */
	wlr_xdg_output_manager_v1_create(server.display, server.output_layout);
	server.foreign_toplevel_manager =
		wlr_foreign_toplevel_manager_v1_create(server.display);

	server.seat = wlr_seat_create(server.display, "seat0");
	server.cursor = wlr_cursor_create();
	if (server.cursor == NULL) {
		wlr_log(WLR_ERROR, "failed to create cursor");
		return EXIT_FAILURE;
	}
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	server.xcursor_manager = wlr_xcursor_manager_create(CONFIG_CURSOR_THEME, 24);
	if (server.xcursor_manager == NULL) {
		wlr_log(WLR_ERROR, "failed to create xcursor manager");
		return EXIT_FAILURE;
	}
	/* show the default arrow right away so the pointer is visible when the
	 * first output appears - otherwise no cursor image exists until the
	 * first pointer motion and the desktop starts without a cursor */
	wlr_cursor_set_xcursor(server.cursor, server.xcursor_manager, "left_ptr");

	for (int i = 0; i < LAYER_COUNT; i++) {
		server.layers[i] = wlr_scene_tree_create(&server.scene->tree);
		if (server.layers[i] == NULL) {
			wlr_log(WLR_ERROR, "failed to create scene tree");
			return EXIT_FAILURE;
		}
	}
	wl_list_init(&server.toplevels);
	wl_list_init(&server.layer_surfaces);
	wl_list_init(&server.imes);
	wl_list_init(&server.text_inputs);
	wl_list_init(&server.keyboards);

	/* ---- core & stable protocols ----
	  No explicit wl_shm creation: wlr_renderer_init_wl_display() above
	  already registered the wl_shm global (and, on renderers with a DRM
	  fd and dmabuf support, the linux-dmabuf global too).  Creating
	  wl_shm again here advertised a second duplicate wl_shm global. */
	wlr_compositor_create(server.display, 6, server.renderer);
	wlr_subcompositor_create(server.display);

	if (wlr_renderer_get_texture_formats(server.renderer,
			WLR_BUFFER_CAP_DMABUF) == NULL || wlr_renderer_get_drm_fd(server.renderer) < 0) {
		wlr_linux_dmabuf_v1_create_with_renderer(server.display, 5, server.renderer);
	}
	wlr_data_device_manager_create(server.display);
	/* ext-data-control-v1: privileged clipboard/selection control for tools
	 * such as wl-clipboard (wl-copy/wl-paste) and clipboard managers, which
	 * is how editors like vim reach the Wayland clipboard.  wlroots
	 * implements the protocol server-side and bridges it to the seat
	 * selection/primary-selection, so registering the global is enough. */
	server.ext_data_control_manager =
		wlr_ext_data_control_manager_v1_create(server.display, 1);
	if (server.ext_data_control_manager == NULL) {
		wlr_log(WLR_ERROR, "failed to create ext-data-control-v1 global");
	}
	/* wlr-data-control-unstable-v1: the legacy wlroots clipboard-control
	 * protocol.  CopyQ (and older wl-clipboard/detached clipboard managers)
	 * still bind only this one, not ext-data-control-v1, so it has to stay
	 * advertised for them to reach the clipboard.  Implemented server-side
	 * by wlroots like the ext- variant. */
	server.data_control_manager =
		wlr_data_control_manager_v1_create(server.display);
	if (server.data_control_manager == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr-data-control-v1 global");
	}
	struct wlr_xdg_shell *xdg_shell =
		wlr_xdg_shell_create(server.display, 6);
	wlr_viewporter_create(server.display);
	wlr_presentation_create(server.display, server.backend, 2);

	/* ---- wlroots protocols ---- */
	struct wlr_layer_shell_v1 *layer_shell =
		wlr_layer_shell_v1_create(server.display, 5);
	struct wlr_xdg_decoration_manager_v1 *decoration_manager =
		wlr_xdg_decoration_manager_v1_create(server.display);
	struct wlr_virtual_pointer_manager_v1 *virtual_pointer_manager =
		wlr_virtual_pointer_manager_v1_create(server.display);

	/* wlr-screencopy-v1: screen capture for grim/slurp/wf-recorder.
	 * wlroots implements the whole protocol server-side (frame capture,
	 * damage, cursor overlay), the compositor only registers the global.
	 * Kept in favor of ext-image-copy-capture-v1: it is what the capture
	 * tools expect today and works with the bundled DMABUF/SHM paths. */
	struct wlr_screencopy_manager_v1 *screencopy_manager = wlr_screencopy_manager_v1_create(server.display);
	if (screencopy_manager == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr-screencopy-v1 global");
	}

	/* cursor-shape-v1: clients pick a shape, the compositor renders it from
	 * its own xcursor theme at the output's (fractional) scale, so the
	 * cursor size always matches - no client-side guessing */
	server.cursor_shape_manager =
		wlr_cursor_shape_manager_v1_create(server.display, 1);

	/* pointer-constraints-v1 + relative-pointer-v1: clients such as QEMU
	 * (captured mouse) and games lock or confine the pointer to one of
	 * their surfaces and read the raw relative deltas while it is locked.
	 * wlroots implements both server-side; pointer.c drives the constraint
	 * activation and the delta handling, here only the globals are
	 * registered. */
	server.pointer_constraints =
		wlr_pointer_constraints_v1_create(server.display);
	if (server.pointer_constraints == NULL) {
		wlr_log(WLR_ERROR,
			"failed to create pointer-constraints-v1 global");
	}
	server.relative_pointer_manager =
		wlr_relative_pointer_manager_v1_create(server.display);
	if (server.relative_pointer_manager == NULL) {
		wlr_log(WLR_ERROR, "failed to create relative-pointer-v1 global");
	}

	/* xdg-activation-v1: clients can request focus through an activation
	 * token; the compositor focuses the matching toplevel on request */
	server.activation = wlr_xdg_activation_v1_create(server.display);
	if (server.activation == NULL) {
		wlr_log(WLR_ERROR, "failed to create xdg-activation-v1 global");
	}

	/* wp_fractional_scale_v1: surfaces are told the output's exact
	 * fractional scale; wlroots scene surfaces (layer-shell, subsurfaces,
	 * cursor, toplevels) handle it automatically */
	server.fractional_scale_manager =
		wlr_fractional_scale_manager_v1_create(server.display, 1);
	if (server.fractional_scale_manager == NULL) {
		wlr_log(WLR_ERROR, "failed to create fractional-scale-v1 global");
	}

	/* ---- input method (fcitx5 / ibus) ---- */
	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_manager =
		wlr_virtual_keyboard_manager_v1_create(server.display);
	struct wlr_input_method_manager_v2 *input_method_manager =
		wlr_input_method_manager_v2_create(server.display);
	struct wlr_text_input_manager_v3 *text_input_manager =
		wlr_text_input_manager_v3_create(server.display);

	/* ---- listeners ---- */
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.new_virtual_pointer.notify = server_new_virtual_pointer;
	wl_signal_add(&virtual_pointer_manager->events.new_virtual_pointer, &server.new_virtual_pointer);
	server.new_virtual_keyboard.notify = server_new_virtual_keyboard;
	wl_signal_add(&virtual_keyboard_manager->events.new_virtual_keyboard, &server.new_virtual_keyboard);
	server.layout_change.notify = server_layout_change;
	wl_signal_add(&server.output_layout->events.change, &server.layout_change);
	server.output_manager_apply.notify = output_manager_apply;
	wl_signal_add(&server.output_manager->events.apply, &server.output_manager_apply);
	server.output_manager_test.notify = output_manager_test;
	wl_signal_add(&server.output_manager->events.test, &server.output_manager_test);
	server.new_xdg_toplevel.notify = server_new_toplevel;
	wl_signal_add(&xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	server.new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&layer_shell->events.new_surface, &server.new_layer_surface);
	server.new_decoration.notify = server_new_decoration;
	wl_signal_add(&decoration_manager->events.new_toplevel_decoration, &server.new_decoration);
	server.new_ime.notify = ime_new_input_method;
	wl_signal_add(&input_method_manager->events.new_input_method, &server.new_ime);
	server.new_text_input.notify = ime_new_text_input;
	wl_signal_add(&text_input_manager->events.new_text_input, &server.new_text_input);

	server.cursor_motion.notify = cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
	server.cursor_button.notify = cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);
	if (server.pointer_constraints != NULL) {
		server.new_pointer_constraint.notify = new_pointer_constraint;
		wl_signal_add(&server.pointer_constraints->events.new_constraint,
			&server.new_pointer_constraint);
	}
	server.pointer_focus_change.notify = pointer_focus_change;
	wl_signal_add(&server.seat->pointer_state.events.focus_change,
		&server.pointer_focus_change);

	server.seat_request_set_cursor.notify = seat_request_set_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor, &server.seat_request_set_cursor);
	server.cursor_shape_set_shape.notify = seat_request_set_shape;
	wl_signal_add(&server.cursor_shape_manager->events.request_set_shape, &server.cursor_shape_set_shape);
	if (server.activation != NULL) {
		server.activation_request_activate.notify = xdg_activation_request_activate;
		wl_signal_add(&server.activation->events.request_activate,
			&server.activation_request_activate);
	}
	server.seat_request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection, &server.seat_request_set_selection);
	server.seat_request_set_primary_selection.notify = seat_request_set_primary_selection;
	wl_signal_add(&server.seat->events.request_set_primary_selection, &server.seat_request_set_primary_selection);
	server.seat_request_start_drag.notify = seat_request_start_drag;
	wl_signal_add(&server.seat->events.request_start_drag, &server.seat_request_start_drag);
	server.seat_start_drag.notify = seat_start_drag;
	wl_signal_add(&server.seat->events.start_drag, &server.seat_start_drag);

	wlr_seat_set_capabilities(server.seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

	const char *socket = wl_display_add_socket_auto(server.display);
	if (socket == NULL) {
		wlr_log(WLR_ERROR, "failed to add socket");
		return EXIT_FAILURE;
	}

	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "failed to start backend");
		return EXIT_FAILURE;
	}

	/* linux-drm-syncobj-v1: advertise explicit synchronization only when
	 * both the renderer and the backend can handle timeline wait/signal.
	 * Use the renderer's DRM fd (the device actually used for rendering),
	 * never a hard-coded /dev/dri/card0. */
	if (server.renderer->features.timeline && server.backend->features.timeline) {
		int drm_fd = wlr_renderer_get_drm_fd(server.renderer);
		if (drm_fd >= 0) {
			server.linux_drm_syncobj_manager =
				wlr_linux_drm_syncobj_manager_v1_create(server.display, 1,
					drm_fd);
			if (server.linux_drm_syncobj_manager == NULL) {
				wlr_log(WLR_ERROR,
					"failed to create linux-drm-syncobj-v1 global");
			}
		} else {
			wlr_log(WLR_INFO,
				"renderer has no DRM fd, disabling linux-drm-syncobj-v1");
		}
	} else {
		wlr_log(WLR_INFO,
			"renderer/backend timeline support unavailable, disabling "
			"linux-drm-syncobj-v1");
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	/* the Wayland socket is live: the WM is up, now start the user's
	 * daemons from ~/.config/mywm/run (plus any -s command) */
	run_startup_file();
	if (startup_cmd != NULL) {
		spawn_command(startup_cmd);
	}

	/* IPC socket for status bars (JSON over a Unix domain socket) */
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	char ipc_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	if (runtime_dir != NULL && runtime_dir[0] != '\0') {
		snprintf(ipc_path, sizeof(ipc_path), "%s/monodywm.sock",
			runtime_dir);
	} else {
		snprintf(ipc_path, sizeof(ipc_path), "/tmp/monodywm.sock");
	}
	server.ipc_fd = -1;
	if (!ipc_server_init(&server, ipc_path)) {
		wlr_log(WLR_ERROR, "failed to start IPC socket at %s", ipc_path);
	}

	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
		socket);
	wl_display_run(server.display);

	/* wlroots asserts that the objects it owns (output layout, output
	 * manager, seat, protocol managers, ...) are destroyed without leftover
	 * listeners, so detach every compositor listener before teardown. The
	 * per-surface listeners (toplevels, layer surfaces, ime, cursors) are
	 * removed by their own destroy handlers during destroy_clients(). */
	wl_list_remove(&server.new_output.link);
	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.new_virtual_pointer.link);
	wl_list_remove(&server.new_virtual_keyboard.link);
	wl_list_remove(&server.layout_change.link);
	wl_list_remove(&server.output_manager_apply.link);
	wl_list_remove(&server.output_manager_test.link);
	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_layer_surface.link);
	wl_list_remove(&server.new_decoration.link);
	wl_list_remove(&server.new_ime.link);
	wl_list_remove(&server.new_text_input.link);
	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);
	if (server.pointer_constraints != NULL) {
		wl_list_remove(&server.new_pointer_constraint.link);
	}
	wl_list_remove(&server.pointer_focus_change.link);
	/* the per-surface commit listener is normally dropped by the constraint
	 * destroy handler; detach it here too so teardown never trips wlroots'
	 * empty-surface-signal assert */
	if (server.constraint_commit.link.prev != NULL) {
		wl_list_remove(&server.constraint_commit.link);
	}
	wl_list_remove(&server.seat_request_set_cursor.link);
	wl_list_remove(&server.cursor_shape_set_shape.link);
	if (server.activation != NULL) {
		wl_list_remove(&server.activation_request_activate.link);
	}
	wl_list_remove(&server.seat_request_set_selection.link);
	wl_list_remove(&server.seat_request_set_primary_selection.link);
	wl_list_remove(&server.seat_request_start_drag.link);
	wl_list_remove(&server.seat_start_drag.link);

	ipc_server_destroy(&server);
	wl_display_destroy_clients(server.display);
	wl_display_destroy(server.display);
	return EXIT_SUCCESS;
}
