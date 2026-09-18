// pointer.c - 合成器光标交互
//
// 无装饰窗口的合成器边框: 顶部条 (拖动/长按移动, 双击左/中/右 = 最小化/
// 最大化切换/关闭, 右键+滚轮 = 最大化-还原/最小化-还原), 边缘/角缩放.
// 边框内的按压被合成器吞掉, 不到达客户端; 客户端自绘装饰保留原生控件.

#include "server.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

#include <linux/input-event-codes.h>

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>

// 光标是否位于无装饰窗口可见的标题条上? 该条横跨窗口宽度:
// 内容顶部 CONFIG_TITLEBAR_HEIGHT 像素, 分成三个手势段 (最小化/最大化/关闭).
static bool is_in_titlebar_zone(struct server *server, struct toplevel *tl) {
	// 浮在标题条上的 popup (菜单/下拉框) 赢得指针:
	// 光标在其上时不进行移动/最小化/最大化/关闭抓取
	if (pointer_over_popup(server) || pointer_over_layer_surface(server)) {
		return false;
	}
	if (tl->fullscreen) {
		// 全屏: 窗口覆盖整个输出, 合成器在其上不拥有边框 -
		// 标题条手势都不适用, 客户端必须拿回它的点击/光标
		return false;
	}
	if (tl->decoration_mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE &&
			!toplevel_is_dialog(tl) && !toplevel_is_fixed_size(tl)) {
		return false; // 客户端自绘标题栏, 原生使用它
	}
	// 对话框和固定尺寸窗口: 即使客户端自绘 (CSD) 装饰,
	// 顶部条带也是合成器的关闭按钮
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	if (base == NULL || !base->surface->mapped) {
		return false;
	}
	struct wlr_box box;
	toplevel_frame_box(server, tl, &box);
	if (box.width <= 0) {
		return false;
	}
	double lx = server->cursor->x;
	double ly = server->cursor->y;
	return lx >= box.x && lx < box.x + box.width &&
		ly >= box.y &&
		ly < box.y + CONFIG_TITLEBAR_HEIGHT;
}

// 按压落在标题条哪三分之一: 左三分之一 = 最小化,
// 中间三分之一 = 切换最大化/还原, 右三分之一 = 关闭
static enum zone_action title_strip_action(struct server *server,
		struct toplevel *tl) {
	// 不能最大化/最小化的窗口 (对话框, 或如 QQ 登录的固定尺寸) 顶部边框
	// 就是单个关闭按钮: 双击其任意位置都关闭窗口
	if (toplevel_is_dialog(tl) || toplevel_is_fixed_size(tl)) {
		return ZONE_CLOSE;
	}
	struct wlr_box box;
	toplevel_frame_box(server, tl, &box);
	if (box.width <= 0) {
		return ZONE_CLOSE;
	}
	double x = server->press_x;
	double third = box.width / 3.0;
	if (x < box.x + third) {
		return ZONE_MINIMIZE;
	}
	if (x < box.x + 2.0 * third) {
		return ZONE_MAXIMIZE;
	}
	return ZONE_CLOSE;
}

static void disarm_zone_timer(struct server *server) {
	if (server->zone_timer != NULL) {
		wl_event_source_timer_update(server->zone_timer, 0);
	}
}

// ------------------------------------------------------------------
// 窗口移动
// ------------------------------------------------------------------

