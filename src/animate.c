// animate.c - 窗口动画
//
//   创建 (map)  -> 淡入  (透明度 0 -> 1)
//   关闭         -> 淡出 (透明度 1 -> 0, 然后发送 xdg close); 请求关闭的瞬间
//                   窗口就变成惰性 (closing 标志), 完全透明后禁用其场景节点,
//                   这样不理会关闭请求的客户端也不会留下不可见却阻塞输入的窗口.
//                   关闭一个仍在落下 (最小化) 的窗口会保留掉落的同时淡出;
//                   刚 map 就关闭则从当前透明度开始淡出, 不会闪回.
//   最小化       -> 窗口从当前位置垂直下落, 直到完全低于其输出下边缘,
//                   然后隐藏场景节点 (隐藏时弹回静止原点, 几何记账保持不变)
//   还原         -> 隐藏的窗口从自身顶边上方落回, 精确落回原位
//
// 运动作用在 toplevel 的场景树节点 (位置) 上, 淡变作用在可见窗口 buffer 上
// (经 rounded_window_set_opacity()) - 所以圆角副本、边框和阴影一起移动/淡变.
//
// 时间模型: 动画状态不由相位会相对显示刷新漂移的每窗口定时器推进.
// 而是由每个输出的 frame 处理器 (output.c) 在场景渲染前把所有运行中的动画
// 推进到它自己的 CLOCK_MONOTONIC 时刻, 所以每个渲染帧显示的正是它自己 vblank
// 对应的缓动状态 - 不存在更新定时器与 vblank 的节拍错位, 高刷输出会插值出
// 更多状态. 每次状态变化产生的场景 damage 会让输出在动画期间每个 vblank 都渲染.
// 一个全局节拍看门狗 (仅在有动画运行时臂置, 固定 16ms 下限 - 见 ANIM_WATCHDOG_MS)
// 负责 damage 无法驱动的阶段: 窗口完全移出输出 (落下尾部/落回头部, 其 damage 可能被裁掉)
// 以及墙钟超时 (客户端始终不提交目标尺寸的最大化缩放).
//
// 所有入口在动画被禁用或无法运行时返回 false (且不改变任何东西),
// 调用方据此退回瞬时、无动画的行为.

#include "server.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>

#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

// 节拍看门狗下限: 动画运行期间看门狗请求输出帧的最小间隔.
// 这不是动画帧率 - 动画状态按输出自己的 vblank 每渲染帧推进一次, 完全与合成器同步
// (见文件头注释). 看门狗只保证不可见阶段 (窗口完全移出输出, damage 被裁掉)
// 和墙钟超时继续推进; 16ms 足够短, 任何输出上的动画都不会明显迟结束或迟进入.
#define ANIM_WATCHDOG_MS 16

// 一个窗口同一时刻只能运行一种动画
enum anim_kind {
	ANIM_NONE = 0,
	ANIM_FADE_IN,   // map: 透明度 0 -> 1, 不移动
	ANIM_FADE_OUT,  // close: 透明度 1 -> 0, 然后发送 close
	ANIM_FALL_OUT,  // minimize: 落到屏幕下边缘之外
	ANIM_FALL_IN,   // restore: 从窗口上方落回
	ANIM_GEOM,      // maximize/restore: 在两个框之间缩放
};

struct toplevel_anim {
	struct toplevel *tl;

	// 动画中途窗口的场景树销毁 (窗口关闭): 释放状态, 避免再触碰已释放的 toplevel
	struct wl_listener tree_destroy;

	enum anim_kind kind;

	// 静止原点: 窗口在屏幕上该待的地方. 最小化开始时 (落下前) 捕获,
	// 这样打断落下过程的还原仍能把窗口落回真正的位置.
	bool has_rest;
	int rest_x, rest_y;

	// 当前运行: 缓动位置/透明度插值端点
	int from_x, from_y; // 动画起始位置
	int to_x, to_y;     // 动画结束位置
	float op_from, op_to; // 透明度范围 (仅淡变)
	float op_cur;         // 当前应用的透明度 (淡变): 让打断的运行从上一次
	                       // 实际停留的位置开始 (map 淡入途中关闭)
	int win_w, win_h;   // 窗口尺寸 (扫描区域 damage)

	// 最小化落下/还原落回的缩放 (掉落): 窗口落下时围绕自己的 (移动中的) 中心
	// 缩小到 CONFIG_ANIM_FALL_SCALE, 落回时从该值放大回 1.0.
	// 仅当存在可缩放的圆角 FBO 时按运行启用.
	bool scale_fall;
	float scale_from;   // p = 0 时的缩放: 1.0 (落下), CONFIG (落回)

	// ANIM_GEOM (Windows 式最大化/还原缩放): 在 geom_from 与 geom_to 之间插值的窗口框.
	// 缩放从等待阶段开始 - 只有客户端提交了目标尺寸且圆角缓存按该尺寸重绘后,
	// 目标尺寸的 FBO 才存在 - 所以绝不会在两种内容布局之间跳变.
	struct wlr_box geom_from, geom_to;
	bool geom_zooming;  // false = 等待目标尺寸的 FBO

