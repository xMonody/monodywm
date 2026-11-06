// popup.c - xdg_popup 首次提交前的输出约束
//
// toplevel 与 layer-shell 的 popup 都需要同一套逻辑: 在 popup 首次提交时
// 把它的 positioner 限制进所属输出的框内, 并借此发送首个 configure.
// 两处唯一不同的是"用哪个输出"和"坐标原点在哪", 由调用方通过回调提供.

#include "server.h"

#include <stdlib.h>

#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>

struct popup_unconstrain {
	struct server *server;
	struct wlr_xdg_popup *popup;
	// popup 的基准输出/坐标原点 (调用方知道自己的场景树)
	struct wlr_output *(*get_output)(void *data, struct server *server);
	void (*get_origin)(void *data, int *x, int *y);
	void *data;
	// 所有者里指向本对象的字段: 释放时清空, 避免悬空
	struct popup_unconstrain **owner;

	struct wl_listener commit;
	struct wl_listener destroy;
};

void popup_unconstrain_free(struct popup_unconstrain *pu) {
	if (pu == NULL) {
		return;
	}
	listener_remove_if_attached(&pu->commit);
	listener_remove_if_attached(&pu->destroy);
	if (pu->owner != NULL) {
		*pu->owner = NULL;
	}
	free(pu);
}

static void popup_unconstrain_apply(struct popup_unconstrain *pu) {
	struct server *server = pu->server;
	if (pu->popup == NULL || pu->popup->base == NULL) {
		popup_unconstrain_free(pu);
		return;
	}
	struct wlr_output *output = pu->get_output != NULL
		? pu->get_output(pu->data, server) : NULL;
	if (output == NULL) {
		output = wlr_output_layout_get_center_output(server->output_layout);
	}
	if (output != NULL) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, output, &box);
		// 约束框必须是 popup 基准 surface 的局部坐标, 而
		// wlr_output_layout_get_box() 给的是布局全局坐标: 减去 popup
		// 场景树的布局原点, 否则面板/窗口不在原点时菜单会被推出屏幕.
		int ox = 0, oy = 0;
		if (pu->get_origin != NULL) {
			pu->get_origin(pu->data, &ox, &oy);
		}
		box.x -= ox;
		box.y -= oy;
		wlr_xdg_popup_unconstrain_from_box(pu->popup, &box);
	}
	popup_unconstrain_free(pu);
}

static void popup_unconstrain_commit(struct wl_listener *listener, void *data) {
	struct popup_unconstrain *pu = wl_container_of(listener, pu, commit);
	popup_unconstrain_apply(pu);
}

static void popup_unconstrain_destroy(struct wl_listener *listener,
		void *data) {
	struct popup_unconstrain *pu = wl_container_of(listener, pu, destroy);
	popup_unconstrain_free(pu);
}

struct popup_unconstrain *popup_unconstrain_create(struct server *server,
		struct wlr_xdg_popup *popup,
		struct wlr_output *(*get_output)(void *data, struct server *server),
		void (*get_origin)(void *data, int *x, int *y),
		struct popup_unconstrain **owner, void *data) {
	struct popup_unconstrain *pu = calloc(1, sizeof(*pu));
	if (pu == NULL) {
		return NULL;
	}
	pu->server = server;
	pu->popup = popup;
	pu->get_output = get_output;
	pu->get_origin = get_origin;
	pu->owner = owner;
	pu->data = data;
	// 注册在 popup 首次提交时应用; destroy 保证角色先消失时不悬空.
	pu->commit.notify = popup_unconstrain_commit;
	wl_signal_add(&popup->base->surface->events.commit, &pu->commit);
	pu->destroy.notify = popup_unconstrain_destroy;
	wl_signal_add(&popup->base->events.destroy, &pu->destroy);
	return pu;
}
