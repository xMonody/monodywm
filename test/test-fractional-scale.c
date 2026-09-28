/* fractional-scale pixel-alignment test client
 *
 * Ports the client half of Wayfire's scaling test
 * (commit 35f8f2ca7ae1c52ee843ffb6c1e21e9ec5447dc1, PR #3032) to monodywm:
 *
 *   - bind wp_fractional_scale_v1 + wp_viewporter
 *   - let the compositor advertise the exact fractional scale (e.g. 5/3 or 7/4)
 *   - attach a buffer whose physical size is round(logical * scale), with
 *     wl_surface.set_buffer_scale(1) and wp_viewport.set_destination(logical)
 *   - capture the output through wlr-screencopy (raw physical pixels, no
 *     grim rescaling) and check the red/green checkerboard for blended pixels
 *
 * Prints one machine-readable line once the checkerboard is committed:
 *   READY logical=WxH buffer=WxH frac=<u32> pref=<int>
 * then captures and prints the analysis, exiting 0 (clean) or 1 (blended).
 */
#define _GNU_SOURCE
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wp_fractional_scale_v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#define LOGICAL_W 24
#define LOGICAL_H 24
#define CELLS 6

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wp_fractional_scale_manager_v1 *frac_mgr;
static struct wp_viewporter *viewporter;
static struct zwlr_screencopy_manager_v1 *screencopy_mgr;
static struct wl_output *output;

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *xdg_toplevel;
static struct wp_fractional_scale_v1 *frac;
static struct wp_viewport *viewport;

static bool got_configure;
static int preferred_buffer_scale = 1;
static bool got_fractional_scale;
static uint32_t fractional_scale;

/* ---- registry ---- */

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	(void)data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, version < 6 ? version : 6);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(registry, name,
			&xdg_wm_base_interface, version < 7 ? version : 7);
	} else if (strcmp(interface,
			wp_fractional_scale_manager_v1_interface.name) == 0) {
		frac_mgr = wl_registry_bind(registry, name,
			&wp_fractional_scale_manager_v1_interface, 1);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		viewporter = wl_registry_bind(registry, name,
			&wp_viewporter_interface, 1);
	} else if (strcmp(interface,
			zwlr_screencopy_manager_v1_interface.name) == 0) {
		screencopy_mgr = wl_registry_bind(registry, name,
			&zwlr_screencopy_manager_v1_interface,
			version < 3 ? version : 3);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		if (output == NULL) {
			output = wl_registry_bind(registry, name,
				&wl_output_interface, 1);
		}
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	(void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ---- surface: preferred scale/tranform ---- */

static void surface_enter(void *data, struct wl_surface *s, struct wl_output *o) {
	(void)data; (void)s; (void)o;
}
static void surface_leave(void *data, struct wl_surface *s, struct wl_output *o) {
	(void)data; (void)s; (void)o;
}
static void surface_preferred_buffer_scale(void *data, struct wl_surface *s,
		int32_t factor) {
	(void)data; (void)s;
	preferred_buffer_scale = factor;
}
static void surface_preferred_buffer_transform(void *data, struct wl_surface *s,
		uint32_t transform) {
	(void)data; (void)s; (void)transform;
}

static const struct wl_surface_listener surface_listener = {
	.enter = surface_enter,
	.leave = surface_leave,
	.preferred_buffer_scale = surface_preferred_buffer_scale,
	.preferred_buffer_transform = surface_preferred_buffer_transform,
};

/* ---- xdg-shell ---- */

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	(void)data;
	xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *s,
		uint32_t serial) {
	(void)data;
	xdg_surface_ack_configure(s, serial);
	got_configure = true;
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *t,
		int32_t w, int32_t h, struct wl_array *states) {
	(void)data; (void)t; (void)w; (void)h; (void)states;
}
static void toplevel_close(void *data, struct xdg_toplevel *t) {
	(void)data; (void)t;
}
static void toplevel_configure_bounds(void *data, struct xdg_toplevel *t,
		int32_t w, int32_t h) {
	(void)data; (void)t; (void)w; (void)h;
}
static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *t,
		struct wl_array *caps) {
	(void)data; (void)t; (void)caps;
}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_configure_bounds,
	.wm_capabilities = toplevel_wm_capabilities,
};

/* ---- fractional scale ---- */

static void fractional_preferred_scale(void *data,
		struct wp_fractional_scale_v1 *obj, uint32_t scale) {
	(void)data; (void)obj;
	fractional_scale = scale;
	got_fractional_scale = true;
}
static const struct wp_fractional_scale_v1_listener fractional_listener = {
	.preferred_scale = fractional_preferred_scale,
};

/* ---- shm helpers ---- */