	uint32_t start_ms;  // 运行开始时的 CLOCK_MONOTONIC
	uint32_t duration_ms;
};

static uint32_t mono_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000u);
}

// 每种动画一条单调缓动曲线: 掉落加速 (重力), 落回减速, 淡变线性,
// 最大化/还原缩放使用类似 Windows 的平滑 ease-in-out
static float anim_ease(enum anim_kind kind, float t) {
	switch (kind) {
	case ANIM_FALL_OUT:
		return t * t; // 加速下落
	case ANIM_FALL_IN:
		t = 1.0f - t; // 减速落地
		return 1.0f - t * t * t;
	case ANIM_GEOM:
		return t * t * (3.0f - 2.0f * t); // smoothstep
	case ANIM_FADE_IN:
	case ANIM_FADE_OUT:
	default:
		return t;
	}
}

// 最大化/还原缩放的时长. CONFIG_ANIM_MAXIMIZE_MS 是基准 - 它自己的独立旋钮,
// 特意不绑定最小化掉落时间 (CONFIG_ANIM_FALL_MS). 跨度很大的缩放
// (小窗口铺满全屏) 会获得有界的额外时间 (跨度/12, 最多翻倍),
// 使每帧步进在 60Hz 下仍平滑; 增量足够小, 旋钮仍直接、可预测地控制缩放手感.
static uint32_t anim_geom_duration(const struct wlr_box *from,
		const struct wlr_box *to) {
	int ex = abs(to->x - from->x) + abs(to->width - from->width);
	int ey = abs(to->y - from->y) + abs(to->height - from->height);
	int span = ex > ey ? ex : ey;
	uint32_t dur = CONFIG_ANIM_MAXIMIZE_MS + (uint32_t)(span / 12u);
	uint32_t cap = CONFIG_ANIM_MAXIMIZE_MS * 2u;
	if (dur > cap) {
		dur = cap;
	}
	return dur;
}

// 强制扫描条带触及的输出重绘
static void anim_schedule_frames(struct server *server,
		const struct wlr_box *sweep) {
	struct wlr_output_layout_output *lo;
	wl_list_for_each(lo, &server->output_layout->outputs, link) {
		struct wlr_box obox;
		wlr_output_layout_get_box(server->output_layout, lo->output, &obox);
		struct wlr_box inter;
		if (wlr_box_intersection(&inter, &obox, sweep)) {
			wlr_output_schedule_frame(lo->output);
		}
	}
}

// 运行中的动画扫过的条带 (from .. to 之间的窗口框, 加上圆角 FBO/阴影边距),
// 采用布局坐标
static void anim_sweep_box(struct toplevel_anim *a, struct wlr_box *sweep) {
	int x0 = a->from_x < a->to_x ? a->from_x : a->to_x;
	int x1 = a->from_x > a->to_x ? a->from_x : a->to_x;
	int y0 = a->from_y < a->to_y ? a->from_y : a->to_y;
	int y1 = a->from_y > a->to_y ? a->from_y : a->to_y;
	int pad = shadow_padding() + 1;
	*sweep = (struct wlr_box){
		.x = x0 - pad,
		.y = y0 - pad,
		.width = (x1 - x0) + a->win_w + 2 * pad,
		.height = (y1 - y0) + a->win_h + 2 * pad,
	};
}

// 最大化/还原缩放触及的区域: from 与 to 两个框的并集 (含边距)
static void geom_sweep_box(struct toplevel_anim *a, struct wlr_box *sweep) {
	int x0 = a->geom_from.x < a->geom_to.x ? a->geom_from.x : a->geom_to.x;
	int x1 = a->geom_from.x > a->geom_to.x ? a->geom_from.x : a->geom_to.x;
	int y0 = a->geom_from.y < a->geom_to.y ? a->geom_from.y : a->geom_to.y;
	int y1 = a->geom_from.y > a->geom_to.y ? a->geom_from.y : a->geom_to.y;
	int w0 = a->geom_from.width > a->geom_to.width ?
		a->geom_from.width : a->geom_to.width;
	int h0 = a->geom_from.height > a->geom_to.height ?
		a->geom_from.height : a->geom_to.height;
	int pad = shadow_padding() + 1;
	*sweep = (struct wlr_box){
		.x = x0 - pad,
		.y = y0 - pad,
		.width = (x1 - x0) + w0 + 2 * pad,
		.height = (y1 - y0) + h0 + 2 * pad,
	};
}

// 在 geom_from (p=0) 与 geom_to (p=1) 之间插值窗口形态框到 tl->morph_*;
// 调用方 (最大化/还原缩放) 随后把场景树锚定在该框原点, 并把圆角 FBO 缩放进去
// (rounded_cache_morph_apply)
static void morph_box_at(struct toplevel_anim *a, float p) {
	struct toplevel *tl = a->tl;
	if (p >= 1.0f) {
		tl->morph_x = a->geom_to.x;
		tl->morph_y = a->geom_to.y;
		tl->morph_w = a->geom_to.width;
		tl->morph_h = a->geom_to.height;
	} else {
		tl->morph_x = a->geom_from.x + (int)lroundf(
			(float)(a->geom_to.x - a->geom_from.x) * p);
		tl->morph_y = a->geom_from.y + (int)lroundf(
			(float)(a->geom_to.y - a->geom_from.y) * p);
		tl->morph_w = a->geom_from.width + (int)lroundf(
			(float)(a->geom_to.width - a->geom_from.width) * p);
		tl->morph_h = a->geom_from.height + (int)lroundf(
			(float)(a->geom_to.height - a->geom_from.height) * p);
	}
}

