/*
 * test-ime-app.c - real-app side of the fcitx5 end-to-end test
 *
 * Opens a toplevel with an enabled zwp_text_input_v3 (like foot/GTK), then
 * drives a zwlr_virtual_keyboard to type "ni " (pinyin).  If the input
 * method relay + fcitx5 work, the text input receives the committed
 * Chinese candidate "你" and this program prints it.
 *
 * Usage: run inside a compositor session; the real fcitx5 must be running.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

static int failures;
static void check(bool cond, const char *fmt, ...) {
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printf("%s %s\n", cond ? "PASS" : "FAIL", buf);
	if (!cond) failures++;
}

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct zwp_text_input_manager_v3 *ti_mgr;
static struct zwp_virtual_keyboard_manager_v1 *vkbd_mgr;

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static struct zwp_text_input_v3 *text_input;
static struct zwp_virtual_keyboard_v1 *vkbd;

static bool ti_entered;
static bool ti_preedit;
static bool ti_committed;
static char ti_commit[256];

static void handle_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = { .ping = handle_ping };

static void handle_xdg_configure(void *data, struct xdg_surface *s,
		uint32_t serial) {
	xdg_surface_ack_configure(s, serial);
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = handle_xdg_configure,
};

static void handle_ti_enter(void *data, struct zwp_text_input_v3 *ti,
		struct wl_surface *surf) {
	(void)data; (void)ti; (void)surf;
	ti_entered = true;
}
static void handle_ti_preedit(void *data, struct zwp_text_input_v3 *ti,
		const char *text, int32_t b, int32_t e) {
	(void)data; (void)ti; (void)b; (void)e;
	printf("  [app] preedit: '%s'\n", text ? text : "");
	ti_preedit = true;
}
static void handle_ti_commit_string(void *data, struct zwp_text_input_v3 *ti,
		const char *text) {
	(void)data; (void)ti;
	printf("  [app] commit: '%s'\n", text ? text : "");
	if (text) strncpy(ti_commit, text, sizeof(ti_commit) - 1);
	ti_committed = true;
}
static void handle_ti_leave(void *data, struct zwp_text_input_v3 *ti,
		struct wl_surface *surf) {
	(void)data; (void)ti; (void)surf;
}
static void handle_ti_delete(void *data, struct zwp_text_input_v3 *ti,
		uint32_t before, uint32_t after) {
	(void)data; (void)ti; (void)before; (void)after;
}
static void handle_ti_done(void *data, struct zwp_text_input_v3 *ti,
		uint32_t serial) {
	(void)data; (void)ti; (void)serial;
}
static const struct zwp_text_input_v3_listener ti_listener = {
	.enter = handle_ti_enter,
	.leave = handle_ti_leave,
	.preedit_string = handle_ti_preedit,
	.commit_string = handle_ti_commit_string,
	.delete_surrounding_text = handle_ti_delete,
	.done = handle_ti_done,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	(void)data; (void)version;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 2);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 4);
	} else if (strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0) {
		ti_mgr = wl_registry_bind(registry, name,
			&zwp_text_input_manager_v3_interface, 1);
	} else if (strcmp(interface,
			zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		vkbd_mgr = wl_registry_bind(registry, name,
			&zwp_virtual_keyboard_manager_v1_interface, 1);
	}
}
static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
};

static int roundtrip(void) {
	wl_display_flush(display);
	return wl_display_roundtrip(display);
}

/* commit a 1x1 buffer so the toplevel maps */
static void commit_buffer(void) {
	int fd = syscall(SYS_memfd_create, "buf", 0);
	if (fd < 0) { perror("memfd"); exit(1); }
	ftruncate(fd, 4);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, 4);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, 1, 1,
		4, WL_SHM_FORMAT_ARGB8888);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, 1, 1);
	wl_surface_commit(surface);
}

/* build an xkb keymap, ship it to the virtual keyboard */
static void vkbd_send_keymap(struct zwp_virtual_keyboard_v1 *vk) {
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	char *str = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	size_t len = strlen(str);
	int fd = syscall(SYS_memfd_create, "kmap", 0);
	ftruncate(fd, (off_t)len);
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	memcpy(p, str, len);
	munmap(p, len);
	zwp_virtual_keyboard_v1_keymap(vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
		fd, len);
	close(fd);
	xkb_keymap_unref(keymap);
	xkb_context_unref(ctx);
	free(str);
}

static void vkbd_type(uint32_t keycode) {
	static uint32_t time_msec;
	zwp_virtual_keyboard_v1_key(vkbd, ++time_msec, keycode,
		WL_KEYBOARD_KEY_STATE_PRESSED);
	roundtrip();
	zwp_virtual_keyboard_v1_key(vkbd, ++time_msec, keycode,
		WL_KEYBOARD_KEY_STATE_RELEASED);
	roundtrip();
}

int main(void) {
	display = wl_display_connect(NULL);
	check(display != NULL, "connect to compositor");
	if (!display) return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	roundtrip();
	check(compositor && wm_base && shm && seat && ti_mgr && vkbd_mgr,
		"globals (incl. virtual keyboard manager)");
	if (!ti_mgr || !vkbd_mgr) return 1;

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_set_title(toplevel, "ime-app");
	xdg_toplevel_set_app_id(toplevel, "ime-app");
	wl_surface_commit(surface);
	roundtrip();
	commit_buffer();
	roundtrip(); /* window is mapped and focused now */

	/* enable text input on the focused surface */
	/* create the (typing) virtual keyboard FIRST, like a real keyboard that
	 * exists before fcitx5 starts; fcitx5's own re-injection device comes
	 * later and must not become the seat keyboard */
	vkbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vkbd_mgr,
		seat);
	vkbd_send_keymap(vkbd);
	roundtrip();

	text_input = zwp_text_input_manager_v3_get_text_input(ti_mgr, seat);
	zwp_text_input_v3_add_listener(text_input, &ti_listener, NULL);
	zwp_text_input_v3_enable(text_input);
	zwp_text_input_v3_commit(text_input);
	roundtrip();
	usleep(300000); /* let fcitx5 activate + grab the keyboard */

	/* press Ctrl+Space: fcitx5's trigger key to activate the input method */
	zwp_virtual_keyboard_v1_key(vkbd, 100, 29, WL_KEYBOARD_KEY_STATE_PRESSED); /* LCtrl */
	vkbd_type(57); /* space */
	zwp_virtual_keyboard_v1_key(vkbd, 102, 29, WL_KEYBOARD_KEY_STATE_RELEASED);
	usleep(300000); /* let fcitx5 activate */

	/* type "ni " on the virtual keyboard */
	vkbd_type(49); /* n */
	vkbd_type(23); /* i */
	vkbd_type(57); /* space */
	usleep(500000); /* wait for fcitx5 to commit the candidate */
	wl_display_flush(display);
	wl_display_roundtrip(display); /* read the preedit/commit events */

	printf("entered=%d preedit=%d committed=%s\n", ti_entered, ti_preedit,
		ti_committed ? ti_commit : "(none)");
	check(ti_committed && strcmp(ti_commit, "\xE4\xBD\xA0") == 0,
		"fcitx5 committed '你' after typing 'ni '");

	wl_display_disconnect(display);
	printf(failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", failures);
	return failures == 0 ? 0 : 1;
}
