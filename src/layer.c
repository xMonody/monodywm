// layer.c - wlr-layer-shell surface (背景、状态栏、覆盖层)
//
// layer surface 被堆叠到各自的场景树中, 位于窗口图层之间;
// 它们的独占区会缩小最大化窗口使用的作区.

#include "server.h"

#include <stdlib.h>

#include <wlr/types/wlr_output_layout.h>
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

// 状态栏实际绘制的矩形 (scene 树位置 + surface 尺寸). 如果它比 exclusive_zone
// 更高/更宽 (自绘了额外边框/阴影, 或独占区设小了), 就按实际矩形再收紧作区,
// 这样最大化窗口的边框不会被状态栏盖住.
static void layer_surface_visual_zone(struct layer_surface *ls,
		struct wlr_box *area) {
	struct wlr_layer_surface_v1_state *state = &ls->layer_surface->current;
	if (state->exclusive_zone <= 0) {
		return;
	}
	int w = ls->layer_surface->surface->current.width;
	int h = ls->layer_surface->surface->current.height;
	if (w <= 0 || h <= 0) {
		return;
	}
	int bx = ls->scene_layer->tree->node.x;
	int by = ls->scene_layer->tree->node.y;
	// 与 wlroots/本文件一致的独占边判定 (只看该边, 不牵扯其他边)
	switch (wlr_layer_surface_v1_get_exclusive_edge(ls->layer_surface)) {
	case WLR_EDGE_TOP: {
		int edge = by + h;
		if (edge > area->y) {
			area->height -= edge - area->y;
			area->y = edge;
		}
		break;
	}
	case WLR_EDGE_BOTTOM:
		if (by < area->y + area->height) {
			area->height = by - area->y;
		}
		break;
	case WLR_EDGE_LEFT: {
		int edge = bx + w;
		if (edge > area->x) {
			area->width -= edge - area->x;
			area->x = edge;
		}
		break;
	}
	case WLR_EDGE_RIGHT:
		if (bx < area->x + area->width) {
			area->width = bx - area->x;
		}
		break;
	default:
		return;
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
			// 再用实际绘制矩形收紧一次, 防止独占区小于视觉高度
			layer_surface_visual_zone(ls, area);
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

// 面板/状态栏模糊: 节点被 configure 定位到 box.x/box.y, surface 填满整棵树,
// 所以 blur 放在 (0,0) 并取 surface 尺寸即可与面板对齐. 每次提交都同步,
// 因为它取决于客户端 buffer 尺寸, 而尺寸变化不一定改变布局签名 (见下);
// set_size / set_enabled 本身幂等, 重复调用无开销.
static void layer_surface_sync_blur(struct layer_surface *ls) {
	if (ls->blur == NULL) {
		return;
	}
	int w = ls->layer_surface->surface->current.width;
	int h = ls->layer_surface->surface->current.height;
	if (CONFIG_BLUR && CONFIG_BLUR_LAYER && w > 0 && h > 0) {
		wlr_scene_node_set_enabled(&ls->blur->node, true);
		wlr_scene_blur_set_size(ls->blur, w, h);
	} else {
		wlr_scene_node_set_enabled(&ls->blur->node, false);
	}
}

// 跟踪 layer surface 的布局状态 (独占区几何 + 客户端期望尺寸) 自上次提交
// 以来是否变化. 它既决定要不要给客户端发 configure, 也决定要不要重排已有窗口:
//   - 首次提交总视为变化, 这样在窗口已存在后才出现的状态栏也能把窗口挤出其区域;
//   - 客户端只重绘 (状态栏每秒刷新) 时签名不变, 绝不能重复 configure -
//     否则客户端每收到一个 configure 就 resize + commit, 合成器又在 commit
//     里 configure, 形成 configure ↔ commit 风暴 (CPU 打满, 队列/缓冲暴涨).
static bool layer_surface_layout_changed(struct layer_surface *ls) {
	struct wlr_layer_surface_v1_state *st = &ls->layer_surface->current;
	bool mapped = ls->layer_surface->surface->mapped;
	bool changed = !ls->has_last_state || ls->last_anchor != st->anchor ||
		ls->last_zone != st->exclusive_zone ||
		ls->last_margin_top != st->margin.top ||
		ls->last_margin_bottom != st->margin.bottom ||
		ls->last_margin_left != st->margin.left ||
		ls->last_margin_right != st->margin.right ||
		ls->last_desired_width != st->desired_width ||
		ls->last_desired_height != st->desired_height ||
		ls->last_mapped != mapped;
	ls->has_last_state = true;
	ls->last_anchor = st->anchor;
	ls->last_zone = st->exclusive_zone;
	ls->last_margin_top = st->margin.top;
	ls->last_margin_bottom = st->margin.bottom;
	ls->last_margin_left = st->margin.left;
	ls->last_margin_right = st->margin.right;
	ls->last_desired_width = st->desired_width;
	ls->last_desired_height = st->desired_height;
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

	// 只在布局状态变化时 configure. 无条件 configure 会与客户端形成
	// configure ↔ commit 风暴 (见 layer_surface_layout_changed 注释).
	bool layout_changed = layer_surface_layout_changed(ls);
	if (layout_changed) {
		configure_layer_surface(ls);
	}
	// blur 尺寸跟随客户端 buffer, 必须每次提交同步, 不能只依赖上面的
	// configure (尺寸变化时布局签名可能不变)
	layer_surface_sync_blur(ls);

	// background/bottom 层的画面是背景预模糊缓存的输入: 内容变化时让它失效,
	// 下一帧重算. 只有这两层位于缓存节点之下 (top/overlay 在窗口之上).
	if (st->layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND ||
			st->layer == ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM) {
		output_blur_layer_mark_dirty(ls->server);
	}

	if (became_interactive) {
		layer_surface_keyboard_focus(ls->server, ls);
	} else if (ls->keyboard_focused && (!mapped || !interactive)) {
		layer_surface_keyboard_unfocus(ls->server, ls);
	}
	ls->last_keyboard_interactive = st->keyboard_interactive;

	// 状态栏出现/改变尺寸会缩小作区: 把已有窗口移出独占区, 而不是让它盖住窗口
	if (layout_changed) {
		arrange_for_layer_surface(ls);
	}
}

static void layer_surface_destroy(struct wl_listener *listener, void *data) {
	struct layer_surface *ls = wl_container_of(listener, ls, destroy);
	// 持有键盘的启动器覆盖层正在消失: 交还给先前聚焦的 toplevel
	if (ls->keyboard_focused) {
		layer_surface_keyboard_unfocus(ls->server, ls);
	}
	// 背景/底部层消失: 预模糊缓存的输入变了, 让它失效
	enum zwlr_layer_shell_v1_layer layer = ls->layer_surface->current.layer;
	if (layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND ||
			layer == ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM) {
		output_blur_layer_mark_dirty(ls->server);
	}
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->commit.link);
	wl_list_remove(&ls->new_popup.link);
	wl_list_remove(&ls->link);
	// 独占区消失: 让最大化窗口重新铺开
	arrange_for_layer_surface(ls);
	free(ls);
}

// ------------------------------------------------------------------
// layer popup (面板/状态栏菜单、工具提示)
// ------------------------------------------------------------------

// layer-shell 的 xdg popup: 场景树挂在所属 layer surface 的树下面,
// 所以跟着 layer surface 一起移动/销毁. wlroots 的 scene layer surface
// 不处理 popup, 必须由合成器建节点并在首次提交时做约束(configure),
// 否则 waybar 菜单这类 popup 永远不会映射.
struct layer_popup_unconstrain;

struct layer_popup {
	struct layer_surface *ls;
	struct wlr_scene_tree *tree;

	// 首次提交前的约束跟踪; popup 角色销毁/树销毁时也要一并清理,
	// 否则之后的一次 surface commit 会解引用已释放的 wlr_xdg_popup
	struct layer_popup_unconstrain *unconstrain;

	struct wl_listener tree_destroy; // 树销毁时释放 lp
	struct wl_listener new_popup;    // 嵌套 popup (子菜单)
	struct wl_listener destroy;      // popup 角色销毁: 摘掉上面两个监听器
};

// 首次提交时把 popup 限制进所属输出. 与 lp 分开分配:
// tree_destroy 会先释放 lp, 反向引用用于在释放时清空 lp->unconstrain.
struct layer_popup_unconstrain {
	struct layer_popup *lp;
	struct layer_surface *ls;
	struct wlr_xdg_popup *popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

// wayland 的 wl_list_remove 会把 link 置空; 对同一监听器重复移除会崩
static void listener_remove_if_attached(struct wl_listener *listener) {
	if (listener->link.prev != NULL) {
		wl_list_remove(&listener->link);
	}
}

static void layer_popup_unconstrain_free(struct layer_popup_unconstrain *lu) {
	listener_remove_if_attached(&lu->commit);
	listener_remove_if_attached(&lu->destroy);
	if (lu->lp != NULL) {
		lu->lp->unconstrain = NULL;
	}
	free(lu);
}

static void layer_popup_unconstrain_apply(
		struct layer_popup_unconstrain *lu) {
	struct server *server = lu->ls->server;
	struct wlr_output *output = lu->ls->layer_surface->output;
	if (output == NULL) {
		output = wlr_output_layout_get_center_output(server->output_layout);
	}
	if (output != NULL) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, output, &box);
		// 约束框是 wlr_xdg_popup_unconstrain_from_box 要求的"toplevel 本地"
		// 坐标 (layer popup 的基准是所属 layer surface 左上角), 而
		// wlr_output_layout_get_box 给的是布局全局坐标: 减去 layer surface
		// 的场景位置, 否则面板不在原点时菜单会被 positioner 推出屏幕
		box.x -= lu->ls->scene_layer->tree->node.x;
		box.y -= lu->ls->scene_layer->tree->node.y;
		wlr_xdg_popup_unconstrain_from_box(lu->popup, &box);
	}
	layer_popup_unconstrain_free(lu);
}

