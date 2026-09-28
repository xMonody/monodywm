/* resize-cursor test: drive a virtual pointer through an edge-resize drag
 * while the client honors configure sizes (real resize behavior) and
 * requests a text cursor on enter.  The compositor logs every cursor
 * decision with WLR_DEBUG; the shell script greps for the resize cursor
 * being re-asserted or replaced mid-drag. */
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

static struct wl_output *wl_output;
static int out_w = 0, out_h = 0;

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

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *xdg_toplevel;

static struct wl_surface *cursor_surface;
static struct wl_buffer *cursor_buffer;
static bool xdg_configured;
static int pending_w, pending_h;
static struct wl_buffer *current_buffer;
static int current_buf_w, current_buf_h;

static struct wl_buffer *make_buffer(int w, int h);

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

static void recreate_buffer(int w, int h) {
	if (current_buffer != NULL) {
		wl_buffer_destroy(current_buffer);
	}
	current_buffer = make_buffer(w, h);
	current_buf_w = w;
	current_buf_h = h;
	wl_surface_attach(surface, current_buffer, 0, 0);
	wl_surface_commit(surface);
}

/* honor the resize configure like a real terminal does */
static void handle_toplevel_configure(void *data, struct xdg_toplevel *t,
		int32_t w, int32_t h, struct wl_array *states) {
	(void)states;
	if (w > 0 && h > 0) {
		pending_w = w;
		pending_h = h;
		recreate_buffer(w, h);
		/* a GTK/terminal that re-asserts its cursor on every redraw */
		if (cursor_surface != NULL && cursor_buffer != NULL) {
			wl_pointer_set_cursor(wl_pointer, 0, cursor_surface, 4, 4);
		}
	}
}
static void handle_toplevel_close(void *data, struct xdg_toplevel *t) {}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = handle_toplevel_configure,
	.close = handle_toplevel_close,
};

static void handle_pointer_enter(void *data, struct wl_pointer *p,
		uint32_t serial, struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy) {
	(void)s; (void)sx; (void)sy;
	if (cursor_surface != NULL && cursor_buffer != NULL) {
		wl_pointer_set_cursor(p, serial, cursor_surface, 4, 4);
	}
}
static void handle_pointer_leave(void *data, struct wl_pointer *p,
		uint32_t serial, struct wl_surface *s) { (void)s; }
static void handle_pointer_motion(void *data, struct wl_pointer *p,
		uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
	/* a real client re-asserts its cursor on motion inside the window */
	if (cursor_surface != NULL && cursor_buffer != NULL) {
		/* no serial available here; terminals set the cursor on enter only */
	}
}
static void handle_pointer_button(void *data, struct wl_pointer *p,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED) {
		/* mimic GTK/terminals that refresh their cursor on button press */
		if (cursor_surface != NULL && cursor_buffer != NULL) {
			wl_pointer_set_cursor(p, serial, cursor_surface, 4, 4);
		}
	}
}
static void handle_pointer_axis(void *data, struct wl_pointer *p,
		uint32_t time, uint32_t axis, wl_fixed_t value) { (void)axis; (void)value; }
static void handle_pointer_frame(void *data, struct wl_pointer *p) {}
static void handle_pointer_axis_source(void *data, struct wl_pointer *p,
		uint32_t axis_source) {}
static void handle_pointer_axis_stop(void *data, struct wl_pointer *p,
		uint32_t time, uint32_t axis) {}
static void handle_pointer_axis_discrete(void *data, struct wl_pointer *p,
		uint32_t axis, int32_t discrete) {}
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
		int w, int h, int refresh) {
	(void)o; (void)flags; (void)refresh;
	if (w > 0 && h > 0) { out_w = w; out_h = h; }
}
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
		vp_mgr = wl_registry_bind(registry, name,
			&zwlr_virtual_pointer_manager_v1_interface, 1);
	}
}
static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = NULL,
};

static void roundtrip(void) {
	wl_display_flush(display);
	wl_display_roundtrip(display);
}

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

static void make_cursor_surface(void) {
	cursor_surface = wl_compositor_create_surface(compositor);
	cursor_buffer = make_buffer(8, 8);
	wl_surface_attach(cursor_surface, cursor_buffer, 0, 0);
	wl_surface_commit(cursor_surface);
}

