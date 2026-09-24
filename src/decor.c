// decor.c - 用 scenefx 绘制窗口圆角、边框与投影
// 内容通过 wlr_scene_subsurface_tree_set_clip 裁切到窗口几何 (去掉 CSD 边距/投影),
// 再按 buffer 与窗口四角的位置关系用 wlr_scene_buffer_set_corner_radii 做圆角
// (根 surface 和覆盖整窗的 subsurface 会拿到四角, 窗口内部的 subsurface 不会).

#include "server.h"

#include <math.h>
#include <string.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>

// 遍历场景 buffer 时带上的上下文
struct decor_iter {
	// 四个内容角各自要用的圆角半径 (贴状态栏的角为 0)
	struct fx_corner_radii corners;
	// 窗口内容区尺寸与原点 (scene_tree 坐标系), 用于判断 buffer 是否落在窗口四角
	int window_w, window_h;
	int origin_x, origin_y;
};

static void color_from_hex(uint32_t hex, float alpha, float out[4]) {
	out[0] = (float)((hex >> 16) & 0xff) / 255.0f * alpha;
	out[1] = (float)((hex >> 8) & 0xff) / 255.0f * alpha;
	out[2] = (float)(hex & 0xff) / 255.0f * alpha;
	out[3] = alpha;
}

// 窗口外角半径: 全屏窗口单独配置 (0 = 直角)
static int decor_radius(struct toplevel *tl) {
	if (tl->fullscreen) {
		return CONFIG_FULLSCREEN_ROUNDED_RADIUS;
	}
	return CONFIG_ROUNDED_RADIUS;
}

float decor_border_width(struct toplevel *tl) {
	// 全屏窗口只有在显式开启时才画边框
	if (tl->fullscreen && !CONFIG_FULLSCREEN_BORDER) {
		return 0.0f;
	}
	// 可选: 未聚焦窗口不画边框环
	if (!CONFIG_BORDER_UNFOCUSED_DRAW && tl->server->focused != tl) {
		return 0.0f;
	}
	return (float)CONFIG_BORDER_WIDTH;
}

static void decor_border_color(struct toplevel *tl, float out[4]) {
	uint32_t hex;
	if (tl->fullscreen) {
		hex = CONFIG_FULLSCREEN_BORDER_COLOR;
	} else if (tl->server->focused == tl) {
		hex = CONFIG_BORDER_FOCUSED;
	} else {
		hex = CONFIG_BORDER_UNFOCUSED;
	}
	// 边框色不透明
	color_from_hex(hex, 1.0f, out);
}

static float decor_smoothstep(float t) {
	if (t <= 0.0f) {
		return 0.0f;
	}
	if (t >= 1.0f) {
		return 1.0f;
	}
	return t * t * (3.0f - 2.0f * t);
}

static void decor_mix_color(const float a[4], const float b[4], float t,
		float out[4]) {
	for (int i = 0; i < 4; i++) {
		out[i] = a[i] + (b[i] - a[i]) * t;
	}
}

// 顶部三段的颜色策略:
//   - 全屏: 全部用全屏边框色;
//   - 对话框 / 固定尺寸窗口: 只有关闭条, 全部用基础(焦点)色;
//   - 其余: 左/中/右分别取 CONFIG_BORDER_TOP_*.
static void decor_top_colors(struct toplevel *tl, float left[4],
		float mid[4], float right[4]) {
	if (tl->fullscreen || toplevel_is_dialog(tl) ||
			toplevel_is_fixed_size(tl)) {
		decor_border_color(tl, left);
		memcpy(mid, left, sizeof(float) * 4);
		memcpy(right, left, sizeof(float) * 4);
		return;
	}
	color_from_hex(CONFIG_BORDER_TOP_LEFT, 1.0f, left);
	color_from_hex(CONFIG_BORDER_TOP_MID, 1.0f, mid);
	color_from_hex(CONFIG_BORDER_TOP_RIGHT, 1.0f, right);
}