// 应用进度 p ∈ [0,1] 对应的状态
static void anim_apply(struct toplevel_anim *a, float p) {
	struct toplevel *tl = a->tl;
	switch (a->kind) {
	case ANIM_FALL_OUT:
	case ANIM_FALL_IN: {
		int nx = a->from_x + (int)lroundf(
			(float)(a->to_x - a->from_x) * p);
		int ny = a->from_y + (int)lroundf(
			(float)(a->to_y - a->from_y) * p);
		wlr_scene_node_set_position(&tl->scene_tree->node, nx, ny);
		// 掉落时会围绕窗口自己的 (移动中的) 中心缩小/放大圆角 FBO:
		// 落下 1.0 -> CONFIG_ANIM_FALL_SCALE, 落回 CONFIG_ANIM_FALL_SCALE -> 1.0
		if (a->scale_fall) {
			float to = a->kind == ANIM_FALL_OUT ?
				CONFIG_ANIM_FALL_SCALE : 1.0f;
			float s = a->scale_from + (to - a->scale_from) * p;
			int mw = (int)lroundf(a->win_w * s);
			int mh = (int)lroundf(a->win_h * s);
			if (mw < 1) {
				mw = 1;
			}
			if (mh < 1) {
				mh = 1;
			}
			tl->morph_active = true;
			tl->morph_x = nx + (a->win_w - mw) / 2;
			tl->morph_y = ny + (a->win_h - mh) / 2;
			tl->morph_w = mw;
			tl->morph_h = mh;
			rounded_cache_morph_apply(tl);
		}
		break;
	}
	case ANIM_GEOM:
		// 窗口 (其圆角 FBO 持有目标尺寸的内容) 按缩放显示在插值出的框里:
		// 把场景树移到框原点, 让 rounded.c 把 FBO 缩放进去
		morph_box_at(a, p);
		wlr_scene_node_set_position(&tl->scene_tree->node,
			tl->morph_x, tl->morph_y);
		rounded_cache_morph_apply(tl);
		break;
	case ANIM_FADE_IN:
	case ANIM_FADE_OUT: {
		float op = a->op_from + (a->op_to - a->op_from) * p;
		rounded_window_set_opacity(tl, op);
		a->op_cur = op;
		// 打断最小化掉落的关闭会保留那次掉落的垂直运动 (animate_toplevel_close)
		// 并淡出, 而不是把窗口冻在半空: 运行会落到被打断掉落的屏外目标并在那里消失
		if (a->from_x != a->to_x || a->from_y != a->to_y) {
			wlr_scene_node_set_position(&tl->scene_tree->node,
				a->from_x + (int)lroundf(
					(float)(a->to_x - a->from_x) * p),
				a->from_y + (int)lroundf(
					(float)(a->to_y - a->from_y) * p));
		}
		break;
	}
	default:
		break;
	}
}

static const char *anim_kind_name(enum anim_kind kind) {
	switch (kind) {
	case ANIM_FADE_IN:  return "fade-in";
	case ANIM_FADE_OUT: return "fade-out";
	case ANIM_FALL_OUT: return "minimize-drop";
	case ANIM_FALL_IN:  return "restore-drop";
	case ANIM_GEOM:     return "maximize-zoom";
	default:            return "none";
	}
}

// 拆除正在运行的形态 (最大化/还原缩放, 或最小化掉落/还原落回的收缩):
// 清除形态状态, 把 FBO 节点摆回良定义的布局.
//  - 缩放: 把场景树弹回目标框 (几何), 并按自然 (目标) 尺寸摆放 FBO;
//    若圆角缓存还没持有目标尺寸的内容, 就标记为脏, 客户端提交后立即按自然布局重绘.
//  - 掉落/落回形态: 把 FBO 节点摆回窗口当前框的自然布局,
//    这样接下来运行的任何东西 (保留运动的淡出、取消) 都能正确地从那里绘制窗口.
static void anim_stop(struct toplevel_anim *a); // 定义在下文
static void morph_teardown(struct toplevel_anim *a) {
	struct toplevel *tl = a->tl;
	if (!tl->morph_active) {
		return;
	}
	struct wlr_box sweep;
	if (a->kind == ANIM_GEOM) {
		// 在清除 morph_active 之前弹到目标框, 并按自然 (目标) 尺寸重新摆放 FBO:
		// 最后一个缩放 tick 把节点 dest-size 设成了缩放中途的形态框,
		// 而就绪的缓存不会重新发布, 所以没有这一步窗口会停在中间缩放
		// (例如缩放中途开始的最小化或关闭淡出)
		wlr_scene_node_set_position(&tl->scene_tree->node,
			a->geom_to.x, a->geom_to.y);
		tl->morph_x = a->geom_to.x;
		tl->morph_y = a->geom_to.y;
		tl->morph_w = a->geom_to.width;
		tl->morph_h = a->geom_to.height;
		rounded_cache_morph_apply(tl); // morph_active 仍置位
		tl->morph_active = false;
		if (!rounded_cache_size_ready(tl, a->geom_to.width,
				a->geom_to.height)) {
			rounded_cache_dirty(tl);
		}
		geom_sweep_box(a, &sweep);
	} else {
		// 掉落/落回收缩: 把 FBO 摆回窗口当前位置的自然框, 然后停止缩放
		struct wlr_box box;
		toplevel_box(tl, &box);
		tl->morph_x = box.x;
		tl->morph_y = box.y;
		tl->morph_w = box.width;
		tl->morph_h = box.height;
		rounded_cache_morph_apply(tl);
		tl->morph_active = false;
		anim_sweep_box(a, &sweep);
	}
	anim_schedule_frames(tl->server, &sweep);
	wlr_log(WLR_DEBUG, "animate: %s cancelled for app_id \"%s\"",
		anim_kind_name(a->kind),
		tl->app_id != NULL ? tl->app_id : "?");
}

