// animate.c - 窗口动画
//
//   创建 (map)  -> 淡入  (透明度 0 -> 1)
//   关闭         -> 淡出 (透明度 1 -> 0, 然后发送 xdg close); 请求关闭的瞬间
//                   窗口就变成惰性 (closing 标志), 完全透明后禁用其场景节点,
//                   这样不理会关闭请求的客户端也不会留下不可见却阻塞输入的窗口.
//                   关闭一个仍在落下 (最小化) 的窗口会保留掉落的同时淡出;
//                   刚 map 就关闭则从当前透明度开始淡出, 不会闪回.
//   最小化       -> macOS genie 形变: 窗口内容贴到网格上, 朝状态栏图标位置
//                   扭成梯形/漏斗 (图标位置由 config.h 的 TASKBAR 常量推算,
//                   状态栏未运行也照常推算), 完成后隐藏场景节点并弹回静止原点
//   还原         -> 从任务栏外侧的目标框反向形变放大回静止框
//   最大化/全屏  -> 同样用 genie 形变在旧框与目标框之间过渡
//
// 形变渲染在 rounded.c 的一张离屏网格 buffer 上 (animate.c 只负责网格顶点);
// 淡变作用在可见窗口 buffer 上 (经 rounded_window_set_opacity()),
// 所以圆角副本、边框和阴影一起淡变.
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

// macOS genie 形变网格分段数 (CONFIG_ANIM_GENIE_ROWS/COLS 夹到有意义的范围:
// 顶点索引用 uint16, 且过密的网格只是白白浪费顶点)
#if CONFIG_ANIM_GENIE_ROWS < 2
#define ANIM_WARP_ROWS 2
#elif CONFIG_ANIM_GENIE_ROWS > 512
#define ANIM_WARP_ROWS 512
#else
#define ANIM_WARP_ROWS CONFIG_ANIM_GENIE_ROWS
#endif
#if CONFIG_ANIM_GENIE_COLS < 1
#define ANIM_WARP_COLS 1
#elif CONFIG_ANIM_GENIE_COLS > 64
#define ANIM_WARP_COLS 64
#else
#define ANIM_WARP_COLS CONFIG_ANIM_GENIE_COLS
#endif

// 一个窗口同一时刻只能运行一种动画
enum anim_kind {
	ANIM_NONE = 0,
	ANIM_FADE_IN,   // map: 透明度 0 -> 1, 不移动
	ANIM_FADE_OUT,  // close: 透明度 1 -> 0, 然后发送 close
	ANIM_GEOM,      // maximize/restore/fullscreen: 在两个框之间 genie 形变
	ANIM_ICON_OUT,  // minimize: 朝状态栏图标扭成漏斗 (genie)
	ANIM_ICON_IN,   // restore: 从任务栏外侧反向形变回原位
};

struct toplevel_anim {
	struct toplevel *tl;

	// 动画中途窗口的场景树销毁 (窗口关闭): 释放状态, 避免再触碰已释放的 toplevel
	struct wl_listener tree_destroy;

	enum anim_kind kind;

	// 静止原点: 窗口在屏幕上该待的地方. 最小化开始时捕获,
	// 还原据此把窗口放大回原位.
	bool has_rest;
	int rest_x, rest_y;

	// 当前运行: 缓动位置/透明度插值端点
	int from_x, from_y; // 动画起始位置
	int to_x, to_y;     // 动画结束位置
	float op_from, op_to; // 透明度范围 (仅淡变)
	float op_cur;         // 当前应用的透明度 (淡变): 让打断的运行从上一次
	                       // 实际停留的位置开始 (map 淡入途中关闭)
	int win_w, win_h;   // 窗口尺寸 (扫描区域 damage)

	// ANIM_GEOM (最大化/还原/全屏 genie 形变): 形变起止的两个窗口框.
	// 缩放从等待阶段开始 - 只有客户端提交了目标尺寸且圆角缓存按该尺寸重绘后,
	// 目标尺寸的内容快照才存在 - 所以绝不会在两种内容布局之间跳变.
	struct wlr_box geom_from, geom_to;
	bool geom_zooming;  // false = 等待目标尺寸的 FBO

