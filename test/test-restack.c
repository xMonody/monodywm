/* restack test: verify that a subsurface restack (wl_subsurface.place_below /
 * place_above) re-renders the rounded FBO even though the restack itself
 * attaches no buffer damage.
 *
 * place_above/place_below are applied on the *parent* surface's commit and
 * carry no damage, so the damage-driven FBO cache must detect the changed
 * subsurface stacking order on its own (rounded.c snapshots the order at
 * publish time and compares it on commit).
 *
 * Scenario (compositor logs every publish with WLR_DEBUG
 * "rounded: published FBO ..."):
 *   1. attach main buffer + damage        -> re-renders (content)
 *   2. add two subsurfaces, commit parent -> re-renders (new subsurfaces)
 *   3. place_below(sub2, parent) + commit parent, NO damage
 *                                          -> re-renders (order changed)
 * The shell script counts the publish lines: exactly 3.
 *
 * New subsurfaces are created in synchronized mode, so their buffers only
 * become visible on the parent commit in step 2; step 3 then restacks one of
 * the already-visible subsurfaces, which is the actual reorder under test.
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct xdg_wm_base *xdg_wm_base;
static struct wl_shm *shm;
static struct wl_subcompositor *subcompositor;

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *xdg_toplevel;
static bool xdg_configured;

static struct wl_surface *sub1_surface, *sub2_surface;
static struct wl_subsurface *sub1, *sub2;

static struct wl_buffer *buf_a, *buf_s1, *buf_s2;

static void handle_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = handle_ping,
};

static void handle_xdg_configure(void *data, struct xdg_surface *s,
		uint32_t serial) {
	xdg_surface_ack_configure(s, serial);
	xdg_configured = true;
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = handle_xdg_configure,
};

static void handle_toplevel_configure(void *data, struct xdg_toplevel *t,
		int32_t w, int32_t h, struct wl_array *states) {
	(void)states;
}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = handle_toplevel_configure,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 4);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		xdg_wm_base = wl_registry_bind(registry, name,
			&xdg_wm_base_interface, 2);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
		subcompositor = wl_registry_bind(registry, name,
			&wl_subcompositor_interface, 1);
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
	if (fd < 0) {
		perror("memfd");
		exit(1);
	}
	ftruncate(fd, stride * h);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, stride * h);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h,
		stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

static void add_subsurface(struct wl_surface **out_surface,
		struct wl_subsurface **out_sub, struct wl_buffer *buf,
		int x, int y) {
	*out_surface = wl_compositor_create_surface(compositor);
	*out_sub = wl_subcompositor_get_subsurface(subcompositor, *out_surface,
		surface);
	wl_subsurface_set_position(*out_sub, x, y);
	wl_surface_attach(*out_surface, buf, 0, 0);
	wl_surface_damage(*out_surface, 0, 0, 80, 40);
	wl_surface_commit(*out_surface);
}

int main(void) {
	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "no display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	roundtrip();
	if (!compositor || !xdg_wm_base || !shm || !subcompositor) {
		fprintf(stderr, "missing globals\n");
		return 1;
	}

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(xdg_toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(xdg_toplevel, "restack-test");
	wl_surface_commit(surface); /* initial commit (no buffer) */
	roundtrip();
	for (int i = 0; i < 20 && !xdg_configured; i++) {
		roundtrip();
	}
	if (!xdg_configured) {
		fprintf(stderr, "initial configure never arrived\n");
		return 1;
	}

	buf_a = make_buffer(200, 100);
	buf_s1 = make_buffer(80, 40);
	buf_s2 = make_buffer(80, 40);

	printf("step 1: attach main buffer + damage\n");
	wl_surface_attach(surface, buf_a, 0, 0);
	wl_surface_damage(surface, 0, 0, 200, 100);
	wl_surface_commit(surface);
	roundtrip();
	roundtrip();
	usleep(50000);

	printf("step 2: add two subsurfaces, commit parent\n");
	add_subsurface(&sub1_surface, &sub1, buf_s1, 10, 10);
	add_subsurface(&sub2_surface, &sub2, buf_s2, 20, 20);
	wl_surface_commit(surface); /* apply the cached subsurface commits */
	roundtrip();
	roundtrip();
	usleep(50000);

	printf("step 3: restack sub2 below parent, commit parent (no damage)\n");
	wl_subsurface_place_below(sub2, surface);
	wl_surface_commit(surface); /* restack, no damage attached */
	roundtrip();
	roundtrip();

	usleep(200000);
	printf("done\n");
	return 0;
}