static bool is_double_click(struct server *server, uint32_t button) {
	if (!server->last_was_click || button != server->last_click_button) {
		return false;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t dt = (now.tv_sec - server->last_release_time.tv_sec) * 1000000000L
		+ (now.tv_nsec - server->last_release_time.tv_nsec);
	return dt >= 0 && dt <= CONFIG_DOUBLE_CLICK_NS;
}

void begin_move(struct server *server, struct toplevel *tl,
		double ref_x, double ref_y) {
	if (server->moving) {
		return;
	}
	// 全屏窗口覆盖整个输出, 无处可去:
	// 拖动它 (只会把它从全屏剥下来并露出桌面) 永不生效, 用户必须先离开全屏.
	// 在这里设门控可覆盖所有移动入口 - 左右和弦、标题条拖动和 xdg_toplevel.move -
	// 所以合成器绝不可能移动全屏窗口.
	if (tl != NULL && tl->fullscreen) {
		return;
	}
	server->moving = true;
	server->input_mode = INPUT_MODE_MOVE;
	server->move_toplevel = tl;
	if (tl != NULL) {
		tl->user_moved = true; // 用户移动: 停止自动居中
	}
	server->move_ref_x = ref_x;
	server->move_ref_y = ref_y;
	server->grab_x = ref_x - tl->scene_tree->node.x;
	server->grab_y = ref_y - tl->scene_tree->node.y;
	// 记住最大化框: 拖动还原窗口时, 按压点在窗口内部的偏移会按比例映射进还原框
	// (见 move_toplevel_to)
	server->move_max_w = 0;
	server->move_max_h = 0;
	if (tl->xdg_toplevel->current.maximized) {
		struct wlr_box box;
		toplevel_frame_box(server, tl, &box);
		server->move_max_w = box.width;
		server->move_max_h = box.height;
	}
}

void end_move(struct server *server) {
	if (server->moving) {
	}
	server->moving = false;
	server->input_mode = INPUT_MODE_PASSTHROUGH;
	server->move_deferred_restore = false;
	server->move_toplevel = NULL;
	server->zone_toplevel = NULL;
	server->zone_press = false;
	server->dragged = false;
	server->zone_action = ZONE_NONE;
	disarm_zone_timer(server);
}

// 钳制被拖动窗口的位置, 使其顶部绝不会滑到顶部 layer-shell 状态栏之上;
// 左右状态栏也会钳制对应侧. 底部故意不钳制: 光标本身保持在状态栏之上
// (clamp_move_cursor), 窗口跟随它, 所以窗口可以滑过底部状态栏并移出屏幕底边 -
// Windows 风格.
static void clamp_drag_position(struct server *server, struct toplevel *tl,
		double *x, double *y) {
	struct wlr_box box;
	toplevel_frame_box(server, tl, &box);
	if (box.width <= 0 || box.height <= 0) {
		return;
	}
	struct wlr_output *output = toplevel_output(server, tl);
	if (output == NULL) {
		return;
	}
	struct wlr_box out;
	wlr_output_layout_get_box(server->output_layout, output, &out);
	struct wlr_box area;
	get_work_area(server, output, &area);

	if (area.x > out.x) {           // 左侧有栏
		if (*x < area.x) {
			*x = area.x;
		}
	}
	if (area.y > out.y) {           // 顶部有栏
		if (*y < area.y) {
			*y = area.y;
		}
	}
	if (area.x + area.width < out.x + out.width) { // 右侧有栏
		if (*x + box.width > area.x + area.width) {
			*x = area.x + area.width - box.width;
		}
	}
}

// 拖动窗口期间, 光标绝不能进入 layer-shell 状态栏的独占区:
// 钳制到其下输出的作区, 让它保持在状态栏之上可见.
// 窗口跟随光标且没有底部限制, 所以它可以滑过状态栏并移出屏幕.
static void clamp_move_cursor(struct server *server) {
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (output == NULL) {
		output = wlr_output_layout_get_center_output(server->output_layout);
	}
	if (output == NULL) {
		return;
	}
	struct wlr_box area;
	get_work_area(server, output, &area);
	double cx = server->cursor->x;
	double cy = server->cursor->y;
	if (cx < area.x) {
		cx = area.x;
	} else if (cx > area.x + area.width) {
		cx = area.x + area.width;
	}
	if (cy < area.y) {
		cy = area.y;
	} else if (cy > area.y + area.height) {
		cy = area.y + area.height;
	}
	if (cx != server->cursor->x || cy != server->cursor->y) {
		wlr_cursor_warp(server->cursor, NULL, cx, cy);
	}
}

static void move_toplevel_to(struct server *server, double lx, double ly) {
	struct toplevel *tl = server->move_toplevel;
	if (tl == NULL) {
		return;
	}
	// 客户端 (如 QQ 标题栏单击) 也会发 xdg_toplevel.move: 只有光标越过拖动
	// 阈值才还原最大化窗口, 免得单击/双击被误判成拖动
	if (tl->xdg_toplevel->current.maximized) {
		double ddx = server->cursor->x - server->move_ref_x;
		double ddy = server->cursor->y - server->move_ref_y;
		if (ddx * ddx + ddy * ddy <
				CONFIG_DRAG_THRESHOLD * CONFIG_DRAG_THRESHOLD) {
			// 一次从未越过拖动阈值的点击 (或双击的第一次按下):
			// 窗口保持最大化, 什么都不动
			return;
		}
		restore_maximized_toplevel(tl); // 拖动: 抓取接管几何
		// 按比例把抓取点从最大化框映射进还原框, 让光标抓住同一个相对位置
		// (绝不相对于还原后的原点重锚: 那会让抓取变负并把窗口推开)
		if (server->move_deferred_restore && tl->has_restore_box &&
				tl->restore_box.width > 0 && server->move_max_w > 0) {
			server->grab_x = server->grab_x * tl->restore_box.width /
				server->move_max_w;
			server->grab_y = server->grab_y * tl->restore_box.height /
				server->move_max_h;
			// 只映射一次: 在客户端提交取消最大化 configure 之前
			// current.maximized 一直为 true, 否则每次移动都会重入此分支
			// 并让抓取指数收缩, 每个事件都把光标往左上漂
			server->move_max_w = 0;
			server->move_max_h = 0;
		}
	}
	// 拖动期间光标必须保持在状态栏之上; 窗口跟随它且没有底部限制
	// (可以滑过状态栏并移出屏幕, Windows 风格)
	clamp_move_cursor(server);
	lx = server->cursor->x;
	ly = server->cursor->y;
	double nx = lx - server->grab_x;
	double ny = ly - server->grab_y;
	clamp_drag_position(server, tl, &nx, &ny);
	wlr_scene_node_set_position(&tl->scene_tree->node, nx, ny);
}

// 拖动的最大化窗口先还原到保存的浮动几何, 并把抓取参考点钳进还原后的窗口
// (抓取接管几何). 非最大化窗口原样返回.
static void restore_for_drag(struct toplevel *tl, double *ref_x, double *ref_y) {
	if (!tl->xdg_toplevel->current.maximized) {
		return;
	}
	restore_maximized_toplevel(tl); // 拖动: 抓取接管几何
	struct wlr_box rb = tl->restore_box;
	// restore_maximized_toplevel 会把还原位置钳进作区,
	// 所以按窗口实际框 (而不是过期保存的框) 抓住光标
	rb.x = tl->scene_tree->node.x;
	rb.y = tl->scene_tree->node.y;
	if (tl->has_restore_box && rb.width > 0) {
		if (*ref_x < rb.x) {
			*ref_x = rb.x;
		}
		if (*ref_x > rb.x + rb.width - 1) {
			*ref_x = rb.x + rb.width - 1;
		}
		if (*ref_y < rb.y) {
			*ref_y = rb.y;
		}
		if (*ref_y > rb.y + rb.height - 1) {
			*ref_y = rb.y + rb.height - 1;
		}
	}
}

// 把标题条上的按压变成移动抓取: 抓取锚定在原始按压位置;
// 拖动最大化窗口会先还原它 (Windows 行为), 使拖动抓住其还原后的几何
static void begin_zone_drag(struct server *server) {
	struct toplevel *tl = server->zone_toplevel;
	if (tl == NULL) {
		return;
	}
	server->dragged = true;
	server->zone_action = ZONE_NONE; // 拖动取消任何已臂置的双击
	double ref_x = server->press_x;
	double ref_y = server->press_y;
	restore_for_drag(tl, &ref_x, &ref_y);
	begin_move(server, tl, ref_x, ref_y);
	move_toplevel_to(server, server->cursor->x, server->cursor->y);
	server->last_was_click = false;
	disarm_zone_timer(server);
}

// 标题条被按住未移动达到 CONFIG_LONG_PRESS_NS: 抓取窗口使其跟随光标
// (在边框任意处长按都会移动窗口)
static int zone_timer_cb(void *data) {
	struct server *server = data;
	if (server->zone_press && server->zone_toplevel != NULL &&
			!server->dragged && !server->moving && !server->resizing &&
			!server->chord_active) {
		bool held = (server->zone_button == BTN_LEFT &&
			server->left_button_held) ||
			(server->zone_button == BTN_RIGHT &&
			server->right_button_held);
		if (held) {
			begin_zone_drag(server);
		}
	}
	return 0; // 保持源臂置, 供下一次按压使用
}

static void arm_zone_timer(struct server *server) {
	if (server->zone_timer == NULL) {
		struct wl_event_loop *loop =
			wl_display_get_event_loop(server->display);
		server->zone_timer =
			wl_event_loop_add_timer(loop, zone_timer_cb, server);
		if (server->zone_timer == NULL) {
			return; // 没有定时器: 拖动仍能移动, 长按无法抓取
		}
	}
	wl_event_source_timer_update(server->zone_timer,
		CONFIG_LONG_PRESS_NS / 1000000);
}

// ------------------------------------------------------------------
// 边缘缩放
// ------------------------------------------------------------------

// 光标位于 toplevel 的哪些边缘. 只有合成器拥有的窗口 (无客户端侧装饰) 在这里缩放;
// CSD 窗口自行处理边缘. 顶部条本身是标题栏, 但它的两个角区 (左上/右上)
// 是对角缩放柄. 每个抓取区跨骑边缘: CONFIG_EDGE_THICKNESS 的一半在窗口框外,
// 一半在内, 所以从桌面和窗口内部都能碰到手柄.
// 最大化或全屏窗口绝不在这里缩放.
static uint32_t toplevel_resize_edges(struct server *server,
		struct toplevel *tl) {
	// 覆盖窗口边框的 popup 赢得指针: 光标在其上时没有缩放手柄
	// (popup 是聚焦 surface, 其点击必须到达菜单, 而不是触发缩放抓取)
	if (pointer_over_popup(server) || pointer_over_layer_surface(server)) {
		return 0;
	}
	if (tl->minimized || tl->xdg_toplevel->base == NULL ||
			tl->decoration_mode ==
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE ||
			tl->xdg_toplevel->current.maximized || tl->fullscreen ||
			toplevel_is_dialog(tl) || toplevel_is_fixed_size(tl)) {
		// 对话框和固定尺寸窗口 (如 QQ 登录) 绝不缩放,
		// 所以它们的边缘也不会出现缩放光标
		return 0;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);
	if (box.width <= 0 || box.height <= 0) {
		return 0;
	}
	double lx = server->cursor->x;
	double ly = server->cursor->y;
	// 单轴固定尺寸 (min == max) 只禁用该轴的缩放手柄
	bool fixed_w = tl->xdg_toplevel->current.min_width > 0 &&
		tl->xdg_toplevel->current.min_width ==
			tl->xdg_toplevel->current.max_width;
	bool fixed_h = tl->xdg_toplevel->current.min_height > 0 &&
		tl->xdg_toplevel->current.min_height ==
			tl->xdg_toplevel->current.max_height;
	if (fixed_w && fixed_h) {
		return 0;
	}
	uint32_t edges = 0;
	double zone = CONFIG_EDGE_THICKNESS / 2.0; // 一半在外, 一半在内
	double x0 = box.x - zone;              // 左手柄外缘
	double x1 = box.x + box.width + zone;  // 右手柄外缘
	double y0 = box.y - zone;              // 上手柄外缘
	double y1 = box.y + box.height + zone; // 下手柄外缘
	bool in_x = lx >= x0 && lx <= x1;
	bool in_y = ly >= y0 && ly <= y1;
	bool on_left = lx >= x0 && lx < box.x + zone;
	bool on_right = lx > box.x + box.width - zone && lx <= x1;
	bool on_top = ly >= y0 && ly < box.y + zone;
	bool on_bottom = ly > box.y + box.height - zone && ly <= y1;
	// 左右手柄贯穿整个高度 (含两个角区)
	if (on_left && in_y && !fixed_w) {
		edges |= WLR_EDGE_LEFT;
	}
	if (on_right && in_y && !fixed_w) {
		edges |= WLR_EDGE_RIGHT;
	}
	// 下手柄贯穿整个宽度
	if (on_bottom && in_x && !fixed_h) {
		edges |= WLR_EDGE_BOTTOM;
	}
	// 顶部边缘本身是标题条; 只有它的两个角区是对角缩放手柄
	if (on_top && (on_left || on_right) && !fixed_h) {
		edges |= WLR_EDGE_TOP;
	}
	return edges;
}

static const char *resize_cursor_name(uint32_t edges) {
	bool left = (edges & WLR_EDGE_LEFT) != 0;
	bool right = (edges & WLR_EDGE_RIGHT) != 0;
	bool top = (edges & WLR_EDGE_TOP) != 0;
	bool bottom = (edges & WLR_EDGE_BOTTOM) != 0;
	if ((left && top) || (right && bottom)) {
		return "nwse-resize"; // 左上/右下角
	}
	if ((right && top) || (left && bottom)) {
		return "nesw-resize"; // 右上/左下角
	}
	if (left || right) {
		return "ew-resize";
	}
	return "ns-resize";
}

// ------------------------------------------------------------------
// bound buttons (类似 labwc): 合成器吞掉的按压
// ------------------------------------------------------------------

static bool bound_button_contains(struct bound_buttons *bb, uint32_t value) {
	for (int i = 0; i < bb->size; i++) {
		if (bb->values[i] == value) {
			return true;
		}
	}
	return false;
}

static void bound_button_add(struct bound_buttons *bb, uint32_t value) {
	if (bound_button_contains(bb, value)) {
		return;
	}
	if (bb->size >= BOUND_BUTTONS_MAX) {
		return;
	}
	bb->values[bb->size++] = value;
}

static void bound_button_remove(struct bound_buttons *bb, uint32_t value) {
	for (int i = 0; i < bb->size; i++) {
		if (bb->values[i] == value) {
			bb->values[i] = bb->values[--bb->size];
			return;
		}
	}
}

static void set_cursor_override(struct server *server, const char *name) {
	if (server->cursor_override == name) {
		return;
	}
	server->cursor_override = name;
	wlr_log(WLR_DEBUG, "cursor: %s (buttons=%zu)", name,
		server->seat->pointer_state.button_count);
	wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, name);
}