// 结束当前动画: 精确落定最终状态 (没有运行中的动画后, 节拍看门狗自行解除)
static void anim_finish(struct toplevel_anim *a) {
	struct toplevel *tl = a->tl;
	enum anim_kind kind = a->kind;

	if (a->kind == ANIM_NONE) {
		// 另一个结束者 (frame tick 与节拍看门狗都盯着同一个墙钟截止点)
		// 已经落定了这次运行: 绝不要结束两次 - 也不要重发 close
		return;
	}

	wlr_log(WLR_DEBUG, "animate: %s finished for app_id \"%s\"",
		anim_kind_name(kind),
		tl->app_id != NULL ? tl->app_id : "?");
	anim_apply(a, 1.0f); // 精确端点 (也覆盖 p = 1 的取整)
	switch (kind) {
	case ANIM_FADE_IN:
		rounded_window_set_opacity(tl, 1.0f);
		a->op_cur = 1.0f;
		break;
	case ANIM_FADE_OUT:
		// 完全透明: 现在让客户端真正消失
		rounded_window_set_opacity(tl, 0.0f);
		a->op_cur = 0.0f;
		a->kind = ANIM_NONE;
		if (tl->xdg_toplevel != NULL && tl->xdg_toplevel->base != NULL) {
			wlr_xdg_toplevel_send_close(tl->xdg_toplevel);
		}
		if (tl->closing && tl->scene_tree != NULL) {
			// 窗口已完全不可见, 但客户端还没销毁 surface.
			// 禁用节点: wlroots 的场景命中测试忽略透明度,
			// 一个透明却启用的节点会一直挡住下面的指针 (和悬停), 而关闭请求仍未答复
			wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
		}
		return;
	case ANIM_FALL_OUT:
		// 窗口已离开屏幕: 隐藏它并弹回静止原点, 让所有几何记账看到
		// (现在不可见的) 窗口的原始位置. 收缩形态已完成;
		// FBO 节点停在上个 tick 的位置, 随整棵树一起隐藏, 直到窗口被还原.
		wlr_scene_node_set_position(&tl->scene_tree->node,
			a->rest_x, a->rest_y);
		wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
		tl->morph_active = false;
		break;
	case ANIM_FALL_IN:
		// 落地: 保持静止原点. 最后一个 tick (p = 1) 已把 FBO 缩放回自然框,
		// 所以只需清除形态状态本身
		wlr_scene_node_set_position(&tl->scene_tree->node,
			a->rest_x, a->rest_y);
		tl->morph_active = false;
		break;
	case ANIM_GEOM:
		// 已缩放到目标框: 把几何交还圆角缓存 (自然位置/尺寸等于刚显示的内容)
		tl->morph_active = false;
		break;
	default:
		break;
	}
	a->kind = ANIM_NONE;
	struct wlr_box sweep;
	if (kind == ANIM_GEOM) {
		geom_sweep_box(a, &sweep);
	} else {
		anim_sweep_box(a, &sweep);
	}
	anim_schedule_frames(tl->server, &sweep);
}

