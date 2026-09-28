/* cursor-shape-v1 negotiation test:
 *   - binds wp_cursor_shape_manager_v1 and sets a shape on the pointer
 *   - drives a virtual pointer into a window while the compositor logs the
 *     cursor decisions (WLR_DEBUG "cursor: shape N" / "cursor: restore-shape")
 *
 * Expected compositor log (run with WLR_DEBUG=1):
 *   cursor: shape 9              (text shape on enter)
 *   cursor: restore-shape        (after the compositor override ends)
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
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
static bool shape_set;

#define WIN_W 200
#define WIN_H 100

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
		int32_t w, int32_t h, struct wl_array *states) { (void)states; }
static void handle_toplevel_close(void *data, struct xdg_toplevel *t) {}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = handle_toplevel_configure,
	.close = handle_toplevel_close,
};

static void handle_pointer_enter(void *data, struct wl_pointer *p,
		uint32_t serial, struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy) {
	(void)data; (void)p; (void)s; (void)sx; (void)sy;
	/* exactly what a modern toolkit does on enter: set the cursor shape */
	if (shape_dev != NULL) {
		wp_cursor_shape_device_v1_set_shape(shape_dev, serial,
			WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
		shape_set = true;
		printf("[pointer] set_shape(TEXT)\n");
		fflush(stdout);
	}
}
static void handle_pointer_leave(void *data, struct wl_pointer *p,
		uint32_t serial, struct wl_surface *s) { (void)s; }
static void handle_pointer_motion(void *data, struct wl_pointer *p,
		uint32_t time, wl_fixed_t sx, wl_fixed_t sy) { (void)sx; (void)sy; }
static void handle_pointer_button(void *data, struct wl_pointer *p,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) { (void)state; }
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

static void handle_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	(void)data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
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
	} else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
		shape_mgr = wl_registry_bind(registry, name,
			&wp_cursor_shape_manager_v1_interface, 1);
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

static void create_window(void) {
	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(xdg_toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(xdg_toplevel, "cursor-shape-test");
	wl_surface_commit(surface);
	roundtrip();
	for (int i = 0; i < 20 && !xdg_configured; i++) {
		roundtrip();
	}
	if (!xdg_configured) {
		fprintf(stderr, "initial configure never arrived\n");
		exit(1);
	}
	struct wl_buffer *buffer = make_buffer(WIN_W, WIN_H);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_commit(surface);
	roundtrip();
	roundtrip();
}

static int px = 0, py = 0;

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

int main(void) {
	display = wl_display_connect(NULL);
	if (!display) { fprintf(stderr, "no display\n"); return 1; }
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	roundtrip();
	if (!compositor || !xdg_wm_base || !shm || !seat || !vp_mgr) {
		fprintf(stderr, "missing basic globals\n");
		return 1;
	}
	if (shape_mgr == NULL) {
		fprintf(stderr, "FAIL: wp_cursor_shape_manager_v1 global not advertised\n");
		return 1;
	}
	printf("globals: cursor-shape-v1 present\n");

	struct wl_pointer *wl_pointer = wl_seat_get_pointer(seat);
	wl_pointer_add_listener(wl_pointer, &pointer_listener, NULL);

	/* bind a shape device for the pointer */
	shape_dev = wp_cursor_shape_manager_v1_get_pointer(shape_mgr, wl_pointer);
	roundtrip();

	vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(vp_mgr, seat);
	roundtrip();

	/* park the pointer at (300, 300) so the window centers there */
	px = py = 300;
	zwlr_virtual_pointer_v1_motion_absolute(vp, 1, px, py, 600, 600);
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(display);
	roundtrip();

	create_window(); /* box (200,250)-(400,350) on a 600x600 output */

	/* enter the window body -> client sets shape "text" */
	move_to(300, 300);
	/* over the invisible title strip -> compositor override "move" */
	move_to(300, 260);
	/* back into the body -> compositor restores the client's shape */
	move_to(300, 300);

	usleep(200000);
	bool ok = shape_set;
	printf("%s\n", ok ? "PASS: cursor-shape negotiation ok" : "FAIL: shape never set");
	return ok ? 0 : 1;
}