static struct wl_buffer *make_buffer(int w, int h, const uint32_t *pixels) {
	int stride = w * 4;
	int size = stride * h;
	int fd = memfd_create("monodywm-scale-test", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		perror("memfd");
		exit(1);
	}
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	memcpy(data, pixels, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	munmap(data, size);
	close(fd);
	return buffer;
}

static int round_half_away_from_zero(double v) {
	return (int)(v < 0 ? -(__builtin_floor(-v + 0.5))
		: __builtin_floor(v + 0.5));
}

/* red / green checkerboard; cell size = buffer / CELLS so the number of
 * physical cells stays CELLS across the logical size */
static struct wl_buffer *make_checkerboard(int bw, int bh) {
	uint32_t *px = malloc((size_t)bw * bh * 4);
	if (px == NULL) {
		exit(1);
	}
	int cw = bw / CELLS;
	int ch = bh / CELLS;
	if (cw < 1) cw = 1;
	if (ch < 1) ch = 1;
	for (int y = 0; y < bh; y++) {
		for (int x = 0; x < bw; x++) {
			uint32_t red = 0x00FF0000u;
			uint32_t green = 0x0000FF00u;
			px[y * bw + x] = (((x / cw) + (y / ch)) & 1) ? green : red;
		}
	}
	struct wl_buffer *buffer = make_buffer(bw, bh, px);
	free(px);
	return buffer;
}

static void attach_solid(int w, int h, uint32_t color) {
	uint32_t *px = malloc((size_t)w * h * 4);
	if (px == NULL) {
		exit(1);
	}
	for (int i = 0; i < w * h; i++) {
		px[i] = color;
	}
	struct wl_buffer *buffer = make_buffer(w, h, px);
	free(px);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, w, h);
	wl_surface_commit(surface);
	wl_display_flush(display);
}

/* block until pred() or the deadline; delivers server events */
static bool wait_for(bool (*pred)(void), int timeout_ms) {
	struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
	int elapsed = 0;
	while (!pred()) {
		if (elapsed >= timeout_ms) {
			return false;
		}
		int rc = poll(&pfd, 1, 20);
		if (rc > 0 && (pfd.revents & POLLIN)) {
			wl_display_dispatch(display);
		} else {
			wl_display_flush(display);
		}
		elapsed += 20;
	}
	return true;
}

static bool pred_configure(void) { return got_configure; }
static bool pred_fractional(void) { return got_fractional_scale; }

/* ---- screencopy: raw physical pixels ---- */

struct capture {
	int width, height, stride;
	uint32_t format;
	uint8_t *data;
	size_t size;
	bool ready;
	bool failed;
	struct wl_buffer *buffer;
};

static void frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
	struct capture *cap = data;
	cap->width = width;
	cap->height = height;
	cap->stride = stride;
	cap->format = format;
	cap->size = (size_t)stride * height;

	int fd = memfd_create("monodywm-frame", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, cap->size) < 0) {
		cap->failed = true;
		return;
	}
	cap->data = mmap(NULL, cap->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (cap->data == MAP_FAILED) {
		cap->data = NULL;
		cap->failed = true;
		close(fd);
		return;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, cap->size);
	cap->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
		format);
	wl_shm_pool_destroy(pool);
	close(fd);
	zwlr_screencopy_frame_v1_copy(frame, cap->buffer);
}

static void frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t flags) {
	(void)data; (void)frame; (void)flags;
}
static void frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
	(void)frame; (void)tv_sec_hi; (void)tv_sec_lo; (void)tv_nsec;
	((struct capture *)data)->ready = true;
}
static void frame_failed(void *data, struct zwlr_screencopy_frame_v1 *frame) {
	(void)frame;
	((struct capture *)data)->failed = true;
}
static void frame_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *f,
		uint32_t format, uint32_t w, uint32_t h) {
	(void)data; (void)f; (void)format; (void)w; (void)h;
}
static void frame_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *f) {
	(void)data; (void)f;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = frame_buffer,
	.flags = frame_flags,
	.ready = frame_ready,
	.failed = frame_failed,
	.linux_dmabuf = frame_linux_dmabuf,
	.buffer_done = frame_buffer_done,
};

static struct capture *g_cap;

static bool pred_capture(void) {
	return g_cap->ready || g_cap->failed;
}

/* Analyse the captured physical pixels for a red/green checkerboard.
 * Returns 0 if it is sampled 1:1 (no blended pixels), 1 otherwise. */