// 把一个运行中的动画推进到 now_ms: 插值并应用缓动状态, 运行结束时落定最终状态.
// 每个渲染帧调用一次 (anim_frame_tick), 用它自己的时刻,
// 所以输出实际显示的状态总是属于它自己的 vblank.
static void anim_advance(struct toplevel_anim *a, uint32_t now_ms) {
	struct toplevel *tl = a->tl;
	uint32_t elapsed = now_ms - a->start_ms;

	// ANIM_GEOM 等待阶段: 直到圆角缓存持有目标尺寸的窗口内容
	// (客户端已提交且缓存已重绘) 才做任何事. 只有到那时才开始真正的缩放 -
	// 这样窗口绝不会在浮动布局和目标布局之间跳变.
	// 等待期间画面静止, 所以这里不应用任何东西.
	if (a->kind == ANIM_GEOM && !a->geom_zooming) {
		if (rounded_cache_size_ready(tl, a->geom_to.width,
				a->geom_to.height)) {
			a->geom_zooming = true;
			a->duration_ms = anim_geom_duration(&a->geom_from,
				&a->geom_to);
			a->start_ms = now_ms;
			wlr_log(WLR_DEBUG, "animate: maximize-zoom content ready, "
				"zooming for app_id \"%s\" (%u ms)",
				tl->app_id != NULL ? tl->app_id : "?",
				a->duration_ms);
			anim_apply(a, 0.0f);
			// p = 0 的缩放状态与窗口已显示的内容相同 (其 FBO 缩放进 from 框),
			// 不损坏任何东西: 显式请求第一帧缩放
			struct wlr_box sweep;
			geom_sweep_box(a, &sweep);
			anim_schedule_frames(tl->server, &sweep);
		} else if (elapsed >= a->duration_ms) {
			// 客户端始终没提交目标尺寸: 放弃缩放并瞬时落在目标几何上
			// (morph_teardown 会请求显示落定结果的那一帧)
			morph_teardown(a);
			anim_stop(a);
		}
		return;
	}

	if (elapsed >= a->duration_ms) {
		anim_finish(a);
		return;
	}

	float t = anim_ease(a->kind,
		(float)elapsed / (float)a->duration_ms);
	anim_apply(a, t);
}

// 把所有运行中的动画推进到给定的 CLOCK_MONOTONIC 时刻;
// 由每个输出的 frame 处理器在场景渲染前调用 (output.c 的 monitor_frame)
void anim_frame_tick(struct server *server, uint32_t now_ms) {
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		struct toplevel_anim *a = tl->anim;
		if (a == NULL || a->kind == ANIM_NONE) {
			continue;
		}
		anim_advance(a, now_ms);
	}
}

// 全局节拍看门狗, 仅在有至少一个动画运行时臂置. 可见动画期间每次状态变化
// 都会损坏场景, 从而让输出自行每个 vblank 渲染; 看门狗只填补 damage 无法覆盖的空隙:
// 窗口完全移出输出 (落下尾部、落回头部), 或状态停在某个取整值上不产生 damage 而停止帧流.
// 当没有帧渲染时, 它也执行墙钟超时 (客户端始终不提交的最大化缩放等待).
static int anim_watchdog(void *data) {
	struct server *server = data;
	bool active = false;
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		struct toplevel_anim *a = tl->anim;
		if (a == NULL || a->kind == ANIM_NONE) {
			continue;
		}
		active = true;
		if (a->kind == ANIM_GEOM && !a->geom_zooming) {
			// 纯等待: 场景静止, 所以这里不请求帧;
			// 就绪检查在由客户端提交驱动的 frame tick 里进行.
			// 只有超时需要墙钟.
			if (!rounded_cache_size_ready(tl, a->geom_to.width,
					a->geom_to.height) &&
					mono_ms() - a->start_ms >= a->duration_ms) {
				morph_teardown(a);
				anim_stop(a);
			}
			continue;
		}
		if (mono_ms() - a->start_ms >= a->duration_ms) {
			// 墙钟兜底: 即使没有帧推进它 (如动画中途输出消失), 运行也已结束
			anim_finish(a);
			continue;
		}
		// 让窗口所在输出持续出帧 (被禁用的输出会忽略请求)
		struct wlr_output *output = toplevel_output(tl->server, tl);
		if (output != NULL && output->enabled) {
			wlr_output_schedule_frame(output);
		}
	}
	if (active) {
		wl_event_source_timer_update(server->anim_timer,
			ANIM_WATCHDOG_MS);
	} else {
		wl_event_source_timer_update(server->anim_timer, 0); // 空闲
	}
	return 0;
}

// 臂置节拍看门狗 (首次使用时惰性创建); 每次开始运行时调用.
// 分配失败时可见动画仍通过自身场景 damage 推进 - 只有屏外阶段和超时失去兜底.
static void anim_watchdog_arm(struct server *server) {
	if (server->anim_timer == NULL) {
		server->anim_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server->display),
			anim_watchdog, server);
	}
	if (server->anim_timer == NULL) {
		return;
	}
	wl_event_source_timer_update(server->anim_timer, ANIM_WATCHDOG_MS);
}

// 窗口的场景树消失 (xdg surface 销毁): 停止动画并丢弃每窗口状态.
// 此时 toplevel 仍存活 - wlroots 先销毁场景树, 之后合成器自己的
// xdg-surface destroy 处理器才释放它 (见 toplevel.c).
static void anim_tree_destroy(struct wl_listener *listener, void *data) {
	struct toplevel_anim *a = wl_container_of(listener, a, tree_destroy);
	(void)data;
	a->tl->anim = NULL;
	wl_list_remove(&a->tree_destroy.link);
	free(a);
	// 看门狗自己扫描 toplevel 链表, 没有运行中的动画后自行解除; 无需显式清理定时器
}

static struct toplevel_anim *anim_get(struct toplevel *tl) {
	if (tl->anim != NULL) {
		return tl->anim;
	}
	if (tl->scene_tree == NULL) {
		return NULL;
	}
	struct toplevel_anim *a = calloc(1, sizeof(*a));
	if (a == NULL) {
		return NULL;
	}
	a->tl = tl;
	a->kind = ANIM_NONE;
	a->tree_destroy.notify = anim_tree_destroy;
	wl_signal_add(&tl->scene_tree->node.events.destroy, &a->tree_destroy);
	tl->anim = a;
	return a;
}