// 显示聚焦客户端当前想要的光标: 它由合成器渲染的光标形状 (cursor-shape-v1)、
// 它自己的 cursor surface (wl_pointer.set_cursor), 或默认箭头.
// 形状优先于 surface: 合成器总能按正确的输出缩放渲染它,
// 而客户端自绘 surface 可能由不使用 wp_fractional_scale_v1 的客户端决定尺寸.
// 若合成器光标覆盖 (标题条/resize 边缘) 处于活动状态, 它优先于一切.
void reapply_client_cursor(struct server *server) {
	if (server->cursor_override != NULL) {
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
			server->cursor_override);
		return;
	}
	struct wlr_surface *focused =
		server->seat->pointer_state.focused_surface;
	// 记录的形状只在指针仍位于该客户端 surface 上时有效;
	// 一旦指针移到空桌面或另一个 surface, 恢复它会显示过期光标
	// (如客户端在边缘设置的 resize 形状). 那种情况下落到下面的默认箭头.
	bool over_shape_client = focused != NULL &&
		server->client_cursor_shape_client != NULL &&
		focused->resource->client ==
			server->client_cursor_shape_client->client;
	if (server->client_cursor_shape != 0 &&
			server->client_cursor_shape_client ==
				server->seat->pointer_state.focused_client &&
			over_shape_client) {
		wlr_log(WLR_DEBUG, "cursor: restore-shape");
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
			wlr_cursor_shape_v1_name(server->client_cursor_shape));
		return;
	}
	// 只在指针仍位于该客户端 surface 上时才把指针交回客户端
	// (如从 resize 条回到窗口内). 若指针移到空桌面, 记录的客户端光标
	// (如终端的文本插入符) 会过期, 所以退回默认箭头.
	// 客户端把光标画在单独的 surface 上, 所以比较所属客户端而不是 surface 本身.
	bool over_client_surface = focused != NULL &&
		server->client_cursor_surface != NULL &&
		focused->resource->client ==
			server->client_cursor_surface->resource->client;
	wlr_log(WLR_DEBUG, "cursor: %s", over_client_surface ?
		"restore-client" : "left_ptr");
	if (over_client_surface) {
		wlr_cursor_set_surface(server->cursor, server->client_cursor_surface,
			server->client_cursor_hotspot_x,
			server->client_cursor_hotspot_y);
	} else {
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
			"left_ptr");
	}
}

static void clear_cursor_override(struct server *server) {
	if (server->cursor_override == NULL) {
		return;
	}
	server->cursor_override = NULL;
	reapply_client_cursor(server);
}

// 光标交互的 toplevel: 光标下的窗口, 或 - 光标在所有窗口之外时 -
// 其缩放抓取区 (CONFIG_EDGE_THICKNESS 像素跨骑窗口框: 一半内一半外)
// 包含光标的那个, 所以缩放手柄即使有一半在框外也仍可触达.
// 标题条在框上方 2 像素的悬出 (环的顶边) 同样可达, 所以整条彩色条都可拖动.
static struct toplevel *toplevel_nearby(struct server *server) {
	struct toplevel *tl = toplevel_at(server);
	if (tl != NULL && !tl->minimized && !tl->closing) {
		return tl;
	}
	struct toplevel *candidate;
	wl_list_for_each(candidate, &server->toplevels, link) {
		if (toplevel_resize_edges(server, candidate) != 0 ||
				(!candidate->minimized && !candidate->closing &&
				 is_in_titlebar_zone(server, candidate))) {
			return candidate;
		}
	}
	return NULL;
}

// 合成器抑制客户端光标请求的窗口周围条带.
// 实际抓取区是 CONFIG_EDGE_THICKNESS/2 (一半内一半外),
// 但客户端在窗口内部整个 CONFIG_EDGE_THICKNESS 范围内检测自己的边缘
// (winit、GTK 等). 没有更宽的条带, 客户端会把它的 resize 形状
// (如 clash-verge 的 ew_resize) 存在抓取区外一点, 光标离开边缘后又被过期地恢复.
static bool cursor_in_cursor_band(struct server *server,
		struct toplevel *tl) {
	if (tl->closing || tl->minimized || tl->xdg_toplevel->base == NULL ||
			tl->decoration_mode ==
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE ||
			tl->xdg_toplevel->current.maximized || tl->fullscreen ||
			toplevel_is_dialog(tl) || toplevel_is_fixed_size(tl)) {
		return false;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);
	if (box.width <= 0 || box.height <= 0) {
		return false;
	}
	double lx = server->cursor->x;
	double ly = server->cursor->y;
	double in = CONFIG_EDGE_THICKNESS;        // 窗口内侧
	double out = CONFIG_EDGE_THICKNESS / 2.0; // 窗口外侧
	bool x_band = lx >= box.x - out && lx <= box.x + box.width + out;
	bool y_band = ly >= box.y - out && ly <= box.y + box.height + out;
	bool on_left = lx >= box.x - out && lx < box.x + in;
	bool on_right = lx > box.x + box.width - in &&
		lx <= box.x + box.width + out;
	bool on_top = ly >= box.y - out && ly < box.y + in;
	bool on_bottom = ly > box.y + box.height - in &&
		ly <= box.y + box.height + out;
	return (on_left && y_band) || (on_right && y_band) ||
		(on_top && x_band) || (on_bottom && x_band);
}

// 光标是否位于任意窗口的合成器边框区 (标题条/resize 边缘)?
// 那里合成器拥有光标, 所以客户端光标请求被忽略 (input.c) -
// 客户端仍保留指针焦点并收到 motion, 所以其悬停反馈继续工作.
bool pointer_over_frame_zone(struct server *server) {
	if (pointer_over_popup(server) || pointer_over_layer_surface(server)) {
		return false;
	}
	struct toplevel *tl = toplevel_at(server);
	if (tl != NULL && !tl->minimized && !tl->closing) {
		return cursor_in_cursor_band(server, tl) ||
			is_in_titlebar_zone(server, tl);
	}
	// 光标在所有窗口之外: 边缘区的外半部分在桌面上仍可触达
	struct toplevel *candidate;
	wl_list_for_each(candidate, &server->toplevels, link) {
		if (cursor_in_cursor_band(server, candidate)) {
			return true;
		}
	}
	return false;
}

// 决定当前位置显示哪种合成器光标: 悬停彩色顶部条 (CONFIG_TITLEBAR_HEIGHT 像素)
// 显示 CONFIG_TITLEBAR_CURSOR (all-scroll); 移出条后通过 clear_cursor_override()
// 恢复客户端光标
void update_cursor_style(struct server *server) {
	// 被捕获的指针 (QEMU 抓取、游戏 mouselook) 属于客户端:
	// 绝不在其上显示合成器自己的边框光标
	if (pointer_constraint_active(server)) {
		clear_cursor_override(server);
		return;
	}
	const char *name = NULL;

	if (server->moving && server->move_toplevel != NULL) {
		name = CONFIG_MOVE_CURSOR;
	} else if (server->resizing && server->resize_toplevel != NULL) {
		name = resize_cursor_name(server->resize_edges);
	} else if (server->seat->pointer_state.button_count > 0) {
		// 隐式抓取: 客户端按住指针按钮 (文本选择、右键拖动等), 指针聚焦在其 surface 上.
		// 按住期间绝不切换到合成器的悬停光标 (边缘 resize/标题条), 无论哪条路径到达这里
		// (motion、按钮释放、焦点变化); 客户端光标保持到最后一个按钮释放.
		return;
	} else {
		struct toplevel *tl = toplevel_nearby(server);
		if (tl != NULL) {
			if (toplevel_is_dialog(tl)) {
				// 对话框: 顶部关闭条带得到 all-scroll 提示
				// (CONFIG_EDGE_THICKNESS / 标题条高度); 边缘保留客户端自己的光标 -
				// 没有缩放光标
				if (is_in_titlebar_zone(server, tl)) {
					name = CONFIG_TITLEBAR_CURSOR;
				}
			} else {
				uint32_t edges = toplevel_resize_edges(server, tl);
				// 角落缩放区优先于标题条; 其他地方顶部条保持其 all-scroll 光标
				bool corner = (edges & WLR_EDGE_TOP) != 0;
				if (corner || !is_in_titlebar_zone(server, tl)) {
					if (edges != 0) {
						name = resize_cursor_name(edges);
					}
				} else {
					name = CONFIG_TITLEBAR_CURSOR;
				}
			}
		}
	}

	if (name != NULL) {
		set_cursor_override(server, name);
	} else if (server->cursor_override != NULL) {
		// 让指针停在客户端自己的光标上 (或默认光标, motion 处理器在 surface
		// 变化时会重置它)
		clear_cursor_override(server);
	}
}

// ------------------------------------------------------------------
// 缩放轮廓 (labwc 风格)
// ------------------------------------------------------------------

static void resize_outline_ensure(struct server *server) {
	if (server->resize_outline != NULL) {
		return;
	}
	server->resize_outline =
		wlr_scene_tree_create(server->layers[LAYER_OVERLAY]);
	// 聚焦边框色 0x9F6680, 预乘
	float color[4] = { 0.62f, 0.40f, 0.50f, 1.0f };
	for (int i = 0; i < 4; i++) {
		server->resize_outline_edges[i] =
			wlr_scene_rect_create(server->resize_outline, 0, 0, color);
	}
}