static void layer_popup_unconstrain_commit(struct wl_listener *listener,
		void *data) {
	struct layer_popup_unconstrain *lu =
		wl_container_of(listener, lu, commit);
	layer_popup_unconstrain_apply(lu);
}

static void layer_popup_unconstrain_destroy(struct wl_listener *listener,
		void *data) {
	struct layer_popup_unconstrain *lu =
		wl_container_of(listener, lu, destroy);
	layer_popup_unconstrain_free(lu);
}

static void layer_popup_tree_destroy(struct wl_listener *listener, void *data) {
	struct layer_popup *lp = wl_container_of(listener, lp, tree_destroy);
	// 父树 (layer surface) 先销毁而 popup base 还活着时, 约束监听仍挂在
	// 它的信号上: 先解除反向引用再释放 lp
	if (lp->unconstrain != NULL) {
		layer_popup_unconstrain_free(lp->unconstrain);
	}
	listener_remove_if_attached(&lp->tree_destroy);
	listener_remove_if_attached(&lp->new_popup);
	listener_remove_if_attached(&lp->destroy);
	free(lp);
}

static void layer_popup_destroy(struct wl_listener *listener, void *data) {
	struct layer_popup *lp = wl_container_of(listener, lp, destroy);
	listener_remove_if_attached(&lp->new_popup);
	listener_remove_if_attached(&lp->destroy);
	// popup 角色先销毁 (xdg_popup.destroy) 时 base/surface 可能还活着,
	// 之后的一次 commit 会解引用已释放的 wlr_xdg_popup
	if (lp->unconstrain != NULL) {
		layer_popup_unconstrain_free(lp->unconstrain);
	}
}