	// macOS genie 形变 (CONFIG_ANIM_GENIE). 激活时窗口自己的场景树被禁用,
	// 画面由 rounded.c 的一张网格形变 buffer 节点显示 - 窗口内容被贴到一张细分
	// 网格上, 顶点按梯形/漏斗形变 (底部两角先到目标位, 顶部随后跟上).
	struct rounded_warp *warp;

	uint32_t start_ms;  // 运行开始时的 CLOCK_MONOTONIC
	uint32_t duration_ms;
};

static uint32_t mono_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000u);
}

// 每种动画一条单调缓动曲线: 淡变线性, 形变使用平滑的 ease-in-out (smoothstep)
static float anim_ease(enum anim_kind kind, float t) {
	switch (kind) {
	case ANIM_GEOM:
	case ANIM_ICON_OUT:
	case ANIM_ICON_IN:
		return t * t * (3.0f - 2.0f * t); // smoothstep
	case ANIM_FADE_IN:
	case ANIM_FADE_OUT:
	default:
		return t;
	}
}

// 每个动画自己的时长. 最大化/还原的基准是 CONFIG_ANIM_MAXIMIZE_MS - 它自己的独立旋钮.
// 跨度很大的缩放
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

// --- macOS genie 形变 (CONFIG_ANIM_GENIE) ---
//
// 把窗口内容贴到一张细分网格上, 顶点按梯形/漏斗形变 (底部两角先被拉到目标位,
// 顶部随后跟上), 由 rounded.c 用一次 draw call 渲染成一张离屏 buffer 显示.
// 网格的左右边缘是连续斜线, 所以没有横条切片法在宽高变化处的阶梯与接缝.
// 激活时窗口自己的场景树被禁用; 动画结束销毁网格 buffer 并恢复窗口树.

// 某一行 (归一化 s: 0=窗口顶边, 1=底边) 在全局进度 p 下的局部进度.
// lead_top 决定哪条边先动: false 时底部 (s=1) 先动, true 时顶部 (s=0) 先动 -
// 先动的边没有延迟, 另一条边延迟 lag. 最小化时底部先动 (目标在下方),
// 还原时顶部先动 (目标在上方), 于是还原正好是最小化的反向.
static double anim_warp_q(float p, double s, double lag, bool lead_top) {
	if (lag <= 0.0) {
		return p;
	}
	if (lag >= 1.0) {
		lag = 0.999;
	}
	// d = 距"先动边"的归一化距离: 0 = 先动, 1 = 最后动
	double d = lead_top ? s : (1.0 - s);
	double q = ((double)p - d * lag) / (1.0 - lag);
	if (q < 0.0) {
		q = 0.0;
	} else if (q > 1.0) {
		q = 1.0;
	}
	return q;
}