static void resize_outline_show(struct server *server, struct wlr_box *box) {
	resize_outline_ensure(server);
	int t = 2;
	struct wlr_scene_rect **e = server->resize_outline_edges;
	wlr_scene_rect_set_size(e[0], box->width, t);
	wlr_scene_node_set_position(&e[0]->node, box->x, box->y);
	wlr_scene_rect_set_size(e[1], box->width, t);
	wlr_scene_node_set_position(&e[1]->node, box->x,
		box->y + box->height - t);
	wlr_scene_rect_set_size(e[2], t, box->height);
	wlr_scene_node_set_position(&e[2]->node, box->x, box->y);
	wlr_scene_rect_set_size(e[3], t, box->height);
	wlr_scene_node_set_position(&e[3]->node, box->x + box->width - t,
		box->y);
	wlr_scene_node_set_enabled(&server->resize_outline->node, true);
}

static void resize_outline_hide(struct server *server) {
	if (server->resize_outline != NULL) {
		wlr_scene_node_set_enabled(&server->resize_outline->node, false);
	}
}

// 轮廓模式: 按钮释放后发送最终尺寸, 并保持抓取直到客户端提交它
// (这样上/左重新定位和缩放光标保持不动). 从不提交的客户端 - 卡死,
// 或忽略 configure - 会让抓取和缩放光标永远卡住, 所以看门狗强制结束抓取.
static int resize_final_timeout_cb(void *data) {
	struct server *server = data;
	if (server->resizing && server->resize_final_pending) {
		resize_grab_clear(server);
	}
	return 0;
}

static void arm_resize_final_timer(struct server *server) {
	if (server->resize_final_timer == NULL) {
		struct wl_event_loop *loop =
			wl_display_get_event_loop(server->display);
		server->resize_final_timer =
			wl_event_loop_add_timer(loop, resize_final_timeout_cb, server);
		if (server->resize_final_timer == NULL) {
			return; // 没有看门狗: 抓取仍会在提交时结束
		}
	}
	wl_event_source_timer_update(server->resize_final_timer,
		CONFIG_RESIZE_FINAL_TIMEOUT_MS);
}

static void disarm_resize_final_timer(struct server *server) {
	if (server->resize_final_timer != NULL) {
		wl_event_source_timer_update(server->resize_final_timer, 0);
	}
}

void begin_resize(struct server *server, struct toplevel *tl,
		uint32_t edges) {
	// 绝不缩放最大化或全屏窗口
	if (server->resizing || tl->xdg_toplevel->current.maximized ||
			tl->fullscreen) {
		return;
	}
	server->resizing = true;
	server->input_mode = INPUT_MODE_RESIZE;
	server->resize_toplevel = tl;
	tl->user_moved = true; // 用户缩放: 停止自动居中
	server->resize_edges = edges;
	server->resize_final_pending = false;
	server->press_x = server->cursor->x;
	server->press_y = server->cursor->y;
	toplevel_box(tl, &server->resize_orig);
	// 尚未移动的抓取不得重新请求当前尺寸
	server->resize_last_w = server->resize_orig.width;
	server->resize_last_h = server->resize_orig.height;
	if (CONFIG_RESIZE_DRAW_CONTENTS) {
		wlr_xdg_toplevel_set_resizing(tl->xdg_toplevel, true);
	} else {
		// 轮廓模式: 显示起始框, 稍后再应用真实尺寸
		server->resize_target = server->resize_orig;
		resize_outline_show(server, &server->resize_target);
	}
	focus_toplevel(server, tl);
}

static void update_resize(struct server *server) {
	struct toplevel *tl = server->resize_toplevel;
	if (tl == NULL || tl->xdg_toplevel->base == NULL ||
			tl->xdg_toplevel->current.maximized) {
		return; // 绝不调整最大化窗口
	}
	double dx = server->cursor->x - server->press_x;
	double dy = server->cursor->y - server->press_y;
	struct wlr_box orig = server->resize_orig;

	// 目标尺寸用四舍五入而不是 (int) 截断: 截断会让拖动边缘落后光标最多 1 像素,
	// 并不对称地量化亚像素鼠标移动, 所以边缘在光标下抖动,
	// 而移动 (端到端用 double) 是平滑的.
	int nw = orig.width;
	int nh = orig.height;
	if ((server->resize_edges & WLR_EDGE_RIGHT) != 0) {
		nw = orig.width + (int)llround(dx);
	}
	if ((server->resize_edges & WLR_EDGE_LEFT) != 0) {
		nw = orig.width - (int)llround(dx);
	}
	if ((server->resize_edges & WLR_EDGE_BOTTOM) != 0) {
		nh = orig.height + (int)llround(dy);
	}
	if ((server->resize_edges & WLR_EDGE_TOP) != 0) {
		nh = orig.height - (int)llround(dy);
	}

	// 钳制到客户端约束 (或合理的回退值)
	int min_w = tl->xdg_toplevel->current.min_width > 0
		? tl->xdg_toplevel->current.min_width : 40;
	int min_h = tl->xdg_toplevel->current.min_height > 0
		? tl->xdg_toplevel->current.min_height : 30;
	int max_w = tl->xdg_toplevel->current.max_width > 0
		? tl->xdg_toplevel->current.max_width : INT_MAX;
	int max_h = tl->xdg_toplevel->current.max_height > 0
		? tl->xdg_toplevel->current.max_height : INT_MAX;
	if (nw < min_w) {
		nw = min_w;
	}
	if (nw > max_w) {
		nw = max_w;
	}
	if (nh < min_h) {
		nh = min_h;
	}
	if (nh > max_h) {
		nh = max_h;
	}

	// 目标框: 上/左抓取会移动窗口原点, 使对侧边缘保持在抓取开始时的锚点
	struct wlr_box box = {
		.x = (server->resize_edges & WLR_EDGE_LEFT) != 0
			? orig.x + orig.width - nw : orig.x,
		.y = (server->resize_edges & WLR_EDGE_TOP) != 0
			? orig.y + orig.height - nh : orig.y,
		.width = nw,
		.height = nh,
	};

	if (!CONFIG_RESIZE_DRAW_CONTENTS) {
		// 轮廓模式 (labwc 风格): 只用轮廓跟随光标;
		// 释放时一次性应用真实尺寸, 使拖动边缘无需等待客户端
		// configure -> commit 往返, 手感像窗口移动一样紧.
		if (box.x == server->resize_target.x &&
				box.y == server->resize_target.y &&
				box.width == server->resize_target.width &&
				box.height == server->resize_target.height) {
			return;
		}
		server->resize_target = box;
		resize_outline_show(server, &server->resize_target);
		return;
	}

	// 实时模式: 只有目标尺寸真正变化时才发 configure;
	// wlr_xdg_toplevel_set_size 总会调度一次, 所以在每个 motion 事件上重复同样尺寸
	// (亚像素增量, 或客户端尚未提交) 会让 configure -> commit -> mask/
	// border GPU 重绘循环在每个事件上穿过客户端. 那种同步的每次提交 GL 工作
	// 正是缩放拖动卡顿的原因 (软件光标也跟着卡), 而移动 - 无往返、无 GL - 保持平滑.
	if (nw == server->resize_last_w && nh == server->resize_last_h) {
		return;
	}
	server->resize_last_w = nw;
	server->resize_last_h = nh;
	wlr_xdg_toplevel_set_size(tl->xdg_toplevel, nw, nh);
}

void resize_grab_clear(struct server *server) {
	server->resizing = false;
	server->input_mode = INPUT_MODE_PASSTHROUGH;
	server->resize_toplevel = NULL;
	server->resize_edges = 0;
	server->resize_final_pending = false;
	// 释放到最终提交之间的迟到 motion 可能重新显示轮廓;
	// 在这里隐藏它, 使抓取结束后不残留幽灵轮廓
	resize_outline_hide(server);
	disarm_resize_final_timer(server);
}

void end_resize(struct server *server) {
	if (!server->resizing) {
		return;
	}
	struct toplevel *tl = server->resize_toplevel;

	if (tl != NULL && tl->xdg_toplevel->base != NULL) {
		if (CONFIG_RESIZE_DRAW_CONTENTS) {
			wlr_xdg_toplevel_set_resizing(tl->xdg_toplevel, false);
			resize_grab_clear(server);
			return;
		}
		// 轮廓模式: 隐藏轮廓并应用最终框. 保持抓取 (resizing 仍为 true)
		// 直到客户端提交新几何, 使光标保持缩放形状, 上/左重新定位发生在
		// xdg_toplevel_commit() 中并基于已提交尺寸 (没有一帧的弹跳, 也没有瞬时的默认光标).
		resize_outline_hide(server);
		struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
		if (!base->surface->mapped) {
			resize_grab_clear(server);
			return;
		}
		if (server->resize_target.width != base->geometry.width ||
				server->resize_target.height != base->geometry.height) {
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel,
				server->resize_target.width,
				server->resize_target.height);
			server->resize_final_pending = true; // 在提交时结束
			arm_resize_final_timer(server); // 看门狗
		} else {
			resize_grab_clear(server); // 尺寸未变: 不会有提交
		}
		return;
	}
	resize_grab_clear(server);
}

