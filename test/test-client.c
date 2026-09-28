/* tiny test client: exercises maximize / minimize / move / close */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct xdg_wm_base *xdg_wm_base;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct zxdg_decoration_manager_v1 *deco_mgr;
static struct zxdg_toplevel_decoration_v1 *deco;
static bool got_deco_mode;
static uint32_t deco_mode;

static void handle_deco_configure(void *data,
		struct zxdg_toplevel_decoration_v1 *d, uint32_t mode) {
	printf("decoration configure: mode=%u\n", mode);
	deco_mode = mode;
	got_deco_mode = true;
}

static const struct zxdg_toplevel_decoration_v1_listener deco_listener = {
	.configure = handle_deco_configure,
};

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *xdg_toplevel;
static bool configured;
static bool got_configure;
static int cfg_width, cfg_height;
static bool cfg_maximized;

static void handle_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = handle_ping,
};

static void handle_configure(void *data, struct xdg_surface *s, uint32_t serial) {
	xdg_surface_ack_configure(s, serial);
	got_configure = true;
}

static void handle_toplevel_configure(void *data, struct xdg_toplevel *t,
		int32_t w, int32_t h, struct wl_array *states) {
	cfg_width = w;
	cfg_height = h;
	cfg_maximized = false;
	uint32_t *state;
	wl_array_for_each(state, states) {
		if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED) cfg_maximized = true;
	}
	printf("toplevel configure: %dx%d maximized=%d\n", w, h, cfg_maximized);
	configured = true;
}

static void handle_toplevel_close(void *data, struct xdg_toplevel *t) {
	printf("toplevel close received\n");
	exit(0);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = handle_configure,
};

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = handle_toplevel_configure,
	.close = handle_toplevel_close,
};

static void handle_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 2);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 4);
	} else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
		deco_mgr = wl_registry_bind(registry, name,
			&zxdg_decoration_manager_v1_interface, 1);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static int roundtrip(void) {
	wl_display_flush(display);
	return wl_display_roundtrip(display);
}

int main(void) {
	display = wl_display_connect(NULL);
	if (!display) { fprintf(stderr, "no display\n"); return 1; }
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	roundtrip();
	if (!compositor || !xdg_wm_base || !shm) {
		fprintf(stderr, "missing globals: compositor=%p xdg=%p shm=%p\n",
			(void *)compositor, (void *)xdg_wm_base, (void *)shm);
		return 1;
	}

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(xdg_toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(xdg_toplevel, "test-client");
	xdg_toplevel_set_app_id(xdg_toplevel, "test-client");
	wl_surface_commit(surface);
	roundtrip();
	printf("initial configure received\n");

	/* test request_maximize */
	got_configure = false;
	xdg_toplevel_set_maximized(xdg_toplevel);
	wl_surface_commit(surface);
	roundtrip();
	if (got_configure) printf("maximize configure delivered\n");
	else printf("FAIL: no configure after maximize\n");

	/* xdg-decoration: request SERVER_SIDE, expect it to be honored */
	if (deco_mgr) {
		deco = zxdg_decoration_manager_v1_get_toplevel_decoration(deco_mgr,
			xdg_toplevel);
		zxdg_toplevel_decoration_v1_add_listener(deco, &deco_listener, NULL);
		zxdg_toplevel_decoration_v1_set_mode(deco,
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		roundtrip();
		printf("after server_side request: mode=%u%s\n", deco_mode,
			deco_mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE ?
			" (honored)" : " (NOT honored)");

		zxdg_toplevel_decoration_v1_set_mode(deco,
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
		roundtrip();
		printf("after client_side request: mode=%u%s\n", deco_mode,
			deco_mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE ?
			" (honored)" : " (NOT honored)");
	} else {
		printf("no decoration manager global\n");
	}

	/* test request_minimize */
	xdg_toplevel_set_minimized(xdg_toplevel);
	roundtrip();

	/* test request_move (needs a serial; just send, compositor must not crash) */
	if (seat) {
		xdg_toplevel_move(xdg_toplevel, seat, 100);
		roundtrip();
	}

	/* test request_close from foreign-toplevel side is not client-visible;
	 * just exit cleanly */
	printf("test-client done\n");
	wl_display_disconnect(display);
	return 0;
}