static int analyze(struct capture *cap) {
	/* wl_shm ARGB8888/XRGB8888 are little-endian BGRA/BGRX, ABGR8888/XBGR8888
	 * are RGBA/RGBX.  Figure out the channel byte offsets from the format. */
	int r_off, g_off, b_off;
	switch (cap->format) {
	case WL_SHM_FORMAT_ARGB8888:
	case WL_SHM_FORMAT_XRGB8888:
		r_off = 2; g_off = 1; b_off = 0;
		break;
	case WL_SHM_FORMAT_ABGR8888:
	case WL_SHM_FORMAT_XBGR8888:
		r_off = 0; g_off = 1; b_off = 2;
		break;
	default:
		fprintf(stderr, "unexpected screencopy format 0x%x\n", cap->format);
		return 2;
	}

	int w = cap->width, h = cap->height;
	int min_x = w, min_y = h, max_x = -1, max_y = -1;
	long red = 0, green = 0;
	for (int y = 0; y < h; y++) {
		const uint8_t *row = cap->data + (size_t)y * cap->stride;
		for (int x = 0; x < w; x++) {
			const uint8_t *p = row + (size_t)x * 4;
			int r = p[r_off], g = p[g_off], b = p[b_off];
			bool is_red = r >= 200 && g <= 48 && b <= 48;
			bool is_green = g >= 200 && r <= 48 && b <= 48;
			if (is_red || is_green) {
				red += is_red; green += is_green;
				if (x < min_x) min_x = x;
				if (y < min_y) min_y = y;
				if (x > max_x) max_x = x;
				if (y > max_y) max_y = y;
			}
		}
	}
	if (max_x < 0) {
		printf("  no checkerboard pixels found (window not visible?)\n");
		return 2;
	}
	int bw = max_x - min_x + 1, bh = max_y - min_y + 1;
	long blend = 0;
	for (int y = min_y; y <= max_y; y++) {
		const uint8_t *row = cap->data + (size_t)y * cap->stride;
		for (int x = min_x; x <= max_x; x++) {
			const uint8_t *p = row + (size_t)x * 4;
			int r = p[r_off], g = p[g_off], b = p[b_off];
			if (r >= 48 && g >= 48 && b <= 32) {
				blend++;
			}
		}
	}
	long total = (long)bw * bh;
	printf("  capture: %dx%d  checker bbox: x=[%d..%d] y=[%d..%d] (%dx%d) "
		"red=%ld green=%ld\n", w, h, min_x, max_x, min_y, max_y, bw, bh,
		red, green);
	printf("  red/green blend pixels inside bbox: %ld / %ld (%.1f%%)\n",
		blend, total, 100.0 * blend / total);
	if (blend == 0) {
		printf("  PASS: checkerboard is sampled 1:1, no fractional-scale "
			"blending\n");
		return 0;
	}
	printf("  FAIL: checkerboard shows blended pixels -> window is "
		"resampled\n");
	return 1;
}

int main(void) {
	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "no display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);

	if (compositor == NULL || shm == NULL || wm_base == NULL ||
			frac_mgr == NULL || viewporter == NULL ||
			screencopy_mgr == NULL || output == NULL) {
		fprintf(stderr, "missing globals: compositor=%p shm=%p xdg=%p "
			"frac=%p viewporter=%p screencopy=%p output=%p\n",
			(void *)compositor, (void *)shm, (void *)wm_base,
			(void *)frac_mgr, (void *)viewporter,
			(void *)screencopy_mgr, (void *)output);
		return 2;
	}

	surface = wl_compositor_create_surface(compositor);
	wl_surface_add_listener(surface, &surface_listener, NULL);
	frac = wp_fractional_scale_manager_v1_get_fractional_scale(frac_mgr, surface);
	wp_fractional_scale_v1_add_listener(frac, &fractional_listener, NULL);
	viewport = wp_viewporter_get_viewport(viewporter, surface);

	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(xdg_toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(xdg_toplevel, "fractional-scale-test");
	xdg_toplevel_set_app_id(xdg_toplevel, "fractional-scale-test");
	wl_surface_commit(surface);
	wl_display_flush(display);

	if (!wait_for(pred_configure, 3000)) {
		fprintf(stderr, "no xdg configure\n");
		return 1;
	}

	/* first (fractional-unaware) buffer: maps the surface so the compositor
	 * starts advertising the fractional scale */
	attach_solid(LOGICAL_W, LOGICAL_H, 0x00336699u);

	if (!wait_for(pred_fractional, 3000)) {
		fprintf(stderr, "no wp_fractional_scale_v1.preferred_scale\n");
		return 1;
	}

	double scale = (double)fractional_scale / 120.0;
	int bw = round_half_away_from_zero(LOGICAL_W * scale);
	int bh = round_half_away_from_zero(LOGICAL_H * scale);
	struct wl_buffer *buffer = make_checkerboard(bw, bh);

	/* Wayfire's attach_with_fractional_scale(): buffer_scale=1 plus an exact
	 * viewport destination, buffer sized in physical pixels */
	wl_surface_set_buffer_scale(surface, 1);
	wp_viewport_set_destination(viewport, LOGICAL_W, LOGICAL_H);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, bw, bh);
	wl_surface_commit(surface);
	wl_display_flush(display);
	wl_display_roundtrip(display);

	printf("READY logical=%dx%d buffer=%dx%d frac=%u pref=%d\n",
		LOGICAL_W, LOGICAL_H, bw, bh, fractional_scale,
		preferred_buffer_scale);
	fflush(stdout);

	/* let the open animation settle before sampling */
	struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
	for (int i = 0; i < 120; i++) {
		int rc = poll(&pfd, 1, 10);
		if (rc > 0 && (pfd.revents & POLLIN)) {
			wl_display_dispatch(display);
		}
	}

	struct capture cap = {0};
	g_cap = &cap;
	struct zwlr_screencopy_frame_v1 *frame =
		zwlr_screencopy_manager_v1_capture_output(screencopy_mgr, 0, output);
	zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, &cap);
	if (!wait_for(pred_capture, 5000)) {
		fprintf(stderr, "screencopy timed out\n");
		return 2;
	}
	if (cap.failed) {
		fprintf(stderr, "screencopy failed\n");
		return 2;
	}
	return analyze(&cap);
}