static void layer_popup_attach(struct layer_surface *ls,
		struct wlr_xdg_popup *popup, struct wlr_scene_tree *parent_tree);

static void layer_popup_new_popup(struct wl_listener *listener, void *data) {
	struct layer_popup *lp = wl_container_of(listener, lp, new_popup);
	// 嵌套 popup (子菜单) 挂在父 popup 自己的树下面
	layer_popup_attach(lp->ls, data, lp->tree);
}

static void layer_popup_attach(struct layer_surface *ls,
		struct wlr_xdg_popup *popup, struct wlr_scene_tree *parent_tree) {
	struct layer_popup *lp = calloc(1, sizeof(*lp));
	if (lp == NULL) {
		return;
	}
	lp->ls = ls;

	struct layer_popup_unconstrain *lu = calloc(1, sizeof(*lu));
	if (lu != NULL) {
		lu->lp = lp;
		lu->ls = ls;
		lu->popup = popup;
		lp->unconstrain = lu;
		lu->commit.notify = layer_popup_unconstrain_commit;
		wl_signal_add(&popup->base->surface->events.commit, &lu->commit);
		lu->destroy.notify = layer_popup_unconstrain_destroy;
		wl_signal_add(&popup->base->events.destroy, &lu->destroy);
	}

	// wlr_scene_xdg_surface_create 处理 popup surface 及其 subsurface,
	// 并在每次提交时把树定位到 popup->current.geometry;
	// 它注册的监听器会在 popup 的 xdg surface 销毁时销毁该树
	lp->tree = wlr_scene_xdg_surface_create(parent_tree, popup->base);
	if (lp->tree == NULL) {
		if (lp->unconstrain != NULL) {
			layer_popup_unconstrain_free(lp->unconstrain);
		}
		free(lp);
		return;
	}
	// 打标签: 命中测试据此知道光标在 popup 上, 合成器边框手势让路
	xdg_surface_tag(lp->tree, TAG_POPUP, popup);

	lp->tree_destroy.notify = layer_popup_tree_destroy;
	wl_signal_add(&lp->tree->node.events.destroy, &lp->tree_destroy);
	lp->new_popup.notify = layer_popup_new_popup;
	wl_signal_add(&popup->base->events.new_popup, &lp->new_popup);
	lp->destroy.notify = layer_popup_destroy;
	wl_signal_add(&popup->events.destroy, &lp->destroy);
}

