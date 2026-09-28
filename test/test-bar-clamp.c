/* reproduction test: window dragging must be clamped so the window never
 * slides underneath a bottom layer-shell bar (or above a top one), and the
 * window top never goes closer than CONFIG_EDGE_THICKNESS px to the screen
 * top when there is no bar there.
 *
 * Probing: the test owns both the bar and the window, so it cannot tell
 * which one got a button press.  Instead it tracks wl_pointer.enter
 * (which surface the cursor entered).  Because the compositor only sends
 * enter on focus change, the test parks the cursor on empty desktop to
 * clear the focus before each probe. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#define OUT_W 1280
#define OUT_H 720
#define BAR_H 40
#define WIN_W 200
#define WIN_H 100
#define EDGE_THICKNESS 8 /* must match CONFIG_EDGE_THICKNESS in config.h */

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct xdg_wm_base *xdg_wm_base;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *wl_pointer;
static struct zwlr_virtual_pointer_manager_v1 *vp_mgr;
static struct zwlr_virtual_pointer_v1 *vp;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_output *wl_output;

static struct wl_surface *bar_surface;
static struct zwlr_layer_surface_v1 *bar_layer;
static struct wl_surface *win_surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *xdg_toplevel;

static bool xdg_configured;
static int bar_configure_w, bar_configure_h;
static struct wl_surface *last_enter_surface;

static int failures;

static void check(bool cond, const char *fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printf("%s %s\n", cond ? "PASS" : "FAIL", buf);
	if (!cond) {
		failures++;
	}
}

static void handle_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = { .ping = handle_ping };

static void handle_xdg_configure(void *data, struct xdg_surface *s, uint32_t serial) {
	xdg_surface_ack_configure(s, serial);
	xdg_configured = true;
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = handle_xdg_configure,
};

static void handle_toplevel_configure(void *data, struct xdg_toplevel *t,
		int32_t w, int32_t h, struct wl_array *states) {
	(void)data; (void)t; (void)w; (void)h; (void)states;
}
static void handle_toplevel_close(void *data, struct xdg_toplevel *t) { (void)data; (void)t; }
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = handle_toplevel_configure,
	.close = handle_toplevel_close,
};

static void handle_layer_configure(void *data, struct zwlr_layer_surface_v1 *s,
		uint32_t serial, uint32_t w, uint32_t h) {
	zwlr_layer_surface_v1_ack_configure(s, serial);
	bar_configure_w = w;
	bar_configure_h = h;
}
static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = handle_layer_configure,
};

static void handle_pointer_enter(void *data, struct wl_pointer *p,
		uint32_t serial, struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy) {
	(void)data; (void)p; (void)serial; (void)sx; (void)sy;
	last_enter_surface = s;
}
static void handle_pointer_leave(void *data, struct wl_pointer *p,
		uint32_t serial, struct wl_surface *s) { (void)data; (void)p; (void)serial; (void)s; }
static void handle_pointer_motion(void *data, struct wl_pointer *p,
		uint32_t time, wl_fixed_t sx, wl_fixed_t sy) { (void)data; (void)p; (void)time; (void)sx; (void)sy; }
static void handle_pointer_button(void *data, struct wl_pointer *p,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	(void)data; (void)p; (void)serial; (void)time; (void)button; (void)state;
}
static void handle_pointer_axis(void *data, struct wl_pointer *p,
		uint32_t time, uint32_t axis, wl_fixed_t value) { (void)data; (void)p; (void)time; (void)axis; (void)value; }
static void handle_pointer_frame(void *data, struct wl_pointer *p) { (void)data; (void)p; }
static void handle_pointer_axis_source(void *data, struct wl_pointer *p,
		uint32_t axis_source) { (void)data; (void)p; (void)axis_source; }
static void handle_pointer_axis_stop(void *data, struct wl_pointer *p,
		uint32_t time, uint32_t axis) { (void)data; (void)p; (void)time; (void)axis; }
static void handle_pointer_axis_discrete(void *data, struct wl_pointer *p,
		uint32_t axis, int32_t discrete) { (void)data; (void)p; (void)axis; (void)discrete; }