// 按进度 p 生成一帧的三角形网格并交给 rounded.c 重绘:
// 顶点 (x,y) 是布局坐标, (u,v) 是窗口内容纹理的归一化坐标.
static void anim_warp_apply(struct toplevel_anim *a, float p) {
	if (a->warp == NULL) {
		return;
	}
	const struct wlr_box *from = &a->geom_from;
	const struct wlr_box *to = &a->geom_to;
	if (from->width <= 0 || from->height <= 0 ||
			to->width <= 0 || to->height <= 0) {
		return;
	}
	if (p < 0.0f) {
		p = 0.0f;
	} else if (p > 1.0f) {
		p = 1.0f;
	}
	double lag = CONFIG_ANIM_GENIE_LAG;
	// 先动边 = 离目标更近的边: 最小化 (目标在下方) 底边先被拉走,
	// 还原 (目标在上方) 顶边先展开 - 两个方向观感对称, 不会出现
	// 还原只是一坨从小放大的感觉.
	bool lead_top = (to->y + to->height * 0.5) <
		(from->y + from->height * 0.5);
	double fcx = from->x + from->width * 0.5;
	double tcx = to->x + to->width * 0.5;

	static float verts[(ANIM_WARP_ROWS + 1) * (ANIM_WARP_COLS + 1) * 4];
	static uint16_t indices[ANIM_WARP_ROWS * ANIM_WARP_COLS * 6];
	int nv = 0;
	double minx = 0.0, miny = 0.0, maxx = 0.0, maxy = 0.0;
	for (int i = 0; i <= ANIM_WARP_ROWS; i++) {
		double s = (double)i / (double)ANIM_WARP_ROWS;
		double q = anim_warp_q(p, s, lag, lead_top);
		// 水平: 底部先收窄并滑到目标中心, 顶部最后才收
		double cx = fcx + (tcx - fcx) * q;
		double w = from->width + (to->width - from->width) * q;
		// 垂直: 每一行各自向目标行插值, 底部行先到
		double fy = from->y + s * from->height;
		double ty = to->y + s * to->height;
		double y = fy + (ty - fy) * q;
		for (int j = 0; j <= ANIM_WARP_COLS; j++) {
			double u = (double)j / (double)ANIM_WARP_COLS;
			double x = cx + (u - 0.5) * w;
			verts[nv * 4 + 0] = (float)x;
			verts[nv * 4 + 1] = (float)y;
			verts[nv * 4 + 2] = (float)u;
			verts[nv * 4 + 3] = (float)s;
			if (nv == 0) {
				minx = maxx = x;
				miny = maxy = y;
			} else {
				if (x < minx) { minx = x; }
				if (x > maxx) { maxx = x; }
				if (y < miny) { miny = y; }
				if (y > maxy) { maxy = y; }
			}
			nv++;
		}
	}
	int ni = 0;
	for (int i = 0; i < ANIM_WARP_ROWS; i++) {
		for (int j = 0; j < ANIM_WARP_COLS; j++) {
			uint16_t v00 = (uint16_t)(i * (ANIM_WARP_COLS + 1) + j);
			uint16_t v10 = (uint16_t)((i + 1) * (ANIM_WARP_COLS + 1) + j);
			uint16_t v01 = (uint16_t)(i * (ANIM_WARP_COLS + 1) + j + 1);
			uint16_t v11 = (uint16_t)((i + 1) * (ANIM_WARP_COLS + 1) + j + 1);
			indices[ni++] = v00;
			indices[ni++] = v10;
			indices[ni++] = v01;
			indices[ni++] = v01;
			indices[ni++] = v10;
			indices[ni++] = v11;
		}
	}
	struct wlr_box bbox = {
		.x = (int)floor(minx),
		.y = (int)floor(miny),
		.width = (int)ceil(maxx) - (int)floor(minx),
		.height = (int)ceil(maxy) - (int)floor(miny),
	};
	rounded_warp_update(a->warp, verts, nv, indices, ni, &bbox);
}

// 结束形变: 销毁网格 buffer 节点, 解锁快照. restore_tree 为真时把窗口树重新启用
// (还原/最大化结束要重新显示窗口; 窗口正在销毁时传 false, 不碰它).
static void anim_warp_end(struct toplevel_anim *a, bool restore_tree) {
	if (a->warp == NULL) {
		return;
	}
	rounded_warp_end(a->warp);
	a->warp = NULL;
	struct toplevel *tl = a->tl;
	if (!restore_tree || tl->scene_tree == NULL) {
		return;
	}
	if (a->kind == ANIM_GEOM) {
		// 最大化/还原形变期间场景树一直停在 from 框 (网格负责视觉);
		// 结束时要落到 to 框. 圆角节点一直是相对树的自然摆放 (rounded_publish),
		// 所以不需额外复位.
		wlr_scene_node_set_position(&tl->scene_tree->node,
			a->geom_to.x, a->geom_to.y);
	}
	if (a->kind == ANIM_ICON_IN || a->kind == ANIM_GEOM) {
		// 恢复自然透明度后重新显示窗口 (最大化的等待阶段曾用它隐藏窗口)
		rounded_window_set_opacity(tl, 1.0f);
		wlr_scene_node_set_enabled(&tl->scene_tree->node, true);
	}
}

// 开始形变: 快照窗口当前 FBO 并建立网格 buffer. 返回 false 时调用方退回瞬时行为.
// 网格顶点永远落在 geom_from/geom_to 的并集里, 所以用它作为离屏 buffer 的包围盒.
static bool anim_warp_begin(struct toplevel_anim *a) {
	if (!CONFIG_ANIM_GENIE) {
		return false;
	}
	if (a->warp != NULL) {
		return true;
	}
	struct toplevel *tl = a->tl;
	if (tl->scene_tree == NULL) {
		return false;
	}
	const struct wlr_box *from = &a->geom_from;
	const struct wlr_box *to = &a->geom_to;
	int x0 = from->x < to->x ? from->x : to->x;
	int y0 = from->y < to->y ? from->y : to->y;
	int x1 = (from->x + from->width) > (to->x + to->width) ?
		(from->x + from->width) : (to->x + to->width);
	int y1 = (from->y + from->height) > (to->y + to->height) ?
		(from->y + from->height) : (to->y + to->height);
	struct wlr_box bounds = { x0, y0, x1 - x0, y1 - y0 };
	a->warp = rounded_warp_begin(tl, &bounds);
	return a->warp != NULL;
}