static void layer_surface_new_popup(struct wl_listener *listener, void *data) {
	struct layer_surface *ls = wl_container_of(listener, ls, new_popup);
	layer_popup_attach(ls, data, ls->scene_layer->tree);
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

	// 只为状态栏/面板建背景模糊节点, 落到底部 (内容之下). 排掉:
	//   - overlay 层: 启动器/覆盖菜单 (rofi/wofi/fuzzel) 往往全屏, 给它们
	//     模糊会把整个桌面都糊掉 - 点击状态栏弹出的通常正是这类窗口;
	//   - background 层: 它是最底层, blur 节点下面只有场景 clear color,
	//     模糊结果恒为一块纯色, 永远不会有可见效果 - 纯属无用节点.
	// 不设透明掩码: 整块面板区域都模糊, 面板自身的半透明背景色决定最终观感.
	if (CONFIG_BLUR && CONFIG_BLUR_LAYER &&
			layer_surface->pending.layer != ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY &&
			layer_surface->pending.layer !=
				ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
		ls->blur = wlr_scene_blur_create(ls->scene_layer->tree, 0, 0);
		if (ls->blur != NULL) {
			wlr_scene_node_lower_to_bottom(&ls->blur->node);
			wlr_scene_node_set_enabled(&ls->blur->node, false);
		}
	}

	ls->destroy.notify = layer_surface_destroy;
	wl_signal_add(&layer_surface->events.destroy, &ls->destroy);
	ls->commit.notify = layer_surface_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &ls->commit);
	// 面板菜单/工具提示: 建场景节点并做首次约束, 否则 popup 不会映射
	ls->new_popup.notify = layer_surface_new_popup;
	wl_signal_add(&layer_surface->events.new_popup, &ls->new_popup);

	wl_list_insert(server->layer_surfaces.prev, &ls->link);

	// layer surface 只有在首次 (空) 提交后才初始化, 所以初始 configure
	// 从 commit 处理器里发送
}