static const struct wl_pointer_listener pointer_listener = {
	.enter = handle_pointer_enter,
	.leave = handle_pointer_leave,
	.motion = handle_pointer_motion,
	.button = handle_pointer_button,
	.axis = handle_pointer_axis,
	.frame = handle_pointer_frame,
	.axis_source = handle_pointer_axis_source,
	.axis_stop = handle_pointer_axis_stop,
	.axis_discrete = handle_pointer_axis_discrete,
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
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		wl_output = wl_registry_bind(registry, name, &wl_output_interface, 1);
	} else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
		vp_mgr = wl_registry_bind(registry, name,
			&zwlr_virtual_pointer_manager_v1_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 1);
	}
}
static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static void roundtrip(void) {
	wl_display_flush(display);
	wl_display_roundtrip(display);
}

static void vp_motion_abs(int x, int y) {
	zwlr_virtual_pointer_v1_motion_absolute(vp, 1, x, y, OUT_W, OUT_H);
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(display);
}

static void vp_button(uint32_t button, uint32_t state) {
	zwlr_virtual_pointer_v1_button(vp, 1, button, state);
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(display);
}

static struct wl_buffer *make_buffer(int w, int h) {
	int stride = w * 4;
	int fd = syscall(SYS_memfd_create, "buf", 0);
	if (fd < 0) { perror("memfd"); exit(1); }
	ftruncate(fd, stride * h);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, stride * h);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h,
		stride, WL_SHM_FORMAT_ARGB8888);
	return buffer;
}

/* a bar anchored at the top or bottom with a 40px exclusive zone.
 * pass_output = false simulates a NULL-output bar (spans all outputs). */