// 停止运行中的动画但不碰场景状态 (节拍看门狗发现没有运行中的动画后自行解除)
static void anim_stop(struct toplevel_anim *a) {
	a->kind = ANIM_NONE;
}

// 开始一个持续 duration_ms 的 kind 运行; 初始状态必须由调用方紧接着应用
// (起始位置/起始透明度)
static void anim_begin(struct toplevel_anim *a, enum anim_kind kind,
		uint32_t duration_ms) {
	// 另一个动画占着窗口; 如果那是最大化/还原缩放, 先停止缩放并把窗口弹回目标
	morph_teardown(a);
	anim_stop(a);
	a->kind = kind;
	a->duration_ms = duration_ms;
	a->start_ms = mono_ms();
	wlr_log(WLR_DEBUG, "animate: %s started for app_id \"%s\" (%u ms)",
		anim_kind_name(kind),
		a->tl->app_id != NULL ? a->tl->app_id : "?", duration_ms);
	anim_watchdog_arm(a->tl->server);
}

bool animate_toplevel_minimize(struct server *server, struct toplevel *tl) {
	if (!CONFIG_ANIM_ENABLE) {
		return false;
	}
	struct toplevel_anim *a = anim_get(tl);
	if (a == NULL) {
		return false;
	}
	if (tl->scene_tree == NULL ||
			!tl->scene_tree->node.enabled) {
		// 已经隐藏: 没什么可动画的
		return true;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);
	if (box.width <= 0 || box.height <= 0) {
		return false; // 没有可用几何: 调用方瞬时隐藏
	}
	struct wlr_output *output = toplevel_output(server, tl);
	if (output == NULL) {
		return false;
	}
	struct wlr_box obox;
	wlr_output_layout_get_box(server->output_layout, output, &obox);

	// 落到窗口完全离开输出下边缘为止: 其 FBO 顶边在内容顶边上方
	// shadow_padding() 处, 所以内容顶边到 bottom + shadow (再加 1 像素安全量)
	// 时不会露出任何边框/阴影细条. 掉落恰好在窗口离开屏幕时结束 -
	// 不再有不可见的尾部行程.
	int fall_to = obox.y + obox.height + shadow_padding() + 1;
	if (fall_to <= box.y) {
		// 已经在底部之下: 立即隐藏
		wlr_scene_node_set_position(&tl->scene_tree->node, box.x, box.y);
		wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
		return true;
	}

	// 记住静止原点. 落回过程中最小化必须保留那次运行的目标:
	// 还原已消费 has_rest, 而 box 是下落窗口的半空位置, 不是它该待的地方
	if (a->kind == ANIM_FALL_IN) {
		a->has_rest = true;
		a->rest_x = a->to_x;
		a->rest_y = a->to_y;
	} else {
		a->has_rest = true;
		a->rest_x = box.x;
		a->rest_y = box.y;
	}

	anim_begin(a, ANIM_FALL_OUT, CONFIG_ANIM_FALL_MS);
	a->from_x = box.x;
	a->from_y = box.y;
	a->to_x = box.x;
	a->to_y = fall_to;
	a->win_w = box.width;
	a->win_h = box.height;

	// 落下时围绕自己的 (移动中的) 中心缩小窗口 - 到窗口离开屏幕时,
	// 圆角 FBO 从 1.0 缩放到 CONFIG_ANIM_FALL_SCALE.
	// 没有可缩放 FBO 时就是纯掉落.
	a->scale_fall = CONFIG_ANIM_FALL_SCALE < 1.0f &&
		rounded_morph_supported(tl);
	if (a->scale_fall) {
		a->scale_from = 1.0f;
		tl->morph_active = true;
		tl->morph_x = box.x;
		tl->morph_y = box.y;
		tl->morph_w = box.width;
		tl->morph_h = box.height;
	} else {
		tl->morph_active = false;
	}

	// 完全不透明、节点启用, 然后让它落下. 焦点已移到下一个窗口并被提升到它上面
	// (见 set_minimized), 所以把下落的窗口放回 toplevel 图层顶部:
	// 它必须在下落时留在自己的位置, 而不是滑到接替焦点的窗口后面.
	rounded_window_set_opacity(tl, 1.0f);
	wlr_scene_node_set_enabled(&tl->scene_tree->node, true);
	wlr_scene_node_raise_to_top(&tl->scene_tree->node);
	wlr_scene_node_set_position(&tl->scene_tree->node, box.x, box.y);
	struct wlr_box sweep;
	anim_sweep_box(a, &sweep);
	anim_schedule_frames(server, &sweep);
	return true;
}

