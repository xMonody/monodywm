// animate.c - 窗口动画 (仅淡入淡出)
//
//   创建 (map)  -> 淡入 (透明度 0 -> 1)
//   关闭        -> 淡出 (透明度 1 -> 0, 然后发送 xdg close); 请求关闭的瞬间
//                  窗口就变成惰性 (closing 标志), 完全透明后禁用其场景节点,
//                  这样不理会关闭请求的客户端也不会留下不可见却阻塞输入的窗口.
//                  刚 map 就关闭则从当前透明度开始淡出, 不会闪回.
//
// 淡变作用在可见窗口 buffer 上 (经 rounded_window_set_opacity()),
// 所以圆角副本、边框和阴影一起淡变.
//
// 时间模型: 动画状态不由相位会相对显示刷新漂移的每窗口定时器推进.
// 而是由每个输出的 frame 处理器 (output.c) 在场景渲染前把所有运行中的动画
// 推进到它自己的 CLOCK_MONOTONIC 时刻, 所以每个渲染帧显示的正是它自己 vblank
// 对应的缓动状态 - 不存在更新定时器与 vblank 的节拍错位, 高刷输出会插值出
// 更多状态. 每次状态变化产生的场景 damage 会让输出在动画期间每个 vblank 都渲染.
// 一个全局节拍看门狗 (仅在有动画运行时臂置, 固定 16ms 下限 - 见 ANIM_WATCHDOG_MS)
// 执行墙钟超时兜底.
//
// 所有入口在动画被禁用或无法运行时返回 false (且不改变任何东西),
// 调用方据此退回瞬时、无动画的行为.

#include "server.h"

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
// (见文件头注释). 看门狗只保证 damage 无法驱动帧流时仍能推进到墙钟截止点;
// 16ms 足够短, 任何输出上的动画都不会明显迟结束.
#define ANIM_WATCHDOG_MS 16

// 一个窗口同一时刻只能运行一种动画
enum anim_kind {
	ANIM_NONE = 0,
	ANIM_FADE_IN,   // map: 透明度 0 -> 1
	ANIM_FADE_OUT,  // close: 透明度 1 -> 0, 然后发送 close
};

struct toplevel_anim {
	struct toplevel *tl;

	// 动画中途窗口的场景树销毁 (窗口关闭): 释放状态, 避免再触碰已释放的 toplevel
	struct wl_listener tree_destroy;

	enum anim_kind kind;

	float op_from, op_to; // 透明度范围
	float op_cur;         // 当前应用的透明度: 让打断的运行从上一次实际停留的位置
	                      // 开始 (map 淡入途中关闭)

	// 窗口显示框 (扫描区域 damage)
	int win_x, win_y, win_w, win_h;

	uint32_t start_ms;  // 运行开始时的 CLOCK_MONOTONIC
	uint32_t duration_ms;
};

static uint32_t mono_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000u);
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

// 运行中的动画的窗口框 (加上圆角 FBO/阴影边距), 采用布局坐标
static void anim_sweep_box(struct toplevel_anim *a, struct wlr_box *sweep) {
	int pad = shadow_padding() + 1;
	*sweep = (struct wlr_box){
		.x = a->win_x - pad,
		.y = a->win_y - pad,
		.width = a->win_w + 2 * pad,
		.height = a->win_h + 2 * pad,
	};
}

// 应用进度 p ∈ [0,1] 对应的透明度
static void anim_apply(struct toplevel_anim *a, float p) {
	float op = a->op_from + (a->op_to - a->op_from) * p;
	rounded_window_set_opacity(a->tl, op);
	a->op_cur = op;
}

static const char *anim_kind_name(enum anim_kind kind) {
	switch (kind) {
	case ANIM_FADE_IN:  return "fade-in";
	case ANIM_FADE_OUT: return "fade-out";
	default:            return "none";
	}
}

// 结束当前动画: 精确落定最终状态 (没有运行中的动画后, 节拍看门狗自行解除)
static void anim_finish(struct toplevel_anim *a) {
	struct toplevel *tl = a->tl;
	enum anim_kind kind = a->kind;

	if (kind == ANIM_NONE) {
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
	default:
		break;
	}
	a->kind = ANIM_NONE;
	struct wlr_box sweep;
	anim_sweep_box(a, &sweep);
	anim_schedule_frames(tl->server, &sweep);
}

static void anim_stop(struct toplevel_anim *a); // 定义在下文

// 把一个运行中的动画推进到 now_ms: 插值并应用当前状态, 运行结束时落定最终状态.
// 每个渲染帧调用一次 (anim_frame_tick), 用它自己的时刻,
// 所以输出实际显示的状态总是属于它自己的 vblank.
static void anim_advance(struct toplevel_anim *a, uint32_t now_ms) {
	uint32_t elapsed = now_ms - a->start_ms;
	if (elapsed >= a->duration_ms) {
		anim_finish(a);
		return;
	}
	// 淡变线性
	float p = (float)elapsed / (float)a->duration_ms;
	anim_apply(a, p);
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
// 都会损坏场景, 从而让输出自行每个 vblank 渲染; 看门狗只填补 damage 无法覆盖的空隙
// 并执行墙钟超时 (如动画中途输出消失, 不再有帧推进它).
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
		if (mono_ms() - a->start_ms >= a->duration_ms) {
			// 墙钟兜底: 即使没有帧推进它, 运行也已结束
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
// (起始透明度)
static void anim_begin(struct toplevel_anim *a, enum anim_kind kind,
		uint32_t duration_ms) {
	// 另一个动画占着窗口: 先结束它
	anim_stop(a);
	a->kind = kind;
	a->duration_ms = duration_ms;
	a->start_ms = mono_ms();
	wlr_log(WLR_DEBUG, "animate: %s started for app_id \"%s\" (%u ms)",
		anim_kind_name(kind),
		a->tl->app_id != NULL ? a->tl->app_id : "?", duration_ms);
	anim_watchdog_arm(a->tl->server);
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
	a->win_x = box.x;
	a->win_y = box.y;
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

	bool interrupting_fade_in = a->kind == ANIM_FADE_IN;
	struct wlr_box box;
	toplevel_frame_box(tl->server, tl, &box);

	anim_begin(a, ANIM_FADE_OUT, CONFIG_ANIM_FADE_MS);
	a->win_x = box.x;
	a->win_y = box.y;
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
	wlr_log(WLR_DEBUG, "animate: %s cancelled for app_id \"%s\"",
		anim_kind_name(a->kind),
		tl->app_id != NULL ? tl->app_id : "?");
	rounded_window_set_opacity(tl, 1.0f);
	anim_stop(a);
}