// ------------------------------------------------------------------
// 和弦手势
// ------------------------------------------------------------------

// 和弦: 按住一个按钮再按另一个
//   右键 + 双击左键 -> 最大化/还原; 左键 + 双击右键 -> 关闭;
//   按住另一个按钮   -> 移动光标下的窗口 (全屏窗口不移动)

// 用光标开始移动和弦的窗口; 抓取锚定在触发按钮的按压点
// (server->press_x/press_y), 所以整个拖动距离都得到尊重;
// 最大化窗口先还原, 使拖动抓住其还原后的几何 (Windows 行为, 与标题条拖动相同).
// 全屏窗口完全不移动 (提前返回, 所以和弦既不抓取也不显示移动光标).
static void begin_chord_move(struct server *server) {
	struct toplevel *tl = server->chord_toplevel;
	if (tl == NULL || server->moving || server->resizing || tl->fullscreen) {
		return;
	}
	double ref_x = server->press_x;
	double ref_y = server->press_y;
	restore_for_drag(tl, &ref_x, &ref_y);
	focus_toplevel(server, tl);
	begin_move(server, tl, ref_x, ref_y);
	move_toplevel_to(server, server->cursor->x, server->cursor->y);
	server->chord_moving = true;
	update_cursor_style(server); // CONFIG_MOVE_CURSOR
}

static void disarm_chord_timer(struct server *server) {
	if (server->chord_timer != NULL) {
		wl_event_source_timer_update(server->chord_timer, 0);
	}
}

// 判定定时器在触发按钮仍按住时到期: 这是按住, 不是双击的第一次点击 -> 移动窗口
static int chord_timer_cb(void *data) {
	struct server *server = data;
	if (server->chord_active && server->chord_pending &&
			!server->chord_moving && server->chord_toplevel != NULL &&
			!server->moving && !server->resizing) {
		bool held = (server->chord_button == BTN_LEFT &&
			server->left_button_held) ||
			(server->chord_button == BTN_RIGHT &&
			server->right_button_held);
		if (held) {
			server->chord_pending = false;
			begin_chord_move(server);
		}
	}
	return 0; // 保持源臂置, 供下一次和弦使用
}

static void arm_chord_timer(struct server *server) {
	if (server->chord_timer == NULL) {
		struct wl_event_loop *loop =
			wl_display_get_event_loop(server->display);
		server->chord_timer =
			wl_event_loop_add_timer(loop, chord_timer_cb, server);
		if (server->chord_timer == NULL) {
			return; // 没有定时器: 双击仍可用, 按住无法移动
		}
	}
	wl_event_source_timer_update(server->chord_timer,
		CONFIG_DOUBLE_CLICK_NS / 1000000);
}

// 完全结束一次和弦手势及其启动的所有抓取.
// 注意: 不清空 chord_toplevel - 双击触发路径需要在 end_chord() 之后
// 继续用 chord_double_click() 读取它. 窗口销毁路径由 toplevel_unfocus()
// 负责清空, 避免悬空指针.
void end_chord(struct server *server) {
	disarm_chord_timer(server);
	disarm_zone_timer(server);
	server->moving = false;
	server->input_mode = INPUT_MODE_PASSTHROUGH;
	server->move_deferred_restore = false;
	server->move_toplevel = NULL;
	server->zone_toplevel = NULL;
	server->zone_press = false;
	server->dragged = false;
	server->zone_action = ZONE_NONE;
	server->chord_active = false;
	server->chord_pending = false;
	server->chord_moving = false;
}

// 双击动作取决于按住的是哪个按钮:
// 按住右键 + 双击左键 -> 切换最大化/还原
// 按住左键 + 双击右键 -> 关闭窗口
static void chord_double_click(struct server *server, uint32_t chord_button) {
	struct toplevel *tl = server->chord_toplevel;
	if (tl == NULL) {
		return;
	}
	if (chord_button == BTN_LEFT) {
		// 按住的是右键
		focus_toplevel(server, tl);
		if (tl->xdg_toplevel->current.maximized) {
			restore_maximized_toplevel(tl);
		} else {
			set_maximized(server, tl, true);
		}
	} else {
		// 按住的是左键
		close_toplevel(tl);
	}
}

// 一个按钮按住时另一个被按下: 记住合成器消费了哪些按压
// (其释放也要吞掉), 并区分双击 (切换最大化) 和按住 (移动)
static void begin_chord(struct server *server, uint32_t button) {
	struct toplevel *tl = toplevel_at(server);
	server->chord_active = true;
	server->chord_button = button;
	server->chord_toplevel = tl;
	server->chord_pending = false;
	server->chord_moving = false;

	// 触发按钮的按压被合成器吞掉
	server->press_x = server->cursor->x;
	server->press_y = server->cursor->y;
	if (button == BTN_LEFT) {
		server->chord_swallow_left = true;
	} else {
		server->chord_swallow_right = true;
	}
	// 注意: 被按住按钮的释放不需要额外的吞掉标志 -
	// 若其按压作为标题区按压被吞, 它已在 bound_buttons 里,
	// 释放要么由 process_chord_button (zone_press) 吞掉,
	// 要么由通用释放路径 (was_bound) 吞掉.

	if (is_double_click(server, button)) {
		// 双击了另一个按钮: 动作取决于按住的是哪个
		// (按住右键 -> 切换最大化, 按住左键 -> 关闭)
		server->last_was_click = false; // 被双击消费
		server->chord_active = false;
		chord_double_click(server, button);
		return;
	}

	// 首次按下: 可能变成双击 (最大化) 或按住 (移动); 定时器负责判定
	server->chord_pending = true;
	arm_chord_timer(server);
}

// 和弦活动期间的按钮事件: 触发按钮决定双击还是按住; 释放任一个都结束手势
static void process_chord_button(struct server *server, uint32_t time_msec,
		uint32_t button, enum wl_pointer_button_state state) {
	if (button != BTN_LEFT && button != BTN_RIGHT) {
		return; // 其他按钮不参与和弦
	}

	if (button == server->chord_button) {
		if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
			// 这次按压被和弦消费: 其释放也要吞掉 (即使和弦就此结束)
			if (button == BTN_LEFT) {
				server->chord_swallow_left = true;
			} else {
				server->chord_swallow_right = true;
			}
			// 双击窗口内的第二次按下: 用户双击了另一个按钮 ->
			// 最大化 (按住右键) 或关闭 (按住左键)
			if (is_double_click(server, button)) {
				server->last_was_click = false; // 已消费
				end_chord(server);
				chord_double_click(server, button);
			}
			return;
		}
		// 触发按钮的释放
		if (server->chord_pending) {
			// 这是一次点击, 不是按住: 记住它, 使很快的第二次按下被识别为双击
			disarm_chord_timer(server);
			clock_gettime(CLOCK_MONOTONIC, &server->last_release_time);
			server->last_was_click = true;
			server->last_click_button = button;
			server->chord_pending = false;
		} else if (server->chord_moving) {
			// 释放结束移动并恢复光标样式
			end_chord(server);
			update_cursor_style(server);
		} else {
			end_chord(server);
		}
		if (button == BTN_LEFT) {
			server->chord_swallow_left = false;
		} else {
			server->chord_swallow_right = false;
		}
		return;
	}

	// 被按住的按钮被释放: 结束和弦; 只有其按压到达过客户端才转发其释放
	// (否则它是一次被吞的标题区按压, 释放也必须保持被吞)
	if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
		bool zone = server->zone_press;
		// 被吞的标题区按压记在 bound_buttons 里; 这里清除它,
		// 使过期条目绝不会吞掉之后的释放
		bound_button_remove(&server->bound_buttons, button);
		end_chord(server);
		if (!zone) {
			wlr_seat_pointer_notify_button(server->seat, time_msec,
				button, state);
			wlr_seat_pointer_notify_frame(server->seat);
		}
		if (button == BTN_LEFT) {
			server->chord_swallow_left = false;
		} else {
			server->chord_swallow_right = false;
		}
	}
}

// ------------------------------------------------------------------
// 光标事件处理
// ------------------------------------------------------------------

// 布局点相对于某 toplevel surface 的 surface 局部坐标,
// 即使点位于 surface 之外 (用于隐式抓取期间继续向被抓 surface 转发 motion).
// surface 不是 toplevel surface 时返回 false.
// toplevel surface 内容在布局坐标下的原点 (含 geometry 偏移),
// 使 surface 局部坐标能映射回布局 (surface 局部点 + 原点 = 布局坐标)
static bool toplevel_surface_origin(struct server *server,
		struct wlr_surface *surface, double *ox, double *oy) {
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
		if (base == NULL || base->surface != surface) {
			continue;
		}
		*ox = tl->scene_tree->node.x - base->geometry.x;
		*oy = tl->scene_tree->node.y - base->geometry.y;
		return true;
	}
	return false;
}