bool animate_toplevel_restore(struct server *server, struct toplevel *tl) {
	if (!CONFIG_ANIM_ENABLE) {
		return false;
	}
	struct toplevel_anim *a = anim_get(tl);
	if (a == NULL) {
		return false;
	}
	if (tl->scene_tree == NULL) {
		return true;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);
	if (box.width <= 0 || box.height <= 0) {
		return false; // 没有可用几何: 调用方瞬时显示
	}

	// 目标: 最小化时捕获的静止原点; 从未动画过的窗口保持当前位置
	int rest_x = a->has_rest ? a->rest_x : box.x;
	int rest_y = a->has_rest ? a->rest_y : box.y;
	a->has_rest = false;

	// 落回: 从窗口自身顶边上方约自身高度 (+间隙) 处开始, 垂直落回自己的位置.
	// 还原若打断仍在进行的最小化掉落 (窗口正可见地下落), 则从窗口当前位置
	// 平滑反向 - 从任务栏中途还原绝不能把窗口瞬移回位置上方.
	int from_y;
	if (a->kind == ANIM_FALL_OUT) {
		from_y = tl->scene_tree != NULL ?
			tl->scene_tree->node.y : rest_y;
	} else {
		int drop = box.height + CONFIG_ANIM_FALL_GAP;
		from_y = rest_y - drop;
		if (from_y >= rest_y) {
			from_y = rest_y - 1;
		}
	}

	anim_begin(a, ANIM_FALL_IN, CONFIG_ANIM_FALL_MS);
	a->from_x = rest_x;
	a->from_y = from_y;
	a->to_x = rest_x;
	a->to_y = rest_y;
	a->win_w = box.width;
	a->win_h = box.height;

	// 落回时围绕自己的 (移动中的) 中心把窗口放大回全尺寸:
	// CONFIG_ANIM_FALL_SCALE -> 1.0 (仅圆角 FBO).
	// 没有可缩放 FBO 时就是纯落回.
	a->scale_fall = CONFIG_ANIM_FALL_SCALE < 1.0f &&
		rounded_morph_supported(tl);
	if (a->scale_fall) {
		a->scale_from = CONFIG_ANIM_FALL_SCALE;
		int mw = (int)lroundf(box.width * CONFIG_ANIM_FALL_SCALE);
		int mh = (int)lroundf(box.height * CONFIG_ANIM_FALL_SCALE);
		if (mw < 1) {
			mw = 1;
		}
		if (mh < 1) {
			mh = 1;
		}
		tl->morph_active = true;
		tl->morph_x = rest_x + (box.width - mw) / 2;
		tl->morph_y = from_y + (box.height - mh) / 2;
		tl->morph_w = mw;
		tl->morph_h = mh;
		rounded_cache_morph_apply(tl);
	} else {
		tl->morph_active = false;
	}

	// 第一帧就可见, 且已在下落的顶部. 把它提升到其他窗口之上,
	// 这样落回能出现在自己的位置上 (被还原的窗口通常也会被重新聚焦, 从而提升);
	// 没有这一步它会从当前聚焦窗口后面重新出现.
	rounded_window_set_opacity(tl, 1.0f);
	wlr_scene_node_set_enabled(&tl->scene_tree->node, true);
	wlr_scene_node_raise_to_top(&tl->scene_tree->node);
	wlr_scene_node_set_position(&tl->scene_tree->node, rest_x, from_y);
	struct wlr_box sweep;
	anim_sweep_box(a, &sweep);
	anim_schedule_frames(server, &sweep);
	return true;
}

bool animate_toplevel_fade_in(struct server *server, struct toplevel *tl) {
	if (!CONFIG_ANIM_ENABLE) {
		return false;
	}
	struct toplevel_anim *a = anim_get(tl);
	if (a == NULL) {
		return false;
	}
	if (tl->scene_tree == NULL || tl->minimized) {
		// 隐藏的窗口: 没什么可淡入的
		return true;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);

	anim_begin(a, ANIM_FADE_IN, CONFIG_ANIM_FADE_MS);
	a->from_x = a->to_x = box.x;
	a->from_y = a->to_y = box.y;
	a->win_w = box.width;
	a->win_h = box.height;
	a->op_from = 0.0f;
	a->op_to = 1.0f;
	a->op_cur = 0.0f; // 从透明开始: 打断淡入的关闭知道窗口实际在哪里

	// 立即从透明开始, 避免第一帧闪一下; 窗口保持自然大小, 只有透明度淡入
	rounded_window_set_opacity(tl, 0.0f);
	struct wlr_box sweep;
	anim_sweep_box(a, &sweep);
	anim_schedule_frames(server, &sweep);
	return true;
}