// 配置一段顶部边框: 用整高 rect + clipped_region 只保留顶部 bw 高.
// 必须整高, 否则小高度 rect 的外圆角会被 clamp 掉, 与窗口圆角对不上.
static void decor_top_segment(struct toplevel *tl, int idx, int x, int width,
		int bw, int h, int radius_tl, int radius_tr, const float color[4]) {
	struct wlr_scene_rect *r = tl->border_top[idx];
	if (r == NULL) {
		return;
	}
	if (width <= 0 || bw <= 0) {
		wlr_scene_node_set_enabled(&r->node, false);
		return;
	}
	wlr_scene_node_set_enabled(&r->node, true);
	// 边框画在内容外侧, 顶部条整体上移一个边框宽 (bw)
	wlr_scene_node_set_position(&r->node, x, -bw);
	wlr_scene_rect_set_size(r, width, h);
	wlr_scene_rect_set_corner_radii(r,
		corner_radii_new(radius_tl, radius_tr, 0, 0));
	if (h > bw) {
		wlr_scene_rect_set_clipped_region(r, (struct clipped_region){
			.area = { 0, bw, width, h - bw },
			.corners = corner_radii_all(0),
		});
	} else {
		wlr_scene_rect_set_clipped_region(r, clipped_region_get_default());
	}
	wlr_scene_rect_set_color(r, color);
}

// 交界渐变: 在 [x0,x1) 铺 STEPS 条小 rect, 颜色按 smoothstep 在 c0/c1 间插值
static void decor_top_gradient(struct toplevel *tl, int base_idx, int x0,
		int x1, int bw, const float c0[4], const float c1[4]) {
	int span = x1 - x0;
	for (int i = 0; i < CONFIG_BORDER_GRADIENT_STEPS; i++) {
		struct wlr_scene_rect *r = tl->border_top[base_idx + i];
		if (r == NULL) {
			continue;
		}
		int a = x0 + span * i / CONFIG_BORDER_GRADIENT_STEPS;
		int b = x0 + span * (i + 1) / CONFIG_BORDER_GRADIENT_STEPS;
		int width = b - a;
		if (width <= 0 || bw <= 0) {
			wlr_scene_node_set_enabled(&r->node, false);
			continue;
		}
		float t = ((float)i + 0.5f) /
			(float)CONFIG_BORDER_GRADIENT_STEPS;
		float color[4];
		decor_mix_color(c0, c1, decor_smoothstep(t), color);
		wlr_scene_node_set_enabled(&r->node, true);
		wlr_scene_node_set_position(&r->node, a, -bw);
		wlr_scene_rect_set_size(r, width, bw);
		wlr_scene_rect_set_corner_radii(r, corner_radii_all(0));
		wlr_scene_rect_set_clipped_region(r, clipped_region_get_default());
		wlr_scene_rect_set_color(r, color);
	}
}

