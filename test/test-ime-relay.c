/*
 * ime-relay-test.c - exercises the input method relay end to end
 *
 * One process, two Wayland connections:
 *   - the "app"  : xdg toplevel + zwp_text_input_v3 (like a GTK/Qt app)
 *   - the "IM"   : zwp_input_method_v2 (like fcitx5)
 *
 * Flow under test: app maps a window (compositor focuses it, activates the
 * IM) -> IM sends preedit + commit string -> the app's text input must
 * receive enter, preedit and the commit string.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "input-method-unstable-v2-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include <xkbcommon/xkbcommon.h>

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

/* ============================ app side ============================ */

static struct wl_display *app_display;
static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct wl_shm *shm;
static struct zwp_text_input_manager_v3 *ti_mgr;
static struct zwp_virtual_keyboard_manager_v1 *vkbd_mgr;
static struct wl_seat *app_seat;
static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static struct zwp_text_input_v3 *text_input;

static bool got_configure;
static bool ti_entered;
static bool ti_left;
static bool ti_preedit;
static bool ti_committed;
static char ti_commit[256];

static void handle_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = { .ping = handle_ping };

static void handle_configure(void *data, struct xdg_surface *s, uint32_t serial) {
	xdg_surface_ack_configure(s, serial);
	got_configure = true;
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = handle_configure,
};

static void handle_toplevel_configure(void *data, struct xdg_toplevel *t,
		int32_t w, int32_t h, struct wl_array *states) {
	(void)data; (void)t; (void)w; (void)h; (void)states;
}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = handle_toplevel_configure,
};

/* create a virtual keyboard with a keymap; the compositor attaches it to
 * the seat, which also connects any pending IM keyboard grab to it */
static struct zwp_virtual_keyboard_v1 *make_vkbd(void) {
	struct zwp_virtual_keyboard_v1 *vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
		vkbd_mgr, app_seat);
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	char *str = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
	size_t len = strlen(str);
	int fd = syscall(SYS_memfd_create, "kmap", 0);
	ftruncate(fd, (off_t)len);
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	memcpy(p, str, len);
	munmap(p, len);
	zwp_virtual_keyboard_v1_keymap(vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
		fd, len);
	close(fd);
	xkb_keymap_unref(km);
	xkb_context_unref(ctx);
	free(str);
	return vk;
}

static void vkbd_key(struct zwp_virtual_keyboard_v1 *vk, uint32_t keycode) {
	zwp_virtual_keyboard_v1_key(vk, 1, keycode, WL_KEYBOARD_KEY_STATE_PRESSED);
	zwp_virtual_keyboard_v1_key(vk, 2, keycode, WL_KEYBOARD_KEY_STATE_RELEASED);
}

/* commit a 1x1 ARGB buffer so the toplevel actually maps */
static void commit_buffer(void) {
	int stride = 4;
	int fd = syscall(SYS_memfd_create, "buf", 0);
	if (fd < 0) { perror("memfd"); exit(1); }
	ftruncate(fd, stride);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, stride);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, 1, 1,
		stride, WL_SHM_FORMAT_ARGB8888);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, 1, 1);
	wl_surface_commit(surface);
}

