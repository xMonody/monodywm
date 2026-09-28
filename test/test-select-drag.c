/* selection-drag cursor test: terminal-like client (cursor-shape "text" on
 * enter), holds the left button (implicit grab) and drags to the top edge and
 * outside; the compositor logs every cursor decision with WLR_DEBUG. */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "cursor-shape-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct xdg_wm_base *xdg_wm_base;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *vp_mgr;
static struct zwlr_virtual_pointer_v1 *vp;
static struct wp_cursor_shape_manager_v1 *shape_mgr;
static struct wp_cursor_shape_device_v1 *shape_dev;

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *xdg_toplevel;
static bool xdg_configured;
static int out_w = 0, out_h = 0;

#define WIN_W 200
#define WIN_H 100

static void handle_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_base_listener = { .ping = handle_ping };

static void handle_xdg_configure(void *d, struct xdg_surface *s, uint32_t serial) {
	xdg_surface_ack_configure(s, serial);
	xdg_configured = true;
}
static const struct xdg_surface_listener xdg_surface_listener = { .configure = handle_xdg_configure };

static void handle_toplevel_configure(void *d, struct xdg_toplevel *t,
		int32_t w, int32_t h, struct wl_array *s) { (void)s; }
static void handle_toplevel_close(void *d, struct xdg_toplevel *t) {}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = handle_toplevel_configure, .close = handle_toplevel_close,
};

static uint32_t enter_serial;
static void handle_pointer_enter(void *d, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy) {
	printf("[client] ENTER\n"); fflush(stdout);
	enter_serial = serial;
	if (shape_dev) {
		wp_cursor_shape_device_v1_set_shape(shape_dev, serial,
			WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
		printf("[client] set_shape(TEXT)\n"); fflush(stdout);
	}
}
static void handle_pointer_leave(void *d, struct wl_pointer *p,
		uint32_t serial, struct wl_surface *s) {
	printf("[client] LEAVE\n"); fflush(stdout);
}
static void handle_pointer_motion(void *d, struct wl_pointer *p,
		uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
	printf("[client] motion %d,%d\n", wl_fixed_to_int(sx), wl_fixed_to_int(sy)); fflush(stdout);
	/* mimic a CSD client that switches to a resize cursor near the top edge */
	if (wl_fixed_to_int(sy) <= 4 && shape_dev) {
		wp_cursor_shape_device_v1_set_shape(shape_dev, enter_serial,
			WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE);
		printf("[client] set_shape(E_RESIZE)\n"); fflush(stdout);
	}
}
static void handle_pointer_button(void *d, struct wl_pointer *p,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {}
static void handle_pointer_axis(void *d, struct wl_pointer *p,
		uint32_t time, uint32_t axis, wl_fixed_t value) {}
static void handle_pointer_frame(void *d, struct wl_pointer *p) {}
static void handle_pointer_axis_source(void *d, struct wl_pointer *p, uint32_t s) {}
static void handle_pointer_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void handle_pointer_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {}
static const struct wl_pointer_listener pointer_listener = {
	.enter = handle_pointer_enter, .leave = handle_pointer_leave,
	.motion = handle_pointer_motion, .button = handle_pointer_button,
	.axis = handle_pointer_axis, .frame = handle_pointer_frame,
	.axis_source = handle_pointer_axis_source, .axis_stop = handle_pointer_axis_stop,
	.axis_discrete = handle_pointer_axis_discrete,
};

static void handle_output_geometry(void *d, struct wl_output *o, int x, int y,
		int pw, int ph, int sub, const char *make, const char *model, int t) {}
static void handle_output_mode(void *d, struct wl_output *o, uint32_t flags,
		int w, int h, int refresh) { if (w > 0 && h > 0) { out_w = w; out_h = h; } }
static void handle_output_done(void *d, struct wl_output *o) {}
static void handle_output_scale(void *d, struct wl_output *o, int32_t f) {}
static const struct wl_output_listener output_listener = {
	.geometry = handle_output_geometry, .mode = handle_output_mode,
	.done = handle_output_done, .scale = handle_output_scale,
};

static void handle_global(void *d, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t version) {
	if (strcmp(iface, wl_compositor_interface.name) == 0)
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
		xdg_wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 2);
	else if (strcmp(iface, wl_shm_interface.name) == 0)
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (strcmp(iface, wl_seat_interface.name) == 0)
		seat = wl_registry_bind(r, name, &wl_seat_interface, 4);
	else if (strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0)
		vp_mgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
	else if (strcmp(iface, wp_cursor_shape_manager_v1_interface.name) == 0)
		shape_mgr = wl_registry_bind(r, name, &wp_cursor_shape_manager_v1_interface, 1);
	else if (strcmp(iface, wl_output_interface.name) == 0) {
		struct wl_output *o = wl_registry_bind(r, name, &wl_output_interface, 1);
		wl_output_add_listener(o, &output_listener, NULL);
	}
}
static const struct wl_registry_listener registry_listener = { .global = handle_global };

static void rt(void) { wl_display_flush(display); wl_display_roundtrip(display); }

static struct wl_buffer *make_buffer(int w, int h) {
	int stride = w * 4;
	int fd = syscall(SYS_memfd_create, "buf", 0);
	ftruncate(fd, stride * h);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, stride * h);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool); close(fd);
	return buf;
}

