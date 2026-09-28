/* sanity test: two windows, focus switches between them (repaints the
 * server-side borders in focused/unfocused colors), then teardown */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#define WIN_W 200
#define WIN_H 100

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct xdg_wm_base *xdg_wm_base;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *wl_pointer;
static struct zwlr_virtual_pointer_manager_v1 *vp_mgr;
static struct zwlr_virtual_pointer_v1 *vp;
static struct wl_output *wl_output;
static int out_w = 0, out_h = 0;

struct win {
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	struct wl_buffer *buffer;
	bool configured;
};

static struct win wins[2];

static void handle_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = { .ping = handle_ping };

static void handle_xdg_configure(void *data, struct xdg_surface *s, uint32_t serial) {
	xdg_surface_ack_configure(s, serial);
	for (int i = 0; i < 2; i++) {
		if (wins[i].xdg_surface == s) wins[i].configured = true;
	}
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = handle_xdg_configure,
};
static void handle_toplevel_configure(void *data, struct xdg_toplevel *t,
		int32_t w, int32_t h, struct wl_array *states) { (void)states; }
static void handle_toplevel_close(void *data, struct xdg_toplevel *t) {}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = handle_toplevel_configure,
	.close = handle_toplevel_close,
};
static void handle_pointer_enter(void *data, struct wl_pointer *p,
		uint32_t serial, struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy) { (void)s; }
static void handle_pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {}
static void handle_pointer_motion(void *data, struct wl_pointer *p,
		uint32_t time, wl_fixed_t sx, wl_fixed_t sy) { (void)sx; (void)sy; }
static void handle_pointer_button(void *data, struct wl_pointer *p,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) { (void)state; }
static void handle_pointer_axis(void *data, struct wl_pointer *p,
		uint32_t time, uint32_t axis, wl_fixed_t value) {}
static void handle_pointer_frame(void *data, struct wl_pointer *p) {}
static void handle_pointer_axis_source(void *data, struct wl_pointer *p, uint32_t axis_source) {}
static void handle_pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis) {}
static void handle_pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete) {}
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

static void handle_output_geometry(void *data, struct wl_output *o, int x, int y,
		int phys_w, int phys_h, int subpixel, const char *make,
		const char *model, int transform) { (void)o; (void)x; (void)y; (void)phys_w;
	(void)phys_h; (void)subpixel; (void)make; (void)model; (void)transform; }
static void handle_output_mode(void *data, struct wl_output *o, uint32_t flags,
		int w, int h, int refresh) { (void)o; (void)flags; (void)refresh; if (w) { out_w = w; out_h = h; } }
static void handle_output_done(void *data, struct wl_output *o) { (void)o; }
static void handle_output_scale(void *data, struct wl_output *o, int32_t factor) { (void)o; (void)factor; }
static const struct wl_output_listener output_listener = {
	.geometry = handle_output_geometry,
	.mode = handle_output_mode,
	.done = handle_output_done,
	.scale = handle_output_scale,
};

static void handle_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_output_interface.name) == 0 && wl_output == NULL) {
		wl_output = wl_registry_bind(registry, name, &wl_output_interface, 1);
		wl_output_add_listener(wl_output, &output_listener, NULL);
	} else if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 2);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 4);
	} else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
		vp_mgr = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
	}
}
static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = NULL,
};

static void roundtrip(void) { wl_display_flush(display); wl_display_roundtrip(display); }

static struct wl_buffer *make_buffer(int w, int h) {
	int stride = w * 4;
	int fd = syscall(SYS_memfd_create, "buf", 0);
	if (fd < 0) { perror("memfd"); exit(1); }
	ftruncate(fd, stride * h);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, stride * h);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h,
		stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

static void create_window(int idx) {
	struct win *w = &wins[idx];
	w->surface = wl_compositor_create_surface(compositor);
	w->xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, w->surface);
	xdg_surface_add_listener(w->xdg_surface, &xdg_surface_listener, NULL);
	w->xdg_toplevel = xdg_surface_get_toplevel(w->xdg_surface);
	xdg_toplevel_add_listener(w->xdg_toplevel, &toplevel_listener, NULL);
	wl_surface_commit(w->surface);
	roundtrip();
	for (int i = 0; i < 20 && !w->configured; i++) roundtrip();
	w->buffer = make_buffer(WIN_W, WIN_H);
	wl_surface_attach(w->surface, w->buffer, 0, 0);
	wl_surface_commit(w->surface);
	roundtrip(); roundtrip();
}

static int px = 0, py = 0;

static void move_abs(int x, int y) {
	zwlr_virtual_pointer_v1_motion_absolute(vp, 1, x, y, out_w, out_h);
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(display);
	roundtrip();
	px = x; py = y;
}

static void click(void) {
	zwlr_virtual_pointer_v1_button(vp, 1, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
	zwlr_virtual_pointer_v1_frame(vp);
	zwlr_virtual_pointer_v1_button(vp, 1, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(display);
	roundtrip();
}

int main(void) {
	display = wl_display_connect(NULL);
	if (!display) { fprintf(stderr, "no display\n"); return 1; }
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	roundtrip();
	if (!compositor || !xdg_wm_base || !shm || !seat || !vp_mgr) return 1;
	wl_pointer = wl_seat_get_pointer(seat);
	wl_pointer_add_listener(wl_pointer, &pointer_listener, NULL);
	vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(vp_mgr, seat);
	roundtrip();
	for (int i = 0; i < 20 && out_w == 0; i++) roundtrip();
	if (out_w == 0) return 1;
	printf("output %dx%d\n", out_w, out_h);

	int cx = out_w / 2, cy = out_h / 2;
	move_abs(cx, cy);
	create_window(0);          /* A centered at (cx,cy): (cx-100,cy-50)-(cx+100,cy+50) */
	move_abs(cx + 300, cy);    /* park pointer away from A */
	create_window(1);          /* B centered at (cx+300,cy) */
	move_abs(cx + 300, cy + 40); /* click B body -> focus B */
	click();
	move_abs(cx, cy + 40);       /* click A body -> focus A */
	click();
	move_abs(cx + 300, cy + 40); /* focus B again */
	click();
	printf("done: focus switches completed without crash\n");
	fflush(stdout);
	usleep(200000);
	return 0;
}