static void process_cursor_motion(struct server *server, uint32_t time_msec) {
	if (server->drag_tree != NULL) {
		wlr_scene_node_set_position(&server->drag_tree->node,
			server->cursor->x, server->cursor->y);
	}

	// 让输入法的候选窗贴着光标
	ime_update_popup(server);

	if (server->moving && server->move_toplevel != NULL) {
		move_toplevel_to(server, server->cursor->x, server->cursor->y);
		update_cursor_style(server);
		return;
	}

	if (server->resizing && server->resize_toplevel != NULL) {
		update_resize(server);
		update_cursor_style(server);
		return;
	}

	// 和弦的触发键按住且光标移动: 立即开始移动, 而不等待判定定时器
	// (没有移动的快速释放仍算点击, 所以双击仍可用)
	if (server->chord_active && server->chord_pending &&
			!server->chord_moving && !server->moving) {
		double dx = server->cursor->x - server->press_x;
		double dy = server->cursor->y - server->press_y;
		if (dx * dx + dy * dy > CONFIG_DRAG_THRESHOLD * CONFIG_DRAG_THRESHOLD) {
			server->chord_pending = false;
			begin_chord_move(server);
			return; // 这次 motion 启动了移动; 不转发它
		}
	}

	if (server->zone_press && server->zone_toplevel != NULL &&
			!server->chord_active) {
		if (!server->dragged) {
			double dx = server->cursor->x - server->press_x;
			double dy = server->cursor->y - server->press_y;
			if (dx * dx + dy * dy > CONFIG_DRAG_THRESHOLD * CONFIG_DRAG_THRESHOLD) {
				// 标题条上会移动的按压变成移动抓取
				begin_zone_drag(server);
			}
		} else if (server->moving) {
			move_toplevel_to(server, server->cursor->x, server->cursor->y);
		}
		return;
	}

	// 常规路径: 把指针 motion 转发给光标下的 surface
	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct wlr_scene_node *node = wlr_scene_node_at(
			&server->scene->tree.node, server->cursor->x, server->cursor->y,
			&sx, &sy);
	if (node != NULL && node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *buffer =
			wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *scene_surface =
			wlr_scene_surface_try_from_buffer(buffer);
		if (scene_surface != NULL) {
			surface = scene_surface->surface;
		} else {
			// 命中的 buffer 是合成器圆角掩码内容的重绘:
			// 通过其树上的场景标签解析 xdg surface
			struct wlr_scene_node *n = node;
			while (n != NULL) {
				if (n->data != NULL) {
					struct scene_tag *tag = n->data;
					if (tag->type == TAG_POPUP) {
						// 圆角 popup (Qt 菜单): 命中的 buffer 是它的
						// 掩码重绘; 解析出 popup surface
						struct wlr_xdg_popup *popup = tag->ptr;
						if (popup != NULL && popup->base != NULL) {
							surface = popup->base->surface;
						}
					} else if (tag->type == TAG_TOPLEVEL) {
						struct toplevel *tl = tag->ptr;
						if (tl->xdg_toplevel->base != NULL) {
							surface = tl->xdg_toplevel->base->surface;
						}
					}
					break; // 更近的带标签对象赢得命中测试
				}
				n = n->parent != NULL ? &n->parent->node : NULL;
			}
		}
	}

	// 隐式抓取: 客户端按住一个指针按钮 (如文本选择).
	// 保持指针聚焦在被抓 surface 上并继续向它转发 motion,
	// 不切换焦点也不改光标, 直到按钮释放.
	if (server->seat->pointer_state.button_count > 0) {
		struct wlr_surface *focused =
			server->seat->pointer_state.focused_surface;
		if (focused != NULL) {
			if (surface == focused) {
				wlr_seat_pointer_notify_motion(server->seat, time_msec,
					sx, sy);
			} else {
				double ox, oy;
				if (toplevel_surface_origin(server, focused, &ox, &oy)) {
					wlr_seat_pointer_notify_motion(server->seat, time_msec,
						server->cursor->x - ox, server->cursor->y - oy);
				}
			}
		}
		return;
	}

	if (server->seat->pointer_state.focused_surface != surface) {
		clear_cursor_override(server);
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
			"left_ptr");
	}
	update_cursor_style(server);
	if (surface != NULL) {
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(server->seat);
	}
}

// 重做命中测试并更新指针焦点/光标样式 (窗口被隐藏/销毁后调用,
// 否则光标要等到下一次 motion 才更新)
void refresh_pointer_focus(struct server *server) {
	if (pointer_constraint_active(server)) {
		// 指针被客户端捕获: 焦点属于它, 只刷新合成器光标样式
		update_cursor_style(server);
		return;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	uint32_t now_ms = (uint32_t)now.tv_sec * 1000u +
		(uint32_t)(now.tv_nsec / 1000000u);
	process_cursor_motion(server, now_ms);
}

static void process_cursor_button(struct server *server, uint32_t time_msec,
		uint32_t button, enum wl_pointer_button_state state) {
	// 客户端持有指针期间 (pointer-constraints 锁定/限定, 如 QEMU 的 Ctrl+Alt+G 抓取),
	// 每个按钮都属于它 - 否则这里的和弦或标题条抓取会在左右键同时按时拖动被捕获窗口.
	// 转发按钮并保持无合成器手势状态.
	if (pointer_constraint_active(server)) {
		// 客户端拥有指针. 只有其按压到达过客户端时才转发释放;
		// 约束激活前被合成器吞掉的按压 (正在进行的抓取) 保持其释放被吞,
		// 所以不会投递孤立的释放.
		if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
			bool swallow = bound_button_contains(&server->bound_buttons,
				button);
			bound_button_remove(&server->bound_buttons, button);
			if (button == BTN_LEFT) {
				server->left_button_held = false;
				swallow = swallow || server->chord_swallow_left;
				server->chord_swallow_left = false;
			}
			if (button == BTN_RIGHT) {
				server->right_button_held = false;
				swallow = swallow || server->chord_swallow_right;
				server->chord_swallow_right = false;
			}
			if (swallow) {
				return;
			}
		} else {
			// 这里也记录按下状态: 被捕获期间按下的键会转发给客户端,
			// 但捕获提前结束时仍需为和弦/滚轮手势保留正确的按钮状态
			if (button == BTN_LEFT) {
				server->left_button_held = true;
			} else if (button == BTN_RIGHT) {
				server->right_button_held = true;
			}
		}
		wlr_seat_pointer_notify_button(server->seat, time_msec, button,
			state);
		return;
	}

	// layer-shell 覆盖层 (如开始菜单) 可能在光标位于其上时被销毁;
	// wlroots 随后会清除指针焦点, 而它只由 motion 事件重建,
	// 所以覆盖层消失后的第一次点击会被 seat 丢掉.
	// 在第一次按压时重跑命中测试, 让光标下的 surface 收到点击.
	if (state == WL_POINTER_BUTTON_STATE_PRESSED
			&& server->seat->pointer_state.button_count == 0
			&& server->seat->pointer_state.focused_surface == NULL) {
		process_cursor_motion(server, time_msec);
	}

	// 记住每个按钮是否按住: 滚轮手势 (上滚 = 最大化, 下滚 = 最小化)
	// 依赖右键, 和弦手势依赖两者
	if (button == BTN_LEFT) {
		server->left_button_held =
			state == WL_POINTER_BUTTON_STATE_PRESSED;
	}
	if (button == BTN_RIGHT) {
		server->right_button_held =
			state == WL_POINTER_BUTTON_STATE_PRESSED;
		if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
			// 新的一次按压开始新的滚动突发
			server->wheel_burst_start.tv_sec = 0;
			server->wheel_burst_start.tv_nsec = 0;
			server->wheel_last_tick.tv_sec = 0;
			server->wheel_last_tick.tv_nsec = 0;
		}
	}

	if (server->chord_active) {
		process_chord_button(server, time_msec, button, state);
		return;
	}

	// 开始和弦: 一个按钮按住, 另一个被按下.
	// 由 CONFIG_WHEEL_DEBOUNCE_ENABLED 设门控: 手势总开关关闭时,
	// 和弦从不启动, 两次按压都作为普通点击落到客户端.
	if (CONFIG_WHEEL_DEBOUNCE_ENABLED &&
			state == WL_POINTER_BUTTON_STATE_PRESSED &&
			!server->resizing && !server->moving &&
			((button == BTN_LEFT && server->right_button_held) ||
			 (button == BTN_RIGHT && server->left_button_held))) {
		begin_chord(server, button);
		return;
	}

	if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
		// 单一决策点 (labwc 风格): 合成器吞掉的每次按压都记录在 bound_buttons,
		// 所以只有未被记录的释放才到达客户端.
		bool was_bound = bound_button_contains(&server->bound_buttons,
			button);
		bound_button_remove(&server->bound_buttons, button);

		// 和弦消费了该按钮的按压但提前结束 (如被按住的按钮先释放):
		// 吞掉释放, 使客户端绝不会看到没有匹配按压的孤立释放
		if ((button == BTN_LEFT && server->chord_swallow_left) ||
				(button == BTN_RIGHT && server->chord_swallow_right)) {
			if (button == BTN_LEFT) {
				server->chord_swallow_left = false;
			} else {
				server->chord_swallow_right = false;
			}
			return;
		}

		if (server->resizing) {
			end_resize(server);
			if (!was_bound) {
				// 客户端发起的缩放 (xdg_toplevel.resize): 其按压被转发过,
				// 所以释放也必须转发
				wlr_seat_pointer_notify_button(server->seat, time_msec,
					button, state);
				wlr_seat_pointer_notify_frame(server->seat);
			}
			update_cursor_style(server);
			return;
		}
		if (server->moving) {
			end_move(server);
			if (!was_bound) {
				// 客户端发起的移动 (xdg_toplevel.move): 其按压被转发过,
				// 所以释放也必须转发
				wlr_seat_pointer_notify_button(server->seat, time_msec,
					button, state);
				wlr_seat_pointer_notify_frame(server->seat);
			}
			update_cursor_style(server);
			return;
		}
		if (server->zone_press) {
			// 被吞的标题区按压的释放
			struct toplevel *tl = server->zone_toplevel;
			server->zone_press = false;
			server->zone_toplevel = NULL;
			disarm_zone_timer(server);
			if (!server->dragged) {
				if (server->zone_action != ZONE_NONE && tl != NULL) {
					// 标题条上双击的第二次点击: 光标下的段决定动作
					switch (server->zone_action) {
					case ZONE_MINIMIZE:
						set_minimized(server, tl, true);
						break;
					case ZONE_MAXIMIZE:
						focus_toplevel(server, tl);
						if (tl->xdg_toplevel->current.maximized) {
							restore_maximized_toplevel(tl);
						} else {
							set_maximized(server, tl, true);
						}
						break;
					case ZONE_CLOSE:
						close_toplevel(tl);
						break;
					default:
						break;
					}
				} else {
					clock_gettime(CLOCK_MONOTONIC,
						&server->last_release_time);
					server->last_was_click = true;
					server->last_click_button = button;
				}
			} else {
				server->last_was_click = false;
			}
			server->dragged = false;
			server->zone_action = ZONE_NONE;
			update_cursor_style(server);
			return;
		}
		// 常规路径: 只有其按压到达过客户端时才转发释放.
		// 抓取提前结束 (如窗口被销毁) 的被吞按压在这里吞掉其释放, 绝不孤立.
		if (!was_bound) {
			wlr_seat_pointer_notify_button(server->seat, time_msec,
				button, state);
			process_cursor_motion(server, time_msec);
		}
		return;
	}

	// WLR_BUTTON_PRESSED
	if (server->moving || server->zone_press || server->resizing) {
		bound_button_add(&server->bound_buttons, button);
		return; // 已经抓取: 吞掉, 释放也必须吞掉
	}

	// 光标位于 layer-shell surface (任务栏/菜单覆盖层) 上:
	// 点击属于它, 绝不要穿过它开始窗口移动/缩放抓取
	if (pointer_over_layer_surface(server)) {
		wlr_seat_pointer_notify_button(server->seat, time_msec, button, state);
		return;
	}

	// 隐式抓取活动: 之前的按压已转发给客户端且仍按住 (buttons > 0).
	// 这次按压也转发, 而不是劫持为缩放/移动抓取,
	// 让客户端看到完整的多按钮手势, 光标也留在隐式抓取规则之下.
	if (server->seat->pointer_state.button_count > 0) {
		struct toplevel *tl = toplevel_nearby(server);
		if (tl != NULL && !tl->minimized) {
			focus_toplevel(server, tl);
		}
		wlr_seat_pointer_notify_button(server->seat, time_msec, button,
			state);
		return;
	}

	struct toplevel *tl = toplevel_nearby(server);
	if (tl != NULL && !tl->minimized) {
		uint32_t edges = toplevel_resize_edges(server, tl);
		// 角落缩放区优先于标题条; 其他地方顶部条保持为标题栏
		if (edges != 0 &&
				((edges & WLR_EDGE_TOP) != 0 ||
				 !is_in_titlebar_zone(server, tl))) {
			bound_button_add(&server->bound_buttons, button);
			begin_resize(server, tl, edges);
			return;
		}
	}
	if (tl != NULL && !tl->minimized && is_in_titlebar_zone(server, tl)) {
		// 接管: 彩色顶部条是我们可见的标题栏
		focus_toplevel(server, tl);
		bound_button_add(&server->bound_buttons, button);
		server->zone_press = true;
		server->zone_toplevel = tl;
		server->zone_button = button;
		server->press_x = server->cursor->x;
		server->press_y = server->cursor->y;
		server->dragged = false;
		if (is_double_click(server, button)) {
			// 双击的第二次点击: 为光标下的段臂置动作 (释放时触发)
			server->last_was_click = false; // 被双击消费
			server->zone_action = title_strip_action(server, tl);
		} else {
			server->zone_action = ZONE_NONE;
			// 快速释放算点击 (两次点击 = 双击);
			// 静止按住超过 CONFIG_LONG_PRESS_NS 则抓取窗口移动 - 定时器负责判定
			arm_zone_timer(server);
		}
		return;
	}

	if (tl != NULL && !tl->minimized) {
		focus_toplevel(server, tl);
	}
	wlr_seat_pointer_notify_button(server->seat, time_msec, button, state);
}

