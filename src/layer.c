// layer.c - wlr-layer-shell surface (背景、状态栏、覆盖层)
//
// layer surface 被堆叠到各自的场景树中, 位于窗口图层之间;
// 它们的独占区会缩小最大化窗口使用的作区.

#include "server.h"

#include <stdlib.h>

#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

static int scene_layer_index(enum zwlr_layer_shell_v1_layer layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return LAYER_BACKGROUND;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:      return LAYER_BOTTOM;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:         return LAYER_TOP;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:     return LAYER_OVERLAY;
	}
	return LAYER_OVERLAY;
}

static void layer_surface_exclusive_zone(struct wlr_layer_surface_v1_state *state,
		struct wlr_box *area) {
	uint32_t top = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
	uint32_t bottom = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	uint32_t left = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
	uint32_t right = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	uint32_t anchor = state->anchor;
	int32_t zone = state->exclusive_zone;
	if (zone <= 0) {
		return;
	}
	if (anchor == top || anchor == (top | left | right)) {
		area->y += zone + state->margin.top;
		area->height -= zone + state->margin.top;
	} else if (anchor == bottom || anchor == (bottom | left | right)) {
		area->height -= zone + state->margin.bottom;
	} else if (anchor == left || anchor == (top | bottom | left)) {
		area->x += zone + state->margin.left;
		area->width -= zone + state->margin.left;
	} else if (anchor == right || anchor == (top | bottom | right)) {
		area->width -= zone + state->margin.right;
	}
	if (area->width < 0) {
		area->width = 0;
	}
	if (area->height < 0) {
		area->height = 0;
	}
}

void get_work_area(struct server *server, struct wlr_output *output,
		struct wlr_box *area) {
	wlr_output_layout_get_box(server->output_layout, output, area);
	struct layer_surface *ls;
	wl_list_for_each(ls, &server->layer_surfaces, link) {
		struct wlr_layer_surface_v1 *layer = ls->layer_surface;
		// 不指定输出的状态栏会横跨每个输出 (wlroots 在每个输出上都渲染它),
		// 所以它的独占区缩小的是每个输出的作区, 而不只是它指定的那个
		if (layer->output != NULL && layer->output != output) {
			continue;
		}
		if (layer->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
			continue; // 临时覆盖层不缩小作区
		}
		if (layer->surface->mapped) {
			layer_surface_exclusive_zone(&layer->current, area);
		}
	}
}

static void configure_layer_surface(struct layer_surface *ls) {
	struct wlr_layer_surface_v1 *layer_surface = ls->layer_surface;
	if (!layer_surface->initialized) {
		return;
	}
	struct wlr_output *output = layer_surface->output;
	if (output == NULL) {
		output = wlr_output_layout_get_center_output(ls->server->output_layout);
	}
	if (output == NULL) {
		return;
	}
	struct wlr_box full_area;
	wlr_output_layout_get_box(ls->server->output_layout, output, &full_area);
	struct wlr_box usable_area = full_area;
	wlr_scene_layer_surface_v1_configure(ls->scene_layer, &full_area,
		&usable_area);
}

// 跟踪 layer surface 的独占区几何自上次提交以来是否变化
// (首次提交总视为变化, 这样在窗口已存在后才出现的状态栏也能把窗口挤出其区域)
static bool layer_exclusive_zone_changed(struct layer_surface *ls) {
	struct wlr_layer_surface_v1_state *st = &ls->layer_surface->current;
	bool mapped = ls->layer_surface->surface->mapped;
	bool changed = !ls->has_last_state || ls->last_anchor != st->anchor ||
		ls->last_zone != st->exclusive_zone ||
		ls->last_margin_top != st->margin.top ||
		ls->last_margin_bottom != st->margin.bottom ||
		ls->last_margin_left != st->margin.left ||
		ls->last_margin_right != st->margin.right ||
		ls->last_mapped != mapped;
	ls->has_last_state = true;
	ls->last_anchor = st->anchor;
	ls->last_zone = st->exclusive_zone;
	ls->last_margin_top = st->margin.top;
	ls->last_margin_bottom = st->margin.bottom;
	ls->last_margin_left = st->margin.left;
	ls->last_margin_right = st->margin.right;
	ls->last_mapped = mapped;
	return changed;
}

// layer surface 的独占区变化 (或消失) 了: 把已有窗口移出它的区域.
// 不指定输出的状态栏横跨每个输出, 所以所有输出都会重排.
static void arrange_for_layer_surface(struct layer_surface *ls) {
	// 只有带独占区的 surface 才改变作区. 菜单/启动器覆盖层 (以及任何
	// 没有独占区的 surface) 不得触发重排: 它们的映射/解除映射会按过期的
	// current.maximized 状态重新适配最大化窗口, 把刚还原 (还原 configure 尚未 ack)
	// 的窗口又弹回最大化框 - 这就是任务栏菜单 (以 layer-shell overlay 实现) 造成的
	// 典型"闪一下又保持最大化".
	if (ls->layer_surface->current.exclusive_zone <= 0) {
		return;
	}
	struct wlr_output *output = ls->layer_surface->output;
	if (output != NULL) {
		arrange_toplevels_work_area(ls->server, output);
		return;
	}
	struct wlr_output_layout_output *lo;
	wl_list_for_each(lo, &ls->server->output_layout->outputs, link) {
		arrange_toplevels_work_area(ls->server, lo->output);
	}
}