// 应用进度 p ∈ [0,1] 对应的状态
static void anim_apply(struct toplevel_anim *a, float p) {
	struct toplevel *tl = a->tl;
	switch (a->kind) {
	case ANIM_GEOM:
	case ANIM_ICON_OUT:
	case ANIM_ICON_IN:
		// genie 形变: 网格按梯形/漏斗形变, 不做整体矩形缩放
		anim_warp_apply(a, p);
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
	case ANIM_GEOM:     return "maximize-zoom";
	case ANIM_ICON_OUT: return "taskbar-shrink";
	case ANIM_ICON_IN:  return "taskbar-grow";
	default:            return "none";
	}
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
	// 结束 genie 形变: 探销网格 buffer 并恢复窗口树 (还原/最大化结束时窗口重新可见)
	anim_warp_end(a, true);
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
	case ANIM_GEOM:
		// 已缩放到目标框: 窗口树已由 anim_warp_end 重新启用并落在目标几何
		break;
	case ANIM_ICON_OUT:
		// 已缩到图标: 隐藏并弹回静止原点
		wlr_scene_node_set_position(&tl->scene_tree->node,
			a->rest_x, a->rest_y);
		wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
		break;
	case ANIM_ICON_IN:
		// 已放大回静止框: 保持静止原点
		wlr_scene_node_set_position(&tl->scene_tree->node,
			a->rest_x, a->rest_y);
		break;
	default:
		break;
	}
	a->kind = ANIM_NONE;
	struct wlr_box sweep;
	if (kind == ANIM_GEOM || kind == ANIM_ICON_OUT ||
			kind == ANIM_ICON_IN) {
		geom_sweep_box(a, &sweep);
	} else {
		anim_sweep_box(a, &sweep);
	}
	anim_schedule_frames(tl->server, &sweep);
}

static void anim_stop(struct toplevel_anim *a); // 定义在下文