// 右键+滚轮手势 (上滚切换最大化/还原, 下滚最小化): 一次滚轮突发只算一个
// 动作. 两个阈值 (见 CONFIG_WHEEL_*): 突发窗口最长时长, 以及两次 tick 的最小间隔.
static bool wheel_action_allowed(struct server *server) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t gap = (now.tv_sec - server->wheel_last_tick.tv_sec) * 1000000000L
		+ (now.tv_nsec - server->wheel_last_tick.tv_nsec);
	int64_t dur = (now.tv_sec - server->wheel_burst_start.tv_sec) * 1000000000L
		+ (now.tv_nsec - server->wheel_burst_start.tv_nsec);
	server->wheel_last_tick = now;
	if (gap >= 0 && gap < CONFIG_WHEEL_TICK_GAP_NS &&
			dur >= 0 && dur <= CONFIG_WHEEL_BURST_NS) {
		return false; // 同一手势: tick 来得快且突发仍在最大长度内
	}
	// 新手势: 要么上一个 tick 太久远 (gap), 要么突发超过了最大长度 (duration)
	server->wheel_burst_start = now;
	return true;
}

// 滚轮手势作用的 toplevel: 光标下的窗口, 或 - 光标位于最小化 (隐藏) 窗口的
// 位置时 - 那个窗口, 使下滚可以还原它
static struct toplevel *toplevel_at_or_minimized(struct server *server) {
	struct toplevel *tl = toplevel_at(server);
	if (tl != NULL && !tl->minimized && !tl->closing) {
		return tl;
	}
	struct toplevel *candidate;
	wl_list_for_each(candidate, &server->toplevels, link) {
		if (candidate->minimized && candidate->xdg_toplevel->base != NULL &&
				candidate->xdg_toplevel->base->surface->mapped) {
			struct wlr_box box;
			toplevel_box(candidate, &box);
			if (server->cursor->x >= box.x &&
					server->cursor->x < box.x + box.width &&
					server->cursor->y >= box.y &&
					server->cursor->y < box.y + box.height) {
				return candidate;
			}
		}
	}
	return NULL;
}

static void process_cursor_axis(struct server *server, uint32_t time_msec,
		struct wlr_pointer_axis_event *event) {
	struct toplevel *tl = toplevel_at_or_minimized(server);
	// 由手势总开关门控: CONFIG_WHEEL_DEBOUNCE_ENABLED 为 false 时
	// 滚轮不抓取滚动 - 它像普通滚动一样在下面转发给客户端
	if (CONFIG_WHEEL_DEBOUNCE_ENABLED && tl != NULL &&
			server->right_button_held &&
			!pointer_constraint_active(server) &&
			event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL &&
			event->delta_discrete != 0) {
		// 在窗口上按住右键时:
		// 上滚 (负轴值) 切换最大化/还原, 下滚 (正轴值) 切换最小化/还原.
		// 两个阈值合并快速 tick: 一次连续滚动最多 CONFIG_WHEEL_BURST_NS 算一个动作,
		// 两个 tick 至少相隔 CONFIG_WHEEL_TICK_GAP_NS 即下一个动作.
		if (!wheel_action_allowed(server)) {
			return; // 吞掉: 仍在同一手势内
		}
		if (event->delta_discrete < 0) {
			if (tl->xdg_toplevel->current.maximized) {
				// 已最大化: 还原保存的几何
				restore_maximized_toplevel(tl);
			} else {
				set_maximized(server, tl, true);
			}
		} else if (tl->minimized) {
			// 已最小化: 还原 (显示) 窗口
			set_minimized(server, tl, false);
		} else {
			set_minimized(server, tl, true);
		}
		return;
	}
	wlr_seat_pointer_notify_axis(server->seat, time_msec, event->orientation,
		event->delta, event->delta_discrete, event->source,
		event->relative_direction);
}

// ------------------------------------------------------------------
// pointer constraints (锁定/限定) + relative pointer
// ------------------------------------------------------------------

// pointer-constraints-v1: 需要捕获鼠标的客户端 (QEMU/游戏/远控) 把指针锁定或
// 限定到自己的 surface. 同一时刻最多一个活动约束 (指针焦点所在 surface 上的);
// 原始增量仍经 relative-pointer-v1 送给客户端. (与 pointer-gestures-v1 无关.)