// 把 seat 键盘焦点交给一个已映射且要求键盘交互的 layer surface
// (rofi/wofi 启动器等). server->focused 特意保持不变: toplevel 仍是
// "聚焦窗口" (边框保持聚焦色), 覆盖层消失后重新拿回键盘.
static void layer_surface_keyboard_focus(struct server *server,
		struct layer_surface *ls) {
	struct wlr_layer_surface_v1 *layer_surface = ls->layer_surface;
	if (server->layer_focused == ls) {
		return;
	}
	ls->keyboard_focused = true;
	server->layer_focused = ls;
	wlr_log(WLR_DEBUG, "layer: keyboard focus -> %s surface %p",
		layer_surface->namespace ? layer_surface->namespace : "(null)",
		(void *)layer_surface->surface);
	seat_keyboard_focus(server, layer_surface->surface);
	ime_set_focus(server, layer_surface->surface);
}

// 释放交互 layer surface 的键盘持有 (若有), 并在同一步把 seat 键盘交给
// `surface`. `surface` 是接管键盘的 toplevel (focus_toplevel) 或先前聚焦
// toplevel 的 surface (layer 解除映射/销毁/失去交互); NULL 表示清空键盘焦点.
void layer_keyboard_clear(struct server *server, struct wlr_surface *surface) {
	struct layer_surface *ls = server->layer_focused;
	if (ls == NULL) {
		return;
	}
	ls->keyboard_focused = false;
	server->layer_focused = NULL;
	wlr_log(WLR_DEBUG, "layer: keyboard focus released (surface %p)",
		(void *)ls->layer_surface->surface);
	seat_keyboard_focus(server, surface);
	ime_set_focus(server, surface);
}

// layer surface 不再持有键盘 (解除映射/销毁/交互关闭):
// 交还给先前聚焦的 toplevel, 没有 toplevel 时清空
static void layer_surface_keyboard_unfocus(struct server *server,
		struct layer_surface *ls) {
	if (server->layer_focused != ls) {
		return;
	}
	struct wlr_surface *surface = NULL;
	struct toplevel *tl = server->focused;
	if (tl != NULL && tl->xdg_toplevel->base != NULL &&
			tl->xdg_toplevel->base->surface->mapped) {
		surface = tl->xdg_toplevel->base->surface;
	}
	layer_keyboard_clear(server, surface);
}

static void layer_surface_commit(struct wl_listener *listener, void *data) {
	struct layer_surface *ls = wl_container_of(listener, ls, commit);
	struct wlr_layer_surface_v1 *layer_surface = ls->layer_surface;
	struct wlr_layer_surface_v1_state *st = &layer_surface->current;

	bool mapped = layer_surface->surface->mapped;
	bool interactive = st->keyboard_interactive !=
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
	// 只在映射/交互状态变化时抓取焦点, 这样持续提交的 layer surface
	// (每秒重绘的状态栏) 永远不会抢走窗口的键盘
	bool became_interactive = mapped && interactive &&
		(!ls->has_last_state || !ls->last_mapped ||
		 ls->last_keyboard_interactive ==
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

	configure_layer_surface(ls);

	if (became_interactive) {
		layer_surface_keyboard_focus(ls->server, ls);
	} else if (ls->keyboard_focused && (!mapped || !interactive)) {
		layer_surface_keyboard_unfocus(ls->server, ls);
	}
	ls->last_keyboard_interactive = st->keyboard_interactive;

	// 状态栏出现/改变尺寸会缩小作区: 把已有窗口移出独占区, 而不是让它盖住窗口
	if (layer_exclusive_zone_changed(ls)) {
		arrange_for_layer_surface(ls);
	}
}

static void layer_surface_destroy(struct wl_listener *listener, void *data) {
	struct layer_surface *ls = wl_container_of(listener, ls, destroy);
	// 持有键盘的启动器覆盖层正在消失: 交还给先前聚焦的 toplevel
	if (ls->keyboard_focused) {
		layer_surface_keyboard_unfocus(ls->server, ls);
	}
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->commit.link);
	wl_list_remove(&ls->link);
	// 独占区消失: 让最大化窗口重新铺开
	arrange_for_layer_surface(ls);
	free(ls);
}

void server_new_layer_surface(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		new_layer_surface);
	struct wlr_layer_surface_v1 *layer_surface = data;

	struct layer_surface *ls = calloc(1, sizeof(*ls));
	if (ls == NULL) {
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}
	ls->server = server;
	ls->layer_surface = layer_surface;
	layer_surface->data = ls;

	ls->scene_layer = wlr_scene_layer_surface_v1_create(
		server->layers[scene_layer_index(layer_surface->pending.layer)],
		layer_surface);
	if (ls->scene_layer == NULL) {
		free(ls);
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}
	xdg_surface_tag(ls->scene_layer->tree, TAG_LAYER, ls);

	ls->destroy.notify = layer_surface_destroy;
	wl_signal_add(&layer_surface->events.destroy, &ls->destroy);
	ls->commit.notify = layer_surface_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &ls->commit);

	wl_list_insert(server->layer_surfaces.prev, &ls->link);

	// layer surface 只有在首次 (空) 提交后才初始化, 所以初始 configure
	// 从 commit 处理器里发送
}