// 重排顶部三段边框 (窗口几何 / 焦点 / 全屏变化时调用)
static void decor_border_top_update(struct toplevel *tl) {
	if (tl->scene_tree == NULL) {
		return;
	}
	struct wlr_box box;
	toplevel_frame_box(tl->server, tl, &box);
	int w = box.width;
	int h = box.height;
	int bw = (int)roundf(decor_border_width(tl));
	if (w <= 0 || h <= 0 || bw <= 0) {
		for (int i = 0; i < DECOR_BORDER_TOP_RECTS; i++) {
			if (tl->border_top[i] != NULL) {
				wlr_scene_node_set_enabled(&tl->border_top[i]->node,
					false);
			}
		}
		return;
	}
	if (bw > h) {
		bw = h;
	}

	float left[4], mid[4], right[4];
	decor_top_colors(tl, left, mid, right);

	// 以窗口宽度三等分, 交界处留出渐变带
	float third = (float)w / 3.0f;
	float g = (float)CONFIG_BORDER_GRADIENT_WIDTH;
	float maxg = third / 2.0f;
	if (g > maxg) {
		g = maxg;
	}
	if (g < 0.0f) {
		g = 0.0f;
	}
	int b1 = (int)lroundf(third - g);
	int b2 = (int)lroundf(third + g);
	int b3 = (int)lroundf(2.0f * third - g);
	int b4 = (int)lroundf(2.0f * third + g);
	if (b1 < 0) {
		b1 = 0;
	}
	if (b2 < b1) {
		b2 = b1;
	}
	if (b3 < b2) {
		b3 = b2;
	}
	if (b4 < b3) {
		b4 = b3;
	}
	if (b4 > w) {
		b4 = w;
	}

	int radius = decor_radius(tl);
	int outer_radius = radius + bw; // 边框外圆角 (与内容内孔同心)
	int off = -bw; // 外框上移一个边框宽, 与 decor_update 的边框环一致
	// 左/右段带外圆角 (整高 + 裁顶带), 中段无圆角
	decor_top_segment(tl, 0, off, b1, bw, h, outer_radius, 0, left);
	decor_top_segment(tl, 1, off + b2, b3 - b2, bw, h, 0, 0, mid);
	decor_top_segment(tl, 2, off + b4, w - b4, bw, h, 0, outer_radius, right);
	decor_top_gradient(tl, 3, off + b1, off + b2, bw, left, mid);
	decor_top_gradient(tl, 3 + CONFIG_BORDER_GRADIENT_STEPS, off + b3,
		off + b4, bw, mid, right);
}

static bool decor_shadow_enabled(struct toplevel *tl) {
	if (CONFIG_SHADOW_BLUR_SIGMA <= 0.0f || CONFIG_SHADOW_ALPHA <= 0.0f) {
		return false;
	}
	// 只有聚焦窗口投影
	if (tl->server->focused != tl) {
		return false;
	}
	// 最大化 / 全屏窗口铺满作区/输出: 阴影没有意义
	if (tl->fullscreen) {
		return false;
	}
	if (tl->xdg_toplevel != NULL &&
			tl->xdg_toplevel->current.maximized &&
			!tl->restore_frame_pending) {
		return false;
	}
	return true;
}

// scenefx 的 wlr_scene_shadow (box_shadow.frag) 与旧 FBO 阴影约定不同:
//   1) 实心矩形从阴影节点四边各内缩 blur_sigma, 高斯宽度取 blur_sigma * 0.5,
//      即高斯 sigma = blur_sigma / 2. 所以想让衰减宽度等于 config 的 sigma,
//      要传 blur_sigma = 2 * sigma;
//   2) 阴影节点外缘到实心矩形边恰好是 blur_sigma = 2 * 高斯 sigma, 也就是
//      节点边缘处阴影 mask 还剩 Phi(-2) ≈ 2.3%. 如果让实心矩形正好是窗口
//      外框 (旧做法), 阴影会在离窗口 2 * sigma 处突然截断, 留下一圈可见的
//      硬边/环. 这里把实心矩形再向外扩 DECOR_SHADOW_OVERSHOOT 倍 sigma,
//      使窗口边缘落在实心区内部 (mask 接近 1), 节点边缘的残余比例不变但对应
//      的颜色 alpha 更小, 硬边因此被压下去;
//   3) 阴影按直线色绘制 (混合用 GL_SRC_ALPHA), 不能像 rect 那样预乘.
#define DECOR_SHADOW_OVERSHOOT 1.0f // 实心矩形相对窗口外框向外扩张的 sigma 倍数

// 实心矩形相对窗口外框向外扩张量 (逻辑像素)
static float decor_shadow_overshoot(void) {
	return CONFIG_SHADOW_BLUR_SIGMA * DECOR_SHADOW_OVERSHOOT;
}

// scenefx 高斯 sigma = blur_sigma / 2, 故 blur = 2 * sigma
static float decor_shadow_blur(void) {
	return CONFIG_SHADOW_BLUR_SIGMA * 2.0f;
}