// 每约束记账: 客户端可随时销毁约束, 此时活动指针必须被丢弃
struct pointer_constraint {
	struct server *server;
	struct wlr_pointer_constraint_v1 *constraint;
	struct wl_listener destroy;
};

// 停止监视活动约束 surface 的 commit (没有在监视时调用也安全)
static void constraint_commit_untrack(struct server *server) {
	if (server->constraint_commit.link.prev != NULL) {
		wl_list_remove(&server->constraint_commit.link);
	}
}

static void pointer_constraint_destroy(struct wl_listener *listener,
		void *data) {
	struct pointer_constraint *pc =
		wl_container_of(listener, pc, destroy);
	if (pc->server->active_constraint == pc->constraint) {
		pc->server->active_constraint = NULL;
		// surface 随约束一起消失: 在 wlroots 断言 commit 信号为空之前摘掉 commit 监听器
		constraint_commit_untrack(pc->server);
	}
	wl_list_remove(&pc->destroy.link);
	free(pc);
}

// 把光标放到约束请求的提示位置 (锁定指针激活时可能要求 warp 到某个 surface 局部点)
static void pointer_warp_to_hint(struct server *server,
		struct wlr_pointer_constraint_v1 *constraint) {
	if (!constraint->current.cursor_hint.enabled ||
			!(constraint->current.committed &
				WLR_POINTER_CONSTRAINT_V1_STATE_CURSOR_HINT)) {
		return;
	}
	double ox, oy;
	if (!toplevel_surface_origin(server, constraint->surface, &ox, &oy)) {
		return;
	}
	double sx = constraint->current.cursor_hint.x;
	double sy = constraint->current.cursor_hint.y;
	wlr_cursor_warp(server->cursor, NULL, ox + sx, oy + sy);
	wlr_seat_pointer_warp(constraint->seat, sx, sy);
}

// 客户端把锁定指针的光标提示作为同步请求设置: 它只在锁定后的那次 surface commit
// 才进入 `current`, 所以提示必须在这里重新应用, 而不只在激活时
static void pointer_constraint_commit(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		constraint_commit);
	if (server->active_constraint != NULL) {
		pointer_warp_to_hint(server, server->active_constraint);
	}
}

static void constraint_commit_track(struct server *server,
		struct wlr_surface *surface) {
	constraint_commit_untrack(server);
	if (surface != NULL) {
		server->constraint_commit.notify = pointer_constraint_commit;
		wl_signal_add(&surface->events.commit, &server->constraint_commit);
	}
}

// 刚捕获指针 (锁定/限定) 的客户端现在拥有每个按钮:
// 正在进行的合成器手势永远看不到其释放 - 约束路径会消费它 - 从而让抓取状态卡住,
// 所以要结束它. 合成器已吞掉的按压仍记录在 bound_buttons / 和弦吞掉标志里,
// 所以它们的释放仍不会作为孤立事件投递给客户端.
static void cancel_compositor_gestures(struct server *server) {
	if (server->moving) {
		end_move(server);
	}
	if (server->resizing) {
		resize_grab_clear(server);
	}
	if (server->chord_active || server->zone_press) {
		end_chord(server);
		server->chord_toplevel = NULL;
	}
}

// 激活 `surface` 拥有的约束 (若有) 并停用上一个; 指针焦点变化时调用
static void set_active_constraint(struct server *server,
		struct wlr_surface *surface) {
	struct wlr_pointer_constraint_v1 *constraint = NULL;
	if (surface != NULL && server->pointer_constraints != NULL) {
		constraint = wlr_pointer_constraints_v1_constraint_for_surface(
			server->pointer_constraints, surface, server->seat);
	}
	if (constraint == server->active_constraint) {
		return;
	}
	struct wlr_pointer_constraint_v1 *old = server->active_constraint;
	// 客户端即将拥有指针: 丢弃正在进行的合成器手势, 使其无法让抓取状态卡住
	if (constraint != NULL) {
		cancel_compositor_gestures(server);
	}
	// 先指向替代者: send_deactivated() 可能销毁旧约束,
	// 其 destroy 处理器绝不能清除新的那个
	server->active_constraint = constraint;
	if (old != NULL) {
		// 旧约束不再拥有指针: 在它可能被销毁前停止跟踪其 surface commit
		constraint_commit_untrack(server);
		wlr_pointer_constraint_v1_send_deactivated(old);
	}
	if (constraint != NULL) {
		wlr_pointer_constraint_v1_send_activated(constraint);
		constraint_commit_track(server, constraint->surface);
		pointer_warp_to_hint(server, constraint);
		// 被捕获的指针由客户端绘制: 立即丢弃合成器自己的边框光标
		update_cursor_style(server);
	}
}

// 拥有指针焦点的 surface 上是否有约束 (锁定/限定) 活动?
// 有活动约束期间客户端拥有指针: 任何合成器手势都不得拦截其按钮或显示自己的光标.
bool pointer_constraint_active(struct server *server) {
	struct wlr_pointer_constraint_v1 *constraint = server->active_constraint;
	return constraint != NULL &&
		constraint->surface == server->seat->pointer_state.focused_surface;
}

void new_pointer_constraint(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		new_pointer_constraint);
	struct wlr_pointer_constraint_v1 *constraint = data;
	struct pointer_constraint *pc = calloc(1, sizeof(*pc));
	if (pc == NULL) {
		wlr_log(WLR_ERROR, "failed to allocate pointer constraint tracker");
		return;
	}
	pc->server = server;
	pc->constraint = constraint;
	pc->destroy.notify = pointer_constraint_destroy;
	wl_signal_add(&constraint->events.destroy, &pc->destroy);

	// 客户端 (QEMU 点击进入虚拟机时) 在其 surface 已有焦点时锁定指针:
	// 立即激活约束
	if (constraint->surface ==
			server->seat->pointer_state.focused_surface) {
		set_active_constraint(server, constraint->surface);
	}
}

void pointer_focus_change(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		pointer_focus_change);
	struct wlr_seat_pointer_focus_change_event *event = data;
	set_active_constraint(server, event->new_surface);
}

// relative-pointer 客户端的原始增量 (QEMU 相对鼠标模式);
// 每次 motion 都发送, 无论是否锁定 - 合成器光标可能不动, 但客户端自己的光标必须动
static void pointer_send_relative_motion(struct server *server,
		uint32_t time_msec, double dx, double dy,
		double dx_unaccel, double dy_unaccel) {
	if (server->relative_pointer_manager == NULL) {
		return;
	}
	wlr_relative_pointer_manager_v1_send_relative_motion(
		server->relative_pointer_manager, server->seat,
		(uint64_t)time_msec * 1000, dx, dy, dx_unaccel, dy_unaccel);
}

// 为活动约束调整待应用的相对移动. 光标完全不能移动 (锁定指针) 时返回 false.
static bool pointer_constraint_apply(struct server *server,
		double *dx, double *dy) {
	struct wlr_pointer_constraint_v1 *constraint = server->active_constraint;
	if (constraint == NULL ||
			constraint->surface !=
				server->seat->pointer_state.focused_surface) {
		return true;
	}
	if (constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
		return false;
	}
	// 限定: 钳制目标点, 让光标留在 surface 局部区域内.
	// seat 存储光标当前的 surface 局部位置, 对任何 surface 类型
	// (toplevel、popup、layer surface) 都有效, 无需重建场景原点.
	double sx = server->seat->pointer_state.sx;
	double sy = server->seat->pointer_state.sy;
	double cx, cy;
	if (!wlr_region_confine(&constraint->region, sx, sy,
			sx + *dx, sy + *dy, &cx, &cy)) {
		return true; // 光标不在区域内: 不动它
	}
	*dx = cx - sx;
	*dy = cy - sy;
	return true;
}

void cursor_motion(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	double dx = event->delta_x;
	double dy = event->delta_y;
	pointer_send_relative_motion(server, event->time_msec, dx, dy,
		event->unaccel_dx, event->unaccel_dy);
	if (!pointer_constraint_apply(server, &dx, &dy)) {
		return; // 锁定指针: 合成器光标保持不动
	}
	wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
	process_cursor_motion(server, event->time_msec);
}

void cursor_motion_absolute(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	// 绝对设备不带增量; 推导一个, 使 relative-pointer 客户端仍收到 motion,
	// 再让约束决定光标可以落在哪里
	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(server->cursor,
		&event->pointer->base, event->x, event->y, &lx, &ly);
	double dx = lx - server->cursor->x;
	double dy = ly - server->cursor->y;
	pointer_send_relative_motion(server, event->time_msec, dx, dy, dx, dy);
	if (!pointer_constraint_apply(server, &dx, &dy)) {
		return; // 锁定指针: 合成器光标保持不动
	}
	// 像 wlr_cursor_warp_absolute() 一样钳制: wlr_cursor_warp() 会静默丢弃
	// 取整后刚好落在设备映射之外的点
	wlr_cursor_warp_closest(server->cursor, &event->pointer->base,
		server->cursor->x + dx, server->cursor->y + dy);
	process_cursor_motion(server, event->time_msec);
}

void cursor_button(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	process_cursor_button(server, event->time_msec, event->button,
		event->state);
}

void cursor_axis(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	process_cursor_axis(server, event->time_msec, event);
}

void cursor_frame(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}