static void create_window(void) {
	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(xdg_toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(xdg_toplevel, "resize-cursor-test");
	wl_surface_commit(surface);
	roundtrip();
	for (int i = 0; i < 20 && !xdg_configured; i++) {
		roundtrip();
	}
	if (!xdg_configured) {
		fprintf(stderr, "initial configure never arrived\n");
		exit(1);
	}
	current_buffer = make_buffer(WIN_W, WIN_H);
	current_buf_w = WIN_W;
	current_buf_h = WIN_H;
	wl_surface_attach(surface, current_buffer, 0, 0);
	wl_surface_commit(surface);
	roundtrip();
	roundtrip();
}

static int px = 100, py = 100;

static void move_to(int x, int y) {
	int dx = x - px, dy = y - py;
	px = x; py = y;
	if (dx != 0 || dy != 0) {
		zwlr_virtual_pointer_v1_motion(vp, 1,
			wl_fixed_from_int(dx), wl_fixed_from_int(dy));
		zwlr_virtual_pointer_v1_frame(vp);
	}
	wl_display_flush(display);
	roundtrip();
}

static void press_left(void) {
	zwlr_virtual_pointer_v1_button(vp, 1, BTN_LEFT,
		WL_POINTER_BUTTON_STATE_PRESSED);
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(display);
	roundtrip();
}

static void release_left(void) {
	zwlr_virtual_pointer_v1_button(vp, 1, BTN_LEFT,
		WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(display);
	roundtrip();
}

static void say(const char *what) {
	printf("[pointer now over: %s]\n", what);
	fflush(stdout);
}

int main(void) {
	display = wl_display_connect(NULL);
	if (!display) { fprintf(stderr, "no display\n"); return 1; }
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	roundtrip();
	if (!compositor || !xdg_wm_base || !shm || !seat || !vp_mgr) {
		fprintf(stderr, "missing globals\n");
		return 1;
	}
	wl_pointer = wl_seat_get_pointer(seat);
	wl_pointer_add_listener(wl_pointer, &pointer_listener, NULL);
	vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(vp_mgr, seat);
	roundtrip();
	make_cursor_surface();

	for (int i = 0; i < 20 && out_w == 0; i++) {
		roundtrip();
	}
	if (out_w == 0) { fprintf(stderr, "no output size\n"); return 1; }
	printf("output %dx%d\n", out_w, out_h);

	int cx = out_w / 2, cy = out_h / 2;
	zwlr_virtual_pointer_v1_motion_absolute(vp, 1, cx, cy, out_w, out_h);
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(display);
	roundtrip();
	px = cx; py = cy;

	create_window(); /* 200x100 centered at (cx,cy) */

	/* ---- right-edge resize drag in 8 px steps ---- */
	say("right edge");
	move_to(cx + 102, cy); /* zone: (740,744] */
	press_left();
	say("resize drag right, step 1");
	move_to(cx + 110, cy);
	say("resize drag right, step 2");
	move_to(cx + 118, cy);
	say("resize drag right, step 3");
	move_to(cx + 126, cy);
	say("resize drag right, step 4");
	move_to(cx + 134, cy);
	say("resize drag right, step 5");
	move_to(cx + 142, cy);
	release_left();
	say("released (window should now be ~294px wide)");

	/* ---- bottom-edge resize drag ---- */
	say("bottom edge");
	move_to(cx, cy + 52); /* zone: (406,414] */
	press_left();
	say("resize drag down");
	move_to(cx, cy + 60);
	move_to(cx, cy + 68);
	move_to(cx, cy + 76);
	move_to(cx, cy + 84);
	release_left();
	say("released");

	/* ---- left-edge resize drag (node reposition path) ---- */
	say("left edge");
	move_to(cx - 100, cy); /* zone: [536,544) */
	press_left();
	say("resize drag left");
	move_to(cx - 108, cy);
	move_to(cx - 116, cy);
	move_to(cx - 124, cy);
	move_to(cx - 132, cy);
	release_left();
	say("released");

	usleep(300000);
	printf("done\n");
	return 0;
}