static void create_bar(bool at_top, bool pass_output) {
	bar_surface = wl_compositor_create_surface(compositor);
	bar_layer = zwlr_layer_shell_v1_get_layer_surface(layer_shell, bar_surface,
		pass_output ? wl_output : NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "test-bar");
	zwlr_layer_surface_v1_add_listener(bar_layer, &layer_listener, NULL);
	uint32_t anchor = at_top ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	zwlr_layer_surface_v1_set_anchor(bar_layer,
		anchor | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_size(bar_layer, 0, BAR_H);
	zwlr_layer_surface_v1_set_exclusive_zone(bar_layer, BAR_H);
	wl_surface_commit(bar_surface);
	roundtrip();
	roundtrip();
	struct wl_buffer *buf = make_buffer(bar_configure_w > 0 ? bar_configure_w : OUT_W,
		bar_configure_h > 0 ? bar_configure_h : BAR_H);
	wl_surface_attach(bar_surface, buf, 0, 0);
	wl_surface_commit(bar_surface);
	roundtrip();
	roundtrip();
}

static void destroy_bar(void) {
	zwlr_layer_surface_v1_destroy(bar_layer);
	wl_surface_destroy(bar_surface);
	bar_layer = NULL;
	bar_surface = NULL;
	roundtrip();
	roundtrip();
}

/* 200x100 shm buffer + toplevel, mapped centered on the cursor */
static void create_window(void) {
	win_surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, win_surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(xdg_toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(xdg_toplevel, "bar-clamp-test");
	wl_surface_commit(win_surface);
	roundtrip();
	for (int i = 0; i < 20 && !xdg_configured; i++) {
		roundtrip();
	}
	struct wl_buffer *buf = make_buffer(WIN_W, WIN_H);
	wl_surface_attach(win_surface, buf, 0, 0);
	wl_surface_commit(win_surface);
	roundtrip();
	roundtrip();
}

static void destroy_window(void) {
	xdg_toplevel_destroy(xdg_toplevel);
	xdg_surface_destroy(xdg_surface);
	wl_surface_destroy(win_surface);
	xdg_configured = false;
	win_surface = NULL;
	roundtrip();
	roundtrip();
}

/* park the cursor on empty desktop so the pointer focus is cleared; the
 * next enter event then fires on the surface under the probe point */
static void clear_pointer_focus(void) {
	last_enter_surface = NULL;
	vp_motion_abs(200, 600);
	roundtrip();
	roundtrip();
}

/* move to (x,y), then check which surface the cursor entered */
static void probe(int x, int y) {
	clear_pointer_focus();
	last_enter_surface = NULL;
	vp_motion_abs(x, y);
	roundtrip();
}

static void drag_window(int from_y, int to_y) {
	vp_motion_abs(640, from_y); /* title strip: window box is x in [540,740] */
	roundtrip();
	vp_button(BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
	roundtrip();
	vp_motion_abs(640, to_y);
	roundtrip();
	vp_button(BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
	roundtrip();
	roundtrip();
}

int main(void) {
	display = wl_display_connect(NULL);
	if (!display) { fprintf(stderr, "no display\n"); return 1; }
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	roundtrip();
	if (!compositor || !xdg_wm_base || !shm || !seat || !vp_mgr || !layer_shell) {
		fprintf(stderr, "missing globals\n");
		return 1;
	}
	wl_pointer = wl_seat_get_pointer(seat);
	wl_pointer_add_listener(wl_pointer, &pointer_listener, NULL);
	vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(vp_mgr, seat);
	roundtrip();

	for (int pass = 0; pass < 2; pass++) {
		const char *how = pass == 0 ? "NULL-output" : "per-output";

		create_bar(false, pass == 1); /* bottom bar */
		printf("== [%s] bottom bar ==\n", how);

		vp_motion_abs(640, 360);
		roundtrip();
		create_window(); /* window centered at (640,360) -> (540,310) */

		/* drag far down, past the bar: the CURSOR is clamped at the bar top
		 * (680), the window follows it with no bottom limit.  The window
		 * was grabbed at strip y=315 (grab_y=5), so it ends at
		 * top=680-5=675, bottom=775 (55px past the screen bottom). */
		drag_window(315, 710);
		probe(640, 660); /* above the window (660 < 675) -> not the window */
		check(last_enter_surface != win_surface,
			"[%s] bottom bar: window slid past the bar (y=660 -> not window)", how);
		probe(640, 676); /* inside the window's top, just above the bar */
		check(last_enter_surface == win_surface,
			"[%s] bottom bar: window follows the clamped cursor (y=676 -> window)", how);
		probe(640, 700); /* over the bar -> the bar surface wins (it is above the window) */
		check(last_enter_surface == bar_surface,
			"[%s] bottom bar: bar stays on top (y=700 -> bar)", how);

		/* drag far up (no top bar): the window follows the cursor with no
		 * top clamp; the cursor is limited to the work area (y>=0).  The
		 * window is at [675,775], grab its strip at y=677 (grab_y=2), drag
		 * to y=5 -> window top = 5-2 = 3, y in [3,103]. */
		drag_window(677, 5);
		probe(640, 40); /* inside the window */
		check(last_enter_surface == win_surface,
			"[%s] top: window follows the cursor (y=40 -> window)", how);
		probe(640, 2); /* just above the window top (2 < 3) -> nothing */
		check(last_enter_surface == NULL,
			"[%s] top: window not off the screen (y=2 -> nothing)", how);

		destroy_window();
		destroy_bar();
	}

	for (int pass = 0; pass < 2; pass++) {
		const char *how = pass == 0 ? "NULL-output" : "per-output";

		create_bar(true, pass == 1); /* top bar */
		printf("== [%s] top bar ==\n", how);

		vp_motion_abs(640, 360);
		roundtrip();
		create_window();

		/* drag far up, over the top bar: the window's top must stop below
		 * the bar -> window y in [43,143] (40px bar + 3px gap) */
		drag_window(315, 5);
		probe(640, 120); /* inside the clamped window */
		check(last_enter_surface == win_surface,
			"[%s] top bar: window clamped below bar (y=120 -> window)", how);
		probe(640, 20); /* over the bar -> bar wins */
		check(last_enter_surface == bar_surface,
			"[%s] top bar: window never over the bar (y=20 -> bar)", how);

		destroy_window();
		destroy_bar();
	}

	printf(failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", failures);
	return failures == 0 ? 0 : 1;
}
