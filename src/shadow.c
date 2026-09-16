// shadow.c - 窗口阴影策略 (scenefx 风格的高斯投影)
//
// 阴影由 rounded.c 的圆角着色器绘制, 表现为 SDF 距离外的高斯衰减.
// 本文件只管策略:
//   - 只有聚焦窗口投影 (未聚焦 sigma 为 0);
//   - 柔度 (sigma)、颜色、峰值透明度来自 config.h;
//   - 阴影颜色与边框色无关.
//
// 每个窗口 FBO 预留的边距由 sigma 推导: shadow_padding() = ceil(3.5 * sigma).
// 高斯在边距处已衰减到峰值的约 0.2%, 所以裁切边缘不可见.

#include "server.h"

#include <math.h>

float shadow_sigma(struct toplevel *tl) {
	// 只有聚焦窗口投影
	if (tl->server->focused != tl) {
		return 0.0f;
	}
	return CONFIG_SHADOW_BLUR_SIGMA;
}

float shadow_alpha(void) {
	return CONFIG_SHADOW_ALPHA;
}

struct wlr_render_color shadow_color(void) {
	return (struct wlr_render_color){
		.r = (float)((CONFIG_SHADOW_COLOR >> 16) & 0xff) / 255.0f,
		.g = (float)((CONFIG_SHADOW_COLOR >> 8) & 0xff) / 255.0f,
		.b = (float)(CONFIG_SHADOW_COLOR & 0xff) / 255.0f,
		.a = 1.0f,
	};
}

// 窗口 FBO 每侧预留的逻辑像素边距, 保证阴影不会碰到 FBO 边缘.
// 该值跨焦点变化保持常量, 因此焦点切换只触发 mask 重绘 (见 rounded.c).
int shadow_padding(void) {
	return (int)ceilf(CONFIG_SHADOW_BLUR_SIGMA * 3.5f);
}