// Windows 式最大化/还原缩放. 调用方已向客户端发送目标尺寸, 且不得自行移动场景节点.
// 窗口先在当前 (浮动/最大化) 框里继续显示当前内容, 直到圆角缓存持有目标尺寸的内容,
// 然后在两个框之间平滑缩放 - 最大化从浮动矩形放大, 还原缩回浮动矩形.
// 节点最终停在 `to` 的原点.
bool animate_toplevel_geometry(struct server *server, struct toplevel *tl,
		const struct wlr_box *from, const struct wlr_box *to) {
	if (!CONFIG_ANIM_ENABLE || CONFIG_ANIM_MAXIMIZE_MS <= 0) {
		return false;
	}
	if (tl->skip_geom) {
		// 窗口刚 map 并上报初始状态 (如启动即最大化的应用): 没有用户最大化动作可动画
		tl->skip_geom = false;
		return false;
	}
	if (tl->scene_tree == NULL || !tl->scene_tree->node.enabled ||
			tl->minimized) {
		return false;
	}
	if (from->width <= 0 || from->height <= 0 ||
			to->width <= 0 || to->height <= 0) {
		return false;
	}
	if (from->x == to->x && from->y == to->y &&
			from->width == to->width && from->height == to->height) {
		return false; // 没有可见变化可动画
	}
	struct toplevel_anim *a = anim_get(tl);
	if (a == NULL) {
		return false;
	}
	if (a->kind != ANIM_NONE) {
		return false; // 另一个动画 (掉落/淡变) 正在运行
	}
	if (!rounded_morph_supported(tl)) {
		return false; // 没有可缩放的圆角 FBO 可缩放
	}

	anim_begin(a, ANIM_GEOM, CONFIG_ANIM_MAXIMIZE_WAIT_MS);
	a->geom_from = *from;
	a->geom_to = *to;
	a->geom_zooming = false;

	// 在目标尺寸内容就绪之前保持视觉上不动:
	// FBO 按缩放发布在当前 (from) 框里, 正是窗口已经所在的位置
	tl->morph_active = true;
	tl->morph_x = from->x;
	tl->morph_y = from->y;
	tl->morph_w = from->width;
	tl->morph_h = from->height;
	wlr_scene_node_set_position(&tl->scene_tree->node, from->x, from->y);
	rounded_cache_morph_apply(tl);
	struct wlr_box sweep;
	geom_sweep_box(a, &sweep);
	anim_schedule_frames(server, &sweep);
	return true;
}

// 停止正在进行的最大化/还原缩放, 让窗口落在缩放目标上
// (toplevel.c 在有新的最大化/还原请求替换正在运行的缩放时使用)
void animate_toplevel_abort_geometry(struct toplevel *tl) {
	if (tl->anim == NULL) {
		return;
	}
	morph_teardown(tl->anim);
	anim_stop(tl->anim);
}

bool animate_toplevel_close(struct toplevel *tl) {
	if (!CONFIG_ANIM_ENABLE) {
		return false;
	}
	if (tl->xdg_toplevel == NULL || tl->xdg_toplevel->base == NULL) {
		return false;
	}
	struct toplevel_anim *a = anim_get(tl);
	if (a == NULL) {
		return false;
	}
	if (tl->scene_tree == NULL ||
			!tl->scene_tree->node.enabled) {
		return false; // 隐藏 (最小化) 的窗口: 没有淡出可看
	}
	if (a->kind == ANIM_FADE_OUT) {
		return true; // 淡出关闭已在运行
	}

	bool interrupting_fall = a->kind == ANIM_FALL_OUT;
	bool interrupting_fade_in = a->kind == ANIM_FADE_IN;
	// 这次关闭打断的最小化掉落继续下落: 捕获它的屏外目标,
	// 让淡出继续该运动 (anim_apply) 而不是把窗口冻在半空
	int fall_target_y = interrupting_fall ? a->to_y : 0;

	struct wlr_box box;
	toplevel_box(tl, &box);

	anim_begin(a, ANIM_FADE_OUT, CONFIG_ANIM_FADE_MS);
	a->from_x = a->to_x = box.x;
	a->from_y = box.y;
	a->to_y = interrupting_fall ? fall_target_y : box.y;
	a->win_w = box.width;
	a->win_h = box.height;
	// 从上一次运行留下的透明度继续: 刚 map 就关闭绝不能先把窗口闪回 100%
	a->op_from = interrupting_fade_in ? a->op_cur : 1.0f;
	a->op_to = 0.0f;
	a->op_cur = a->op_from;

	// 从完全不透明 (或被打断淡入的当前水平) 开始; 窗口保持自然大小,
	// 只有透明度淡出, 结束时发送 xdg close
	rounded_window_set_opacity(tl, a->op_from);
	struct wlr_box sweep;
	anim_sweep_box(a, &sweep);
	anim_schedule_frames(tl->server, &sweep);
	return true;
}

// 窗口在动画结束前 unmaps (客户端自己隐藏/关闭了它): 停止动画并让窗口处于干净状态
void animate_toplevel_cancel(struct toplevel *tl) {
	if (tl->anim == NULL) {
		return;
	}
	struct toplevel_anim *a = tl->anim;
	switch (a->kind) {
	case ANIM_FALL_OUT:
		// 完成最小化: 在静止原点隐藏
		wlr_scene_node_set_position(&tl->scene_tree->node,
			a->rest_x, a->rest_y);
		wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
		break;
	case ANIM_FALL_IN:
		// 完成还原: 落在静止原点
		wlr_scene_node_set_position(&tl->scene_tree->node,
			a->rest_x, a->rest_y);
		break;
	case ANIM_FADE_OUT:
	case ANIM_FADE_IN:
	case ANIM_GEOM:
	default:
		break;
	}
	// 正在运行的最大化/还原缩放拆除到目标框
	morph_teardown(a);
	wlr_log(WLR_DEBUG, "animate: %s cancelled for app_id \"%s\"",
		anim_kind_name(a->kind),
		tl->app_id != NULL ? tl->app_id : "?");
	rounded_window_set_opacity(tl, 1.0f);
	anim_stop(a);
}
