// place.c - 窗口放置 (自动居中)
//
// 窗口保持客户端提交的尺寸 (合成器不重新设尺寸, 这里不调用
// wlr_xdg_toplevel_set_size), 并在输出 (屏幕) 上水平垂直居中.
// 新窗口居中到光标所在的屏幕, 之后只要 surface 尺寸变化就重新居中
// (Electron 窗口如 QQ 常先映射一个小的占位 surface, 稍后才提交真实尺寸),
// 这样无论真实尺寸何时到达, 窗口最终都会居中. 一旦用户与窗口交互
// (移动/缩放/最大化/全屏会置 toplevel.user_moved), 自动居中就停止,
// 窗口停在用户放的位置.
//
// 居中的参考是 SURFACE 的实际渲染范围, 不是 xdg 窗口 geometry:
// Electron 窗口 (QQ) 提交的 surface 比它的 geometry 大, 按较小的 geometry
// 居中会让真实内容偏向右下, 甚至跑出屏幕. (普通窗口 surface == geometry;
// 带透明投影的 CSD 窗口二者中心只差半个阴影边距, 可忽略.)
//
// CONFIG_CENTER_AVOID_BARS 把居中参考从整个输出框换成工作区
// (输出减去 layer-shell 独占区, 即状态栏), 这样状态栏永远盖不住新窗口.

#include "server.h"

#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>

// 把 toplevel 居中到光标所在的输出 (光标不在任何输出上时退回中心输出).
// 窗口还没有可用尺寸时返回 false; 定位完成后返回 true.
// 尺寸不动; 位置让 surface 的渲染范围 (node - geometry + surface size) 居中.
// 比参考区更大的窗口对齐到参考区左上角, 避免一开始就在屏幕外;
// 更小的窗口会做钳制, 保证没有任何部分落在屏幕外.
bool place_toplevel(struct server *server, struct toplevel *tl) {
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	if (base == NULL || base->surface == NULL) {
		return false;
	}
	struct wlr_surface *surface = base->surface;
	int sw = surface->current.width;
	int sh = surface->current.height;
	if (sw <= 0 || sh <= 0) {
		return false;
	}
	// xdg surface 树锚定在 geometry 左上角, surface 位于树内的 -geometry 处,
	// 所以 surface 左上角是 (node.x - gx, node.y - gy), 窗口真实渲染范围
	// 就是该角加上 surface 尺寸. 按这个范围 (而不是 xdg geometry) 居中,
	// 才能让 surface 比 geometry 大的窗口 (QQ) 保持居中.
	int gx = base->geometry.x;
	int gy = base->geometry.y;

	// 居中到光标所在的输出 (窗口应出现在用户看的地方), 退回中心输出
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (output == NULL) {
		output = wlr_output_layout_get_center_output(server->output_layout);
	}
	if (output == NULL) {
		return false;
	}

	struct wlr_box ref; // 居中参考: 输出框或工作区
#if CONFIG_CENTER_AVOID_BARS
	get_work_area(server, output, &ref);
#else
	wlr_output_layout_get_box(server->output_layout, output, &ref);
#endif

	// 让 surface 左上角落在此处的 node 位置
	int rx = ref.x + (ref.width - sw) / 2;
	int ry = ref.y + (ref.height - sh) / 2;

	// 保持 surface 在屏幕内
	if (sw >= ref.width) {
		rx = ref.x; // 比屏幕宽: 左对齐
	} else {
		if (rx < ref.x) {
			rx = ref.x;
		} else if (rx + sw > ref.x + ref.width) {
			rx = ref.x + ref.width - sw;
		}
	}
	if (sh >= ref.height) {
		ry = ref.y; // 比屏幕高: 顶对齐
	} else {
		if (ry < ref.y) {
			ry = ref.y;
		} else if (ry + sh > ref.y + ref.height) {
			ry = ref.y + ref.height - sh;
		}
	}

	wlr_scene_node_set_position(&tl->scene_tree->node, rx + gx, ry + gy);
	return true;
}
