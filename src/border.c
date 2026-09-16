// border.c - 窗口边框策略 (宽度 + 颜色)
//
// 边框本身由 rounded.c 的圆角着色器绘制, 是窗口边缘内侧的一圈环.
// 本文件只管策略: 环的宽度和颜色.
//
// 环的顶部被分成三段 (对应标题栏手势区: 最小化/最大化/关闭), 各有一种颜色;
// 其余部分 (左右和底部) 使用依赖焦点的颜色. 因为基础色依赖焦点, 焦点切换时
// 调用 border_focus_changed(), 把受影响窗口的圆角 FBO 缓存标记为脏;
// 下一帧会用新颜色重绘.

#include "server.h"

#include <stdint.h>

static struct wlr_render_color border_hex_to_color(uint32_t hex) {
	return (struct wlr_render_color){
		.r = (float)((hex >> 16) & 0xff) / 255.0f,
		.g = (float)((hex >> 8) & 0xff) / 255.0f,
		.b = (float)(hex & 0xff) / 255.0f,
		.a = 1.0f,
	};
}

float border_width(struct toplevel *tl) {
	// 全屏窗口只有在显式开启时才画边框
	if (tl->fullscreen && !CONFIG_FULLSCREEN_BORDER) {
		return 0.0f;
	}
	// 可选: 未聚焦窗口不画边框环 (更扁平的外观).
	// 隐形的标题栏/resize 手势区与可见边框环无关, 交互不受影响.
	if (!CONFIG_BORDER_UNFOCUSED_DRAW && tl->server->focused != tl) {
		return 0.0f;
	}
	return (float)CONFIG_BORDER_WIDTH;
}

float border_gradient_width(struct toplevel *tl) {
	(void)tl;
	return (float)CONFIG_BORDER_GRADIENT_WIDTH;
}

struct wlr_render_color border_color(struct server *server,
		struct toplevel *tl) {
	// 全屏使用独立的单一边框色 (忽略焦点)
	if (tl->fullscreen) {
		return border_hex_to_color(CONFIG_FULLSCREEN_BORDER_COLOR);
	}
	return border_hex_to_color(server->focused == tl
		? CONFIG_BORDER_FOCUSED : CONFIG_BORDER_UNFOCUSED);
}

void border_top_colors(struct toplevel *tl,
		struct wlr_render_color *left, struct wlr_render_color *mid,
		struct wlr_render_color *right) {
	// 全屏边框是单色: 顶部不做三段
	if (tl->fullscreen) {
		struct wlr_render_color c =
			border_hex_to_color(CONFIG_FULLSCREEN_BORDER_COLOR);
		*left = *mid = *right = c;
		return;
	}
	// 对话框和固定尺寸窗口只有一个关闭条, 没有三个手势区:
	// 使用统一的 (依赖焦点的) 颜色, 顶部不分三段
	if (toplevel_is_dialog(tl) || toplevel_is_fixed_size(tl)) {
		struct wlr_render_color c = border_color(tl->server, tl);
		*left = *mid = *right = c;
		return;
	}
	*left = border_hex_to_color(CONFIG_BORDER_TOP_LEFT);
	*mid = border_hex_to_color(CONFIG_BORDER_TOP_MID);
	*right = border_hex_to_color(CONFIG_BORDER_TOP_RIGHT);
}

void border_focus_changed(struct toplevel *tl, struct toplevel *prev) {
	// 边框颜色依赖焦点: 重绘新聚焦窗口和先前聚焦窗口的圆角 FBO,
	// 让它们取到新的聚焦/未聚焦颜色. 内容没变, 所以是只重绘 mask:
	// 复用已缓存的内容 pass, 只重跑 SDF 着色器 (见 rounded.c).
	rounded_cache_dirty_mask(tl);
	if (prev != NULL) {
		rounded_cache_dirty_mask(prev);
	}
}