// 阴影节点每侧预留的边距 = 实心矩形内缩 (blur) + 向外扩张量
static int decor_shadow_padding(void) {
	return (int)ceilf(decor_shadow_blur() + decor_shadow_overshoot());
}

// 窗口外框边缘落在实心区内部 overshoot 处, mask = Phi(overshoot / sigma)
static float decor_shadow_edge_mask(void) {
	float x = decor_shadow_overshoot() / CONFIG_SHADOW_BLUR_SIGMA;
	return 0.5f * (1.0f + erff(x / 1.41421356f));
}

static void decor_shadow_color(struct toplevel *tl, float out[4]) {
	float alpha = 0.0f;
	if (decor_shadow_enabled(tl)) {
		float mask = decor_shadow_edge_mask();
		alpha = mask > 0.0f ? CONFIG_SHADOW_ALPHA / mask : CONFIG_SHADOW_ALPHA;
	}
	if (alpha > 1.0f) {
		alpha = 1.0f;
	}
	// 直线色 (不预乘), 与 box_shadow 的 SRC_ALPHA 混合一致
	out[0] = (float)((CONFIG_SHADOW_COLOR >> 16) & 0xff) / 255.0f;
	out[1] = (float)((CONFIG_SHADOW_COLOR >> 8) & 0xff) / 255.0f;
	out[2] = (float)(CONFIG_SHADOW_COLOR & 0xff) / 255.0f;
	out[3] = alpha;
}

static void decor_iter_buffer(struct wlr_scene_buffer *buffer, int sx, int sy,
		void *user_data) {
	struct decor_iter *it = user_data;
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(buffer);
	if (scene_surface == NULL) {
		return;
	}
	struct wlr_xdg_surface *xdg =
		wlr_xdg_surface_try_from_wlr_surface(scene_surface->surface);
	// popup 内容不参与窗口圆角
	if (xdg != NULL && xdg->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}
	// 不是 xdg surface 也不是 subsurface (理论上不会出现在窗口树里): 跳过
	if (xdg == NULL &&
			wlr_subsurface_try_from_wlr_surface(scene_surface->surface) == NULL) {
		return;
	}

	// 按 buffer 与窗口几何的位置关系, 只给真正落在窗口四角的那些角上圆角:
	// 根 surface 和覆盖整窗的 subsurface (Firefox/Chromium) 会拿到四个角,
	// 窗口内部的 subsurface 不会被误加圆角. sx/sy 含 scene_tree 自身的原点,
	// 所以先减掉 origin 换算到窗口内容区坐标.
	int bw = buffer->dst_width;
	int bh = buffer->dst_height;
	if (bw <= 0 || bh <= 0) {
		if (buffer->buffer == NULL) {
			return;
		}
		bw = buffer->buffer->width;
		bh = buffer->buffer->height;
	}
	int left = sx - it->origin_x;
	int top = sy - it->origin_y;
	int right = left + bw;
	int bottom = top + bh;
	int tl = 0, tr = 0, br = 0, bl = 0;
	if (left <= 0 && top <= 0 && right > 0 && bottom > 0) {
		tl = it->corners.top_left;
	}
	if (right >= it->window_w && top <= 0 &&
			left < it->window_w && bottom > 0) {
		tr = it->corners.top_right;
	}
	if (left <= 0 && bottom >= it->window_h &&
			right > 0 && top < it->window_h) {
		bl = it->corners.bottom_left;
	}
	if (right >= it->window_w && bottom >= it->window_h &&
			left < it->window_w && top < it->window_h) {
		br = it->corners.bottom_right;
	}
	wlr_scene_buffer_set_corner_radii(buffer,
		corner_radii_new(tl, tr, br, bl));
}