static void create_window(void) {
	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(xdg_toplevel, &toplevel_listener, NULL);
	wl_surface_commit(surface);
	rt();
	for (int i = 0; i < 20 && !xdg_configured; i++) rt();
	struct wl_buffer *buf = make_buffer(WIN_W, WIN_H);
	wl_surface_attach(surface, buf, 0, 0);
	wl_surface_commit(surface);
	rt(); rt();
}

static int px, py;
static void move_to(int x, int y) {
	int dx = x - px, dy = y - py; px = x; py = y;
	if (dx || dy) {
		zwlr_virtual_pointer_v1_motion(vp, 1, wl_fixed_from_int(dx), wl_fixed_from_int(dy));
		zwlr_virtual_pointer_v1_frame(vp);
	}
	wl_display_flush(display); rt();
}

int main(void) {
	display = wl_display_connect(NULL);
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	rt();
	if (!compositor || !xdg_wm_base || !shm || !seat || !vp_mgr || !shape_mgr) {
		fprintf(stderr, "missing globals\n"); return 1;
	}
	struct wl_pointer *wl_pointer = wl_seat_get_pointer(seat);
	wl_pointer_add_listener(wl_pointer, &pointer_listener, NULL);
	shape_dev = wp_cursor_shape_manager_v1_get_pointer(shape_mgr, wl_pointer);
	vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(vp_mgr, seat);
	rt();
	for (int i = 0; i < 20 && out_w == 0; i++) rt();

	int cx = out_w / 2, cy = out_h / 2;
	/* park far away so the later move_to(cx,cy) actually generates motion
	 * (and therefore a pointer enter into the window) */
	zwlr_virtual_pointer_v1_motion_absolute(vp, 1, 10, 10, out_w, out_h);
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(display); rt();
	px = 10; py = 10;

	create_window(); /* 200x100 centered -> box (cx-100,cy-50)-(cx+100,cy+50) */

	printf("== enter body ==\n"); fflush(stdout);
	move_to(cx, cy);

	printf("== press RIGHT (hold) ==\n"); fflush(stdout);
	zwlr_virtual_pointer_v1_button(vp, 1, BTN_RIGHT, 1);
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(display); rt();

	printf("== drag to left edge zone ==\n"); fflush(stdout);
	move_to(cx - 100, cy);  /* left edge, inside the 4px zone */
	printf("== drag into the edge zone (outside) ==\n"); fflush(stdout);
	move_to(cx - 102, cy);  /* 2px outside the window, within the 8px zone */

	printf("== press LEFT at the edge while RIGHT held ==\n"); fflush(stdout);
	zwlr_virtual_pointer_v1_button(vp, 1, BTN_LEFT, 1);
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(display); rt();

	printf("== release LEFT ==\n"); fflush(stdout);
	zwlr_virtual_pointer_v1_button(vp, 1, BTN_LEFT, 0);
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(display); rt();

	printf("== drag to top edge zone ==\n"); fflush(stdout);
	move_to(cx, cy - 46);
	printf("== drag below to bottom edge zone ==\n"); fflush(stdout);
	move_to(cx, cy + 46);

	printf("== release RIGHT ==\n"); fflush(stdout);
	zwlr_virtual_pointer_v1_button(vp, 1, BTN_RIGHT, 0);
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(display); rt();

	usleep(200000);
	return 0;
}