// 把一个运行中的动画推进到 now_ms: 插值并应用缓动状态, 运行结束时落定最终状态.
// 每个渲染帧调用一次 (anim_frame_tick), 用它自己的时刻,
// 所以输出实际显示的状态总是属于它自己的 vblank.
static void anim_advance(struct toplevel_anim *a, uint32_t now_ms) {
	struct toplevel *tl = a->tl;
	uint32_t elapsed = now_ms - a->start_ms;

	// ANIM_GEOM 等待阶段: 网格已接管并以 p=0 显示旧内容 (与窗口一致).
	// 直到客户端确实按目标尺寸重排了内容 (toplevel_content_at_size) 才重拍快照
	// 并开始缩放 - 这样窗口绝不会在浮动布局和目标布局之间跳变. 圆角 FBO 现在
	// 按合成器的显示框分配, 客户端重排前就已经是目标尺寸, 所以只看 FBO 尺寸不够.
	if (a->kind == ANIM_GEOM && !a->geom_zooming) {
		if (rounded_cache_size_ready(tl, a->geom_to.width,
				a->geom_to.height) &&
				toplevel_content_at_size(tl, a->geom_to.width,
				a->geom_to.height)) {
			a->geom_zooming = true;
			a->duration_ms = anim_geom_duration(&a->geom_from,
				&a->geom_to);
			a->start_ms = now_ms;
			wlr_log(WLR_DEBUG, "animate: maximize-zoom content ready, "
				"zooming for app_id \"%s\" (%u ms)",
				tl->app_id != NULL ? tl->app_id : "?",
				a->duration_ms);
			// 目标尺寸内容已就绪: 换掉网格的源快照, 从 from 框开始缩放.
			// 窗口树保持启用但透明 (等待阶段已设), 命中测试继续走原始内容.
			rounded_warp_resnapshot(a->warp);
			rounded_window_set_opacity(tl, 0.0f);
			anim_apply(a, 0.0f);
			struct wlr_box sweep;
			geom_sweep_box(a, &sweep);
			anim_schedule_frames(tl->server, &sweep);
		} else if (elapsed >= a->duration_ms) {
			// 客户端始终没提交目标尺寸: 放弃缩放并瞬时落在目标几何上
			wlr_log(WLR_DEBUG, "animate: %s content %dx%d never arrived, "
				"giving up for app_id \"%s\"",
				anim_kind_name(a->kind), a->geom_to.width,
				a->geom_to.height,
				tl->app_id != NULL ? tl->app_id : "?");
			anim_stop(a);
			rounded_cache_dirty(tl);
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
			if (!(rounded_cache_size_ready(tl, a->geom_to.width,
					a->geom_to.height) &&
					toplevel_content_at_size(tl, a->geom_to.width,
					a->geom_to.height)) &&
					mono_ms() - a->start_ms >= a->duration_ms) {
				wlr_log(WLR_DEBUG, "animate: watchdog: %s content %dx%d "
					"never arrived for app_id \"%s\"",
					anim_kind_name(a->kind), a->geom_to.width,
					a->geom_to.height,
					tl->app_id != NULL ? tl->app_id : "?");
				anim_stop(a);
				rounded_cache_dirty(tl);
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
	// 窗口场景树正在销毁: 探销形变网格, 但不要再碰窗口树
	anim_warp_end(a, false);
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
	// 离开任何 genie 形变: 探销网格 buffer, 还原/最大化时把窗口树重新启用
	anim_warp_end(a, true);
	a->kind = ANIM_NONE;
}

// 开始一个持续 duration_ms 的 kind 运行; 初始状态必须由调用方紧接着应用
// (起始位置/起始透明度)
static void anim_begin(struct toplevel_anim *a, enum anim_kind kind,
		uint32_t duration_ms) {
	// 另一个动画占着窗口: 先结束它 (形变会拆掉网格并把窗口树落到目标几何)
	anim_stop(a);
	a->kind = kind;
	a->duration_ms = duration_ms;
	a->start_ms = mono_ms();
	wlr_log(WLR_DEBUG, "animate: %s started for app_id \"%s\" (%u ms)",
		anim_kind_name(kind),
		a->tl->app_id != NULL ? a->tl->app_id : "?", duration_ms);
	anim_watchdog_arm(a->tl->server);
}

// 用状态栏布局常量 (config.h) 推算最小化/还原动画的目标框 (布局坐标).
// 位置完全来自配置, 不查询/不判断状态栏是否运行:
//   状态栏贴在输出上边或下边 (CONFIG_TASKBAR_AT_TOP), 高度 CONFIG_TASKBAR_HEIGHT,
//   图标间距 CONFIG_TASKBAR_ICON_PITCH, 第一个图标偏移 CONFIG_TASKBAR_ICON_OFFSET.
// x 对齐任务栏图标; y 不进入状态栏 - 贴底栏时目标框底边压在栏顶边之上,
// 贴顶栏时目标框顶边压在栏底边之下. 这样缩小/放大的窗口全程都不低于状态栏.
static bool taskbar_icon_box(struct server *server, struct toplevel *tl,
		struct wlr_box *out) {
	if (!tl->ipc_added) {
		return false; // 对话框/弹窗没有独立图标
	}
	struct wlr_output *output = toplevel_output(server, tl);
	if (output == NULL) {
		return false;
	}
	struct wlr_box obox;
	wlr_output_layout_get_box(server->output_layout, output, &obox);

	// 状态栏顶边: 贴顶就是输出顶边, 贴底则从输出底边往上量一个栏高
	int bar_top = CONFIG_TASKBAR_AT_TOP ? obox.y :
		obox.y + obox.height - CONFIG_TASKBAR_HEIGHT;

	// 图标序号 = 创建顺序中, 该窗口前面有多少个任务栏可见的窗口.
	// 状态栏按 window_added 顺序排列图标, 所以两者一致.
	int index = 0;
	struct toplevel *it;
	wl_list_for_each(it, &server->toplevels, link) {
		if (it == tl) {
			break;
		}
		if (it->ipc_added) {
			index++;
		}
	}

	int icon = CONFIG_TASKBAR_HEIGHT - 8; // 图标尺寸, 见 config.h
	if (icon < 1) {
		icon = 1;
	}
	int x = obox.x + CONFIG_TASKBAR_ICON_OFFSET +
		index * CONFIG_TASKBAR_ICON_PITCH;
	// 目标框贴着状态栏外侧: 贴底栏时底边 = 栏顶边 (y = bar_top - icon),
	// 贴顶栏时顶边 = 栏底边. 起点的最大化框底边也正好是栏顶边,
	// 所以形变插值出的底边全程恒定在栏顶边, 绝不会沉到状态栏下面.
	int y = CONFIG_TASKBAR_AT_TOP
		? bar_top + CONFIG_TASKBAR_HEIGHT
		: bar_top - icon;
	// 夹在屏幕内, 避免图标很多时飞出
	if (x + icon > obox.x + obox.width) {
		x = obox.x + obox.width - icon;
	}
	if (x < obox.x) {
		x = obox.x;
	}
	*out = (struct wlr_box){ x, y, icon, icon };
	return true;
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
	toplevel_frame_box(server, tl, &box);
	if (box.width <= 0 || box.height <= 0) {
		return false; // 没有可用几何: 调用方瞬时隐藏
	}
	// genie 最小化: 朝状态栏图标缩小 (停在状态栏外侧, 不进入状态栏).
	// 图标位置由常量推算,
	// 状态栏没运行/没圆角 FBO 也照常动画 (后者只移动, 不缩放).
	struct wlr_box icon_box = box;
	taskbar_icon_box(server, tl, &icon_box);

	// 记住静止原点 (被打断的还原用其目标, 而不是半空中的位置)
	if (a->kind == ANIM_ICON_IN) {
		a->has_rest = true;
		a->rest_x = a->geom_to.x;
		a->rest_y = a->geom_to.y;
	} else {
		a->has_rest = true;
		a->rest_x = box.x;
		a->rest_y = box.y;
	}
	anim_begin(a, ANIM_ICON_OUT, CONFIG_ANIM_TASKBAR_MS);
	a->geom_from = box;
	a->geom_to = icon_box;
	a->win_w = box.width;
	a->win_h = box.height;
	a->op_from = 1.0f;
	a->op_to = 1.0f;
	a->op_cur = 1.0f;
	// 网格 buffer 接管画面 (底部两角先被拉向目标);
	// 没有可用的 GL/FBO 时退回瞬时隐藏.
	if (!anim_warp_begin(a)) {
		anim_stop(a);
		return false;
	}
	// 画面完全由网格节点显示: 窗口自身内容变透明, 但场景树保持启用 -
	// 禁用树会让动画期间窗口无法被指针命中 (命中测试走原始客户端内容).
	// 提升到顶层, 使命中测试与置顶显示的网格一致.
	rounded_window_set_opacity(tl, 0.0f);
	wlr_scene_node_raise_to_top(&tl->scene_tree->node);
	struct wlr_box sweep;
	geom_sweep_box(a, &sweep);
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
	toplevel_frame_box(server, tl, &box);
	if (box.width <= 0 || box.height <= 0) {
		return false; // 没有可用几何: 调用方瞬时显示
	}

	// 目标: 最小化时捕获的静止原点; 从未动画过的窗口保持当前位置
	int rest_x = a->has_rest ? a->rest_x : box.x;
	int rest_y = a->has_rest ? a->rest_y : box.y;
	a->has_rest = false;
	a->rest_x = rest_x;
	a->rest_y = rest_y;

	// genie 还原: 从任务栏图标外侧放大回静止框. 图标位置由常量推算.
	struct wlr_box icon_box = box;
	taskbar_icon_box(server, tl, &icon_box);
	struct wlr_box rest_box = { rest_x, rest_y, box.width, box.height };
	anim_begin(a, ANIM_ICON_IN, CONFIG_ANIM_TASKBAR_MS);
	a->geom_from = icon_box;
	a->geom_to = rest_box;
	a->win_w = box.width;
	a->win_h = box.height;
	a->op_from = 1.0f;
	a->op_to = 1.0f;
	a->op_cur = 1.0f;
	// 网格从目标位展开回窗口 (顶部最后到位); 没有可用的 GL/FBO 时退回瞬时显示.
	if (!anim_warp_begin(a)) {
		anim_stop(a);
		return false;
	}
	// 画面完全由网格节点显示: 窗口自身内容透明, 但场景树保持启用,
	// 这样动画期间窗口仍可被指针命中; 并提升到顶层使命中与网格一致.
	rounded_window_set_opacity(tl, 0.0f);
	wlr_scene_node_set_enabled(&tl->scene_tree->node, true);
	wlr_scene_node_raise_to_top(&tl->scene_tree->node);
	struct wlr_box sweep;
	geom_sweep_box(a, &sweep);
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
	toplevel_frame_box(server, tl, &box);

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

// 最大化/还原/全屏的 genie 形变. 调用方已向客户端发送目标尺寸,
// 且不得自行移动场景节点 (形变期间场景树停在 from 框).
// 网格先以 from 框显示当前内容, 直到客户端确实按目标尺寸重排了内容
// (toplevel_content_at_size), 再重拍快照并在两个框之间形变 - 最大化从浮动矩形
// 放大, 还原缩回浮动矩形. 结束时场景树落到 `to` 的原点.
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

	anim_begin(a, ANIM_GEOM, CONFIG_ANIM_MAXIMIZE_WAIT_MS);
	a->geom_from = *from;
	a->geom_to = *to;
	a->geom_zooming = false;

	// 场景树锚定在 from 框 (形变期间不动, 由网格负责视觉)
	wlr_scene_node_set_position(&tl->scene_tree->node, from->x, from->y);
	// 网格立即接管: 以 p=0 显示旧内容 (与窗口一致), 等目标尺寸内容就绪
	// (anim_advance) 再重拍快照并开始缩放. 没有可用的 GL/FBO 时退回瞬时跳变.
	if (!anim_warp_begin(a)) {
		anim_stop(a);
		return false;
	}
	// 场景树全程保持启用: 圆角 FBO 的合成依赖它 (wlroots 跳过被禁用的节点),
	// 目标尺寸的内容才能在客户端提交后重绘; 命中测试也依赖它找到窗口.
	// 用 0 透明度隐藏窗口, 画面由网格节点显示. 不会闪目标几何.
	rounded_window_set_opacity(tl, 0.0f);
	anim_warp_apply(a, 0.0f);
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
	anim_stop(tl->anim);
}

// 是否有拥有窗口场景节点的几何动画在运行 (最大化/还原缩放、最小化/还原).
bool animate_toplevel_owns_geometry(struct toplevel *tl) {
	if (tl->anim == NULL) {
		return false;
	}
	switch (tl->anim->kind) {
	case ANIM_GEOM:
	case ANIM_ICON_OUT:
	case ANIM_ICON_IN:
		return true;
	default:
		return false;
	}
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
	if (a->kind == ANIM_ICON_OUT || a->kind == ANIM_ICON_IN) {
		// 图标缩放还在运行: 先落定它 (最小化已淡出 -> 节点隐藏, 还原回到原位),
		// 再按普通关闭处理, 避免淡出与运行中的缩放共用场景节点
		anim_finish(a);
	}
	if (tl->scene_tree == NULL ||
			!tl->scene_tree->node.enabled) {
		return false; // 隐藏 (最小化) 的窗口: 没有淡出可看
	}
	if (a->kind == ANIM_FADE_OUT) {
		return true; // 淡出关闭已在运行
	}

	bool interrupting_fade_in = a->kind == ANIM_FADE_IN;
	struct wlr_box box;
	toplevel_frame_box(tl->server, tl, &box);

	anim_begin(a, ANIM_FADE_OUT, CONFIG_ANIM_FADE_MS);
	a->from_x = a->to_x = box.x;
	a->from_y = a->to_y = box.y;
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
	case ANIM_ICON_OUT:
		// 完成最小化: 在静止原点隐藏
		wlr_scene_node_set_position(&tl->scene_tree->node,
			a->rest_x, a->rest_y);
		wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
		break;
	case ANIM_ICON_IN:
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
	wlr_log(WLR_DEBUG, "animate: %s cancelled for app_id \"%s\"",
		anim_kind_name(a->kind),
		tl->app_id != NULL ? tl->app_id : "?");
	rounded_window_set_opacity(tl, 1.0f);
	anim_stop(a);
}