// 创建边框与阴影节点, 并插入正确的场景层级
void decor_attach(struct toplevel *tl) {
	if (tl->scene_tree == NULL) {
		return;
	}
	float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};

	// 模糊放在阴影之上、内容之下: 先把 blur 落到底部, 再由后面建的阴影压到它下面
	if (CONFIG_BLUR) {
		tl->blur = wlr_scene_blur_create(tl->scene_tree, 0, 0);
		if (tl->blur != NULL) {
			wlr_scene_node_lower_to_bottom(&tl->blur->node);
			wlr_scene_node_set_enabled(&tl->blur->node, false);
		}
	}

	// 阴影放在内容 (与模糊) 之下
	tl->shadow = wlr_scene_shadow_create(tl->scene_tree, 0, 0, 0, 0.0f, clear);
	if (tl->shadow != NULL) {
		wlr_scene_node_lower_to_bottom(&tl->shadow->node);
		wlr_scene_node_set_enabled(&tl->shadow->node, false);
	}

	// 边框放在内容之上 (scene_tree 已有内容树, 新节点自然在其上)
	tl->border = wlr_scene_rect_create(tl->scene_tree, 0, 0, clear);
	if (tl->border != NULL) {
		wlr_scene_node_raise_to_top(&tl->border->node);
		wlr_scene_node_set_enabled(&tl->border->node, false);
	}

	// 顶部三段边框节点, 盖在基础边框环之上 (创建在后即在上)
	for (int i = 0; i < DECOR_BORDER_TOP_RECTS; i++) {
		tl->border_top[i] = wlr_scene_rect_create(tl->scene_tree, 0, 0,
			clear);
		if (tl->border_top[i] != NULL) {
			wlr_scene_node_set_enabled(&tl->border_top[i]->node, false);
		}
	}
}

// 给窗口内容 buffer 重放圆角 (由 decor_update 在每次 commit/焦点变化/全屏切换时
// 调用; 值未变时 scenefx 会直接返回). w/h 为窗口内容区 (geometry) 尺寸.
static void decor_apply_rounding(struct toplevel *tl, int w, int h,
		struct fx_corner_radii corners) {
	if (tl->scene_tree == NULL) {
		return;
	}
	struct decor_iter it = {
		.corners = corners,
		.window_w = w,
		.window_h = h,
		.origin_x = tl->scene_tree->node.x,
		.origin_y = tl->scene_tree->node.y,
	};
	wlr_scene_node_for_each_buffer(&tl->scene_tree->node,
		decor_iter_buffer, &it);
}

// 背景模糊: 和 swayfx 一样对整块窗口内容区做模糊, 不设透明掩码, 也不额外叠色,
// 所以悬浮/透明窗口下面会自然透出被模糊的背景, 玻璃颜色随窗口自身背景色变化.
static bool decor_blur_enabled(struct toplevel *tl) {
	(void)tl;
	return CONFIG_BLUR;
}

