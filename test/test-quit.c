/* quit test: drive a virtual keyboard to press Shift+Alt+q, which makes the
 * compositor terminate; verifies the shutdown path exits without assertion
 * (the compositor should print nothing scary and exit 0) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-client.h>
#include "virtual-keyboard-unstable-v1-client-protocol.h"

static struct wl_display *display;
static struct wl_seat *seat;
static struct zwp_virtual_keyboard_manager_v1 *vk_mgr;
static struct zwp_virtual_keyboard_v1 *vk;

static void handle_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 4);
	} else if (strcmp(interface,
			zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		vk_mgr = wl_registry_bind(registry, name,
			&zwp_virtual_keyboard_manager_v1_interface, 1);
	}
}
static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = NULL,
};

static void roundtrip(void) { wl_display_flush(display); wl_display_roundtrip(display); }

static void key(uint32_t keycode, uint32_t state) {
	zwp_virtual_keyboard_v1_key(vk, 1, keycode, state);
	wl_display_flush(display);
	roundtrip();
}

int main(void) {
	display = wl_display_connect(NULL);
	if (!display) { fprintf(stderr, "no display\n"); return 1; }
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	roundtrip();
	if (!seat || !vk_mgr) { fprintf(stderr, "missing globals\n"); return 1; }
	vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vk_mgr, seat);
	/* the virtual keyboard needs its own xkb keymap before any key */
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	char *kmap = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
	size_t klen = strlen(kmap);
	int kfd = syscall(SYS_memfd_create, "kmap", 0);
	ftruncate(kfd, (off_t)klen);
	void *p = mmap(NULL, klen, PROT_READ | PROT_WRITE, MAP_SHARED, kfd, 0);
	memcpy(p, kmap, klen);
	munmap(p, klen);
	zwp_virtual_keyboard_v1_keymap(vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, kfd, klen);
	close(kfd);
	xkb_keymap_unref(km);
	xkb_context_unref(ctx);
	free(kmap);
	roundtrip();

	/* Shift+Alt+q = quit */
	key(KEY_LEFTSHIFT, WL_KEYBOARD_KEY_STATE_PRESSED);
	key(KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_PRESSED);
	key(KEY_Q, WL_KEYBOARD_KEY_STATE_PRESSED);
	/* give the compositor a moment to terminate before we exit */
	wl_display_flush(display);
	sleep(1);
	return 0;
}
