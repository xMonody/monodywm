/* mask-guard test: verify the FBO re-render only runs when the commit
 * actually changed the content.
 *
 * Scenario (virtual pointer parked away from the window; the compositor
 * logs every publish with WLR_DEBUG "rounded: published FBO ..."):
 *   1. attach buffer A (200x100) + damage   -> re-renders (content)
 *   2. commit with NO attach and NO damage  -> MUST NOT re-render
 *   3. attach buffer A again + damage       -> re-renders (in-place update)
 *   4. attach buffer B (different size)     -> re-renders (new buffer)
 * The shell script counts the publish lines: exactly 3.  The sleeps after
 * each dirty step keep every commit in its own frame, because the
 * damage-driven renderer legitimately batches commits that land in the
 * same frame window into one re-render.
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

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *xdg_toplevel;
static bool xdg_configured;
static struct wl_buffer *buf_a, *buf_b;

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

static void handle_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 2);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
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

int main(void) {
	display = wl_display_connect(NULL);
	if (!display) { fprintf(stderr, "no display\n"); return 1; }
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	roundtrip();
	if (!compositor || !xdg_wm_base || !shm) {
		fprintf(stderr, "missing globals\n");
		return 1;
	}

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(xdg_toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(xdg_toplevel, "mask-guard-test");
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
	buf_b = make_buffer(240, 120);

	printf("step 1: attach buffer A + damage\n");
	wl_surface_attach(surface, buf_a, 0, 0);
	wl_surface_damage(surface, 0, 0, 200, 100);
	wl_surface_commit(surface);
	roundtrip();
	roundtrip();
	usleep(50000);

	printf("step 2: state-only commit (no attach, no damage)\n");
	wl_surface_commit(surface);
	roundtrip();
	roundtrip();
	usleep(50000);

	printf("step 3: re-attach buffer A + damage\n");
	wl_surface_attach(surface, buf_a, 0, 0);
	wl_surface_damage(surface, 0, 0, 200, 100);
	wl_surface_commit(surface);
	roundtrip();
	roundtrip();
	usleep(50000);

	printf("step 4: attach buffer B (new size)\n");
	wl_surface_attach(surface, buf_b, 0, 0);
	wl_surface_damage(surface, 0, 0, 240, 120);
	wl_surface_commit(surface);
	roundtrip();
	roundtrip();

	usleep(200000);
	printf("done\n");
	return 0;
}