// 根据窗口状态刷新圆角 / 边框 / 阴影几何与颜色
void decor_update(struct toplevel *tl) {
	if (tl->scene_tree == NULL) {
		return;
	}
	// 最小化的窗口整棵场景子树被禁用, 渲染本就不会发生; 这里跳过重建,
	// 免得每次 commit 都重设边框/阴影/模糊和 surface clip. 重新可见时
	// set_minimized() 会再调一次 decor_update, 把 (含 buffer 圆角的) 状态补齐.
	//
	// 尚未映射的窗口同样跳过: 初始提交可能已带 window geometry, 但窗口还
	// 没被 place (map 处理器才做), 直接画会让边框/阴影闪在屏幕左上角.
	// 旧实现把边框烘进离屏 FBO 且只在 mapped 时才渲染, 因此没有这个问题.
	// map 处理器会先 place 再调 decor_update, 所以映射后立刻补上.
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	if (tl->minimized || base == NULL || base->surface == NULL ||
			!base->surface->mapped) {
		return;
	}
	// 外框 = 客户端 window geometry 向外扩一个边框宽 (边框画在内容外侧).
	// 内容 = 外框内缩一个边框宽, 与边框内孔完全重合.
	struct wlr_box box;
	toplevel_frame_box(tl->server, tl, &box);
	int fw = box.width;
	int fh = box.height;
	int bw = (int)roundf(decor_border_width(tl));
	if (bw < 0) {
		bw = 0;
	}
	int w = fw - 2 * bw;
	int h = fh - 2 * bw;
	if (fw <= 0 || fh <= 0 || w <= 0 || h <= 0) {
		if (tl->border != NULL) {
			wlr_scene_node_set_enabled(&tl->border->node, false);
		}
		if (tl->shadow != NULL) {
			wlr_scene_node_set_enabled(&tl->shadow->node, false);
		}
		if (tl->blur != NULL) {
			wlr_scene_node_set_enabled(&tl->blur->node, false);
		}
		decor_border_top_update(tl);
		return;
	}

	int radius = decor_radius(tl);      // 内容圆角 (也是边框内孔半径)
	int outer_radius = radius + bw;     // 边框外圆角 (与内孔同心)
	struct fx_corner_radii content_corners = corner_radii_all(radius);
	struct fx_corner_radii outer_corners = corner_radii_all(outer_radius);

	// 内容裁切到窗口几何, 去掉 CSD 边距/投影 (surface > geometry 时).
	// clip 的坐标空间是根 surface: scene_tree 把 surface 放在树内 -geometry 处,
	// 所以要加上 geometry 偏移, 否则 CSD 窗口的内容会偏出左上角. 只裁 toplevel
	// 自身的 surface 树 (与 dwl 的 c->scene_surface 对应), 不递归到 popup.
	struct wlr_box clip = {
		.x = base->geometry.x,
		.y = base->geometry.y,
		.width = w,
		.height = h,
	};
	struct wlr_scene_node *content = tl->surface_tree != NULL
		? &tl->surface_tree->node : &tl->scene_tree->node;
	wlr_scene_subsurface_tree_set_clip(content, &clip);

	// 裁切/位置更新后再重放圆角 (首个 map 提交的几何可能是 0x0/无效,
	// 那时 surface 树/buffer 还没就绪; commit 时场景已重配好). 幂等.
	decor_apply_rounding(tl, w, h, content_corners);

	// 边框环: 外框大小 rect, 挖掉内容区, 画在内容之外 (内容不再被盖住)
	if (tl->border != NULL) {
		if (bw > 0) {
			float color[4];
			decor_border_color(tl, color);
			wlr_scene_node_set_enabled(&tl->border->node, true);
			wlr_scene_node_set_position(&tl->border->node, -bw, -bw);
			wlr_scene_rect_set_size(tl->border, fw, fh);
			wlr_scene_rect_set_corner_radii(tl->border, outer_corners);
			wlr_scene_rect_set_clipped_region(tl->border,
				(struct clipped_region){
					.area = { bw, bw, w, h },
					.corners = content_corners,
				});
			wlr_scene_rect_set_color(tl->border, color);
		} else {
			wlr_scene_node_set_enabled(&tl->border->node, false);
		}
	}

	// 投影: 挖孔取外框 (边框外缘), 阴影落在整个窗口之外
	if (tl->shadow != NULL) {
		if (decor_shadow_enabled(tl)) {
			float color[4];
			decor_shadow_color(tl, color);
			int pad = decor_shadow_padding();
			int sw = fw + 2 * pad;
			int sh = fh + 2 * pad;
			wlr_scene_node_set_enabled(&tl->shadow->node, true);
			wlr_scene_node_set_position(&tl->shadow->node, -bw - pad,
				-bw - pad);
			wlr_scene_shadow_set_size(tl->shadow, sw, sh);
			// 实心矩形 = 节点内缩 blur_sigma, 比窗口外框向外多扩 overshoot.
			// 圆角矩形向外偏移时圆心不动、半径增大, 所以要同步 +overshoot
			// 才能与窗口外框同心; 直接用 outer_radius 会让四角阴影偏胖.
			int solid_radius = outer_radius +
				(int)lroundf(decor_shadow_overshoot());
			wlr_scene_shadow_set_corner_radius(tl->shadow, solid_radius);
			// scenefx 会把阴影节点的位置/尺寸/圆角按输出 scale 放大, 却不缩放
			// blur_sigma. 实心矩形在输出像素里的内缩必须等于 blur * scale, 才能真正
			// 落在窗口外框之外 overshoot 处; 传入逻辑 blur 会让缩放输出上的实心
			// 矩形偏小, 阴影会挤进窗口内部 (被 clipped_region 挖掉). (跨多个不同
			// scale 输出的窗口只能取主输出.)
			struct wlr_output *output = toplevel_output(tl->server, tl);
			float output_scale = output != NULL ? output->scale : 1.0f;
			wlr_scene_shadow_set_blur_sigma(tl->shadow,
				decor_shadow_blur() * output_scale);
			wlr_scene_shadow_set_clipped_region(tl->shadow,
				(struct clipped_region){
					.area = { pad, pad, fw, fh },
					.corners = corner_radii_all(outer_radius),
				});
			wlr_scene_shadow_set_color(tl->shadow, color);
		} else {
			wlr_scene_node_set_enabled(&tl->shadow->node, false);
		}
	}

	// 背景模糊: 覆盖整块窗口内容区, 圆角与窗口一致. 和 swayfx 一样不设透明掩码:
	// 模糊整块背景, 由窗口自身的半透明内容去混合, 所以不需要指定任何颜色.
	if (tl->blur != NULL) {
		if (decor_blur_enabled(tl)) {
			wlr_scene_node_set_enabled(&tl->blur->node, true);
			wlr_scene_node_set_position(&tl->blur->node, 0, 0);
			wlr_scene_blur_set_size(tl->blur, w, h);
			wlr_scene_blur_set_corner_radii(tl->blur, content_corners);
			// 采样背景预模糊缓存 (只模糊 background/bottom 层): 全屏/最大化
			// 铺满屏幕, 实时模糊代价最高, 缓存收益最大. 缓存由 output.c 维护,
			// layer.c 在背景变化时置脏 (见 output_blur_layer_mark_dirty).
			bool optimize = tl->fullscreen ||
				(tl->xdg_toplevel != NULL &&
				 tl->xdg_toplevel->current.maximized &&
				 !tl->restore_frame_pending);
			wlr_scene_blur_set_should_only_blur_bottom_layer(tl->blur,
				optimize);
		} else {
			wlr_scene_node_set_enabled(&tl->blur->node, false);
		}
	}

	// 顶部三段边框 (随窗口几何/焦点/全屏变化重排)
	decor_border_top_update(tl);
}

// 窗口 unmap 后, 内容子树由 wlroots 自身禁用, 但边框/阴影/模糊是 scene_tree
// 的兄弟节点, 不会随之隐藏, 会留下"幽灵"装饰. 这里显式关闭; map 时
// decor_update() 会按当前状态重新启用并补齐几何.
void decor_hide(struct toplevel *tl) {
	if (tl->border != NULL) {
		wlr_scene_node_set_enabled(&tl->border->node, false);
	}
	for (int i = 0; i < DECOR_BORDER_TOP_RECTS; i++) {
		if (tl->border_top[i] != NULL) {
			wlr_scene_node_set_enabled(&tl->border_top[i]->node, false);
		}
	}
	if (tl->shadow != NULL) {
		wlr_scene_node_set_enabled(&tl->shadow->node, false);
	}
	if (tl->blur != NULL) {
		wlr_scene_node_set_enabled(&tl->blur->node, false);
	}
}

void decor_focus_changed(struct toplevel *tl, struct toplevel *prev) {
	decor_update(tl);
	if (prev != NULL && prev != tl) {
		decor_update(prev);
	}
}