static void handle_ti_enter(void *data, struct zwp_text_input_v3 *ti,
		struct wl_surface *surf) {
	(void)data; (void)ti; (void)surf;
	ti_entered = true;
}
static void handle_ti_leave(void *data, struct zwp_text_input_v3 *ti,
		struct wl_surface *surf) {
	(void)data; (void)ti; (void)surf;
	ti_left = true;
}
static void handle_ti_preedit(void *data, struct zwp_text_input_v3 *ti,
		const char *text, int32_t cursor_begin, int32_t cursor_end) {
	(void)data; (void)ti; (void)cursor_begin; (void)cursor_end;
	printf("  [app] preedit: '%s'\n", text);
	ti_preedit = true;
}
static void handle_ti_commit_string(void *data, struct zwp_text_input_v3 *ti,
		const char *text) {
	(void)data; (void)ti;
	printf("  [app] commit string: '%s'\n", text);
	strncpy(ti_commit, text ? text : "", sizeof(ti_commit) - 1);
	ti_committed = true;
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
		app_seat = wl_registry_bind(registry, name, &wl_seat_interface, 4);
	} else if (strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0) {
		ti_mgr = wl_registry_bind(registry, name,
			&zwp_text_input_manager_v3_interface, 1);
	} else if (strcmp(interface,
			zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		vkbd_mgr = wl_registry_bind(registry, name,
			&zwp_virtual_keyboard_manager_v1_interface, 1);
	}
}
static void handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	(void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

/* ============================= IM side ============================ */

static struct wl_display *im_display;
static struct zwp_input_method_manager_v2 *im_mgr;
static struct wl_seat *im_seat;
static struct zwp_input_method_v2 *im;
static struct zwp_input_method_keyboard_grab_v2 *im_grab;
static uint32_t im_done_count;
static bool im_activated;
static uint32_t im_got_key; /* keycode of the key received via the grab */
static bool im_got_keymap;

static void handle_im_activate(void *data, struct zwp_input_method_v2 *im_) {
	(void)data; (void)im_;
	printf("  [im] activate\n");
	im_activated = true;
}
static void handle_im_deactivate(void *data, struct zwp_input_method_v2 *im_) {
	(void)data; (void)im_;
	printf("  [im] deactivate\n");
	im_activated = false;
}
static void handle_im_surrounding(void *data, struct zwp_input_method_v2 *im_,
		const char *text, uint32_t cursor, uint32_t anchor) {
	(void)data; (void)im_;(void)cursor; (void)anchor;
	printf("  [im] surrounding text: '%s'\n", text ? text : "");
}
static void handle_im_content_type(void *data, struct zwp_input_method_v2 *im_,
		uint32_t hint, uint32_t purpose) {
	(void)data; (void)im_; (void)hint; (void)purpose;
}
static void handle_im_done(void *data, struct zwp_input_method_v2 *im_) {
	(void)data; (void)im_;
	im_done_count++;
}
static void handle_im_unavailable(void *data, struct zwp_input_method_v2 *im_) {
	(void)data; (void)im_;
	printf("  [im] unavailable\n");
}
static void handle_im_text_change_cause(void *data,
		struct zwp_input_method_v2 *im_, uint32_t cause) {
	(void)data; (void)im_; (void)cause;
}
static void handle_grab_keymap(void *data,
		struct zwp_input_method_keyboard_grab_v2 *grab, uint32_t format,
		int32_t fd, uint32_t size) {
	(void)data; (void)grab; (void)format; (void)fd; (void)size;
	im_got_keymap = true;
}
static void handle_grab_key(void *data,
		struct zwp_input_method_keyboard_grab_v2 *grab, uint32_t serial,
		uint32_t time, uint32_t key, uint32_t state) {
	(void)data; (void)grab; (void)serial; (void)time; (void)state;
	printf("  [im] grabbed key event: keycode=%u\n", key);
	im_got_key = key;
}
static void handle_grab_modifiers(void *data,
		struct zwp_input_method_keyboard_grab_v2 *grab, uint32_t serial,
		uint32_t depressed, uint32_t latched, uint32_t locked,
		uint32_t group) {
	(void)data; (void)grab; (void)serial; (void)depressed; (void)latched;
	(void)locked; (void)group;
}
static void handle_grab_repeat(void *data,
		struct zwp_input_method_keyboard_grab_v2 *grab, int32_t rate,
		int32_t delay) {
	(void)data; (void)grab; (void)rate; (void)delay;
}
static const struct zwp_input_method_keyboard_grab_v2_listener grab_listener = {
	.keymap = handle_grab_keymap,
	.key = handle_grab_key,
	.modifiers = handle_grab_modifiers,
	.repeat_info = handle_grab_repeat,
};

static const struct zwp_input_method_v2_listener im_listener = {
	.activate = handle_im_activate,
	.deactivate = handle_im_deactivate,
	.surrounding_text = handle_im_surrounding,
	.content_type = handle_im_content_type,
	.text_change_cause = handle_im_text_change_cause,
	.done = handle_im_done,
	.unavailable = handle_im_unavailable,
};

static void handle_im_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	(void)data; (void)version;
	if (strcmp(interface, wl_seat_interface.name) == 0) {
		im_seat = wl_registry_bind(registry, name, &wl_seat_interface, 4);
	} else if (strcmp(interface, zwp_input_method_manager_v2_interface.name) == 0) {
		im_mgr = wl_registry_bind(registry, name,
			&zwp_input_method_manager_v2_interface, 1);
	}
}
static void handle_im_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	(void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener im_registry_listener = {
	.global = handle_im_global,
	.global_remove = handle_im_global_remove,
};

static int roundtrip(struct wl_display *d) {
	wl_display_flush(d);
	return wl_display_roundtrip(d);
}

/* pump both displays until pred() is true or a timeout expires */
static void pump(struct wl_display *a, struct wl_display *b,
		bool (*pred)(void)) {
	for (int i = 0; i < 200 && !pred(); i++) {
		wl_display_flush(a);
		wl_display_flush(b);
		wl_display_roundtrip(a);
		wl_display_roundtrip(b);
		usleep(5000);
	}
}

static bool app_ready(void) { return ti_entered && ti_preedit && ti_committed; }
static bool im_ready(void) { return im_activated; }
static bool im_key_received(void) { return im_got_key != 0; }

int main(void) {
	/* ---- IM connects first (like fcitx5) ---- */
	im_display = wl_display_connect(NULL);
	check(im_display != NULL, "IM: connect to compositor");
	if (!im_display) return 1;
	struct wl_registry *im_reg = wl_display_get_registry(im_display);
	wl_registry_add_listener(im_reg, &im_registry_listener, NULL);
	roundtrip(im_display);
	check(im_mgr != NULL, "IM: zwp_input_method_manager_v2 advertised");
	check(im_seat != NULL, "IM: seat advertised");
	if (im_mgr && im_seat) {
		im = zwp_input_method_manager_v2_get_input_method(im_mgr, im_seat);
		zwp_input_method_v2_add_listener(im, &im_listener, NULL);
		roundtrip(im_display);
		printf("  [im] created input method\n");
	}

	/* ---- app connects ---- */
	app_display = wl_display_connect(NULL);
	check(app_display != NULL, "app: connect to compositor");
	struct wl_registry *reg = wl_display_get_registry(app_display);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	roundtrip(app_display);
	check(compositor && wm_base && ti_mgr && app_seat,
		"app: globals (compositor/xdg_wm_base/text_input_mgr/seat)");

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, "ime-relay-test");
	wl_surface_commit(surface);
	roundtrip(app_display);

	/* now that the first configure arrived and was acked, attach a buffer
	 * so the toplevel actually maps and gets keyboard focus */
	commit_buffer();
	roundtrip(app_display);

	/* the app enables text input for the seat (like an entry field) */
	text_input = zwp_text_input_manager_v3_get_text_input(ti_mgr, app_seat);
	zwp_text_input_v3_add_listener(text_input, &ti_listener, NULL);
	zwp_text_input_v3_enable(text_input);
	zwp_text_input_v3_set_content_type(text_input,
		ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
	zwp_text_input_v3_commit(text_input);
	roundtrip(app_display);

	/* ---- wait until the compositor activates the IM ---- */
	pump(app_display, im_display, im_ready);
	check(im_activated, "IM: activated after app gained focus");
	if (!im_activated) {
		goto out;
	}

	/* IM grabs the keyboard (fcitx5 does this while composing) */
	im_grab = zwp_input_method_v2_grab_keyboard(im);
	zwp_input_method_keyboard_grab_v2_add_listener(im_grab, &grab_listener, NULL);
	roundtrip(im_display);

	/* the keyboard is created AFTER the IM grabbed (fcitx5 scenario:
	 * compositor must connect the grab to it) */
	struct zwp_virtual_keyboard_v1 *vk = make_vkbd();
	roundtrip(app_display);

	/* type 'n' on the virtual keyboard: the IM's grab must receive it */
	vkbd_key(vk, 49); /* KEY_N */
	pump(app_display, im_display, im_key_received);
	check(im_got_keymap, "IM: received keymap via the grab");
	check(im_got_key == 49, "IM: received key 49 ('n') via the keyboard grab");

	/* IM types pinyin, commits the candidate "你" (serial = #done events) */
	zwp_input_method_v2_set_preedit_string(im, "ni", 1, 1);
	zwp_input_method_v2_commit_string(im, "\xE4\xBD\xA0"); /* 你 */
	zwp_input_method_v2_commit(im, im_done_count);
	pump(app_display, im_display, app_ready);

	check(ti_entered, "app: text input received enter");
	check(ti_preedit, "app: text input received preedit 'ni'");
	check(ti_committed && strcmp(ti_commit, "\xE4\xBD\xA0") == 0,
		"app: text input received commit string '你'");

out:
	if (app_display) wl_display_disconnect(app_display);
	if (im_display) wl_display_disconnect(im_display);
	printf(failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", failures);
	return failures == 0 ? 0 : 1;
}
