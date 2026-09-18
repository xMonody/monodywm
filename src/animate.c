// animate.c - 窗口动画 (仅淡入淡出)
//
//   map   -> 淡入 (0 -> 1)
//   close -> 淡出 (1 -> 0, 之后发送 xdg close); 关闭请求发出后窗口即变惰性,
//            淡完禁用场景节点, 避免不可见却挡住指针
//
// 动画由每个输出的 frame 处理器 (output.c) 推进到该帧自己的 CLOCK_MONOTONIC 时刻,
// 与 vblank 同步; 节拍看门狗只在 damage 无法驱动帧流时兜底.

#include "server.h"

#include <stdlib.h>
#include <time.h>

#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

// 看门狗请求输出帧的最小间隔 (ms)
#define ANIM_WATCHDOG_MS 16

enum anim_kind {
	ANIM_NONE = 0,
	ANIM_FADE_IN,
	ANIM_FADE_OUT,
};

struct toplevel_anim {
	struct toplevel *tl;

	// 场景树销毁时释放状态
	struct wl_listener tree_destroy;

	enum anim_kind kind;

	float op_from, op_to;
	float op_cur; // 当前透明度 (打断的运行从这里继续)

	int win_x, win_y, win_w, win_h; // 窗口框 (damage 扫描用)

	uint32_t start_ms;
	uint32_t duration_ms;
};

static uint32_t mono_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000u);
}

// 让与 sweep 相交的输出重绘
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

// 窗口框加上阴影边距
static void anim_sweep_box(struct toplevel_anim *a, struct wlr_box *sweep) {
	int pad = shadow_padding() + 1;
	*sweep = (struct wlr_box){
		.x = a->win_x - pad,
		.y = a->win_y - pad,
		.width = a->win_w + 2 * pad,
		.height = a->win_h + 2 * pad,
	};
}

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

static void anim_stop(struct toplevel_anim *a);

// 落定动画最终状态
static void anim_finish(struct toplevel_anim *a) {
	struct toplevel *tl = a->tl;
	enum anim_kind kind = a->kind;

	if (kind == ANIM_NONE) {
		return; // frame tick 与看门狗都盯着同一截止点, 不要结束两次
	}
	wlr_log(WLR_DEBUG, "animate: %s finished for app_id \"%s\"",
		anim_kind_name(kind),
		tl->app_id != NULL ? tl->app_id : "?");
	anim_apply(a, 1.0f);
	switch (kind) {
	case ANIM_FADE_IN:
		rounded_window_set_opacity(tl, 1.0f);
		a->op_cur = 1.0f;
		break;
	case ANIM_FADE_OUT:
		rounded_window_set_opacity(tl, 0.0f);
		a->op_cur = 0.0f;
		a->kind = ANIM_NONE;
		if (tl->xdg_toplevel != NULL && tl->xdg_toplevel->base != NULL) {
			wlr_xdg_toplevel_send_close(tl->xdg_toplevel);
		}
		if (tl->closing && tl->scene_tree != NULL) {
			// 完全不可见: 禁用节点 (命中测试忽略透明度), 并让指针焦点落到下面
			wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
			refresh_pointer_focus(tl->server);
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

// 把动画推进到 now_ms, 结束时落定
static void anim_advance(struct toplevel_anim *a, uint32_t now_ms) {
	uint32_t elapsed = now_ms - a->start_ms;
	if (elapsed >= a->duration_ms) {
		anim_finish(a);
		return;
	}
	anim_apply(a, (float)elapsed / (float)a->duration_ms);
}

// 由每个输出的 frame 处理器在场景渲染前调用
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

// 有动画在运行时臂置的节拍看门狗: 填补 damage 驱动不了的帧, 并做墙钟超时
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
			anim_finish(a);
			continue;
		}
		struct wlr_output *output = toplevel_output(tl->server, tl);
		if (output != NULL && output->enabled) {
			wlr_output_schedule_frame(output);
		}
	}
	wl_event_source_timer_update(server->anim_timer,
		active ? ANIM_WATCHDOG_MS : 0);
	return 0;
}

static void anim_watchdog_arm(struct server *server) {
	if (server->anim_timer == NULL) {
		server->anim_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server->display),
			anim_watchdog, server);
	}
	if (server->anim_timer != NULL) {
		wl_event_source_timer_update(server->anim_timer, ANIM_WATCHDOG_MS);
	}
}

// 场景树销毁 (窗口消失): 丢弃每窗口动画状态
static void anim_tree_destroy(struct wl_listener *listener, void *data) {
	struct toplevel_anim *a = wl_container_of(listener, a, tree_destroy);
	(void)data;
	a->tl->anim = NULL;
	wl_list_remove(&a->tree_destroy.link);
	free(a);
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

static void anim_stop(struct toplevel_anim *a) {
	a->kind = ANIM_NONE;
}

// 开始一段动画; 初始状态由调用方紧接着应用
static void anim_begin(struct toplevel_anim *a, enum anim_kind kind,
		uint32_t duration_ms) {
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
		return true; // 隐藏窗口: 无可淡入
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
	a->op_cur = 0.0f;

	rounded_window_set_opacity(tl, 0.0f); // 立即透明, 避免第一帧闪一下
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
	if (tl->scene_tree == NULL || !tl->scene_tree->node.enabled) {
		return false; // 隐藏 (最小化) 的窗口: 没有淡出可看
	}
	if (a->kind == ANIM_FADE_OUT) {
		return true; // 已在淡出
	}

	bool interrupting_fade_in = a->kind == ANIM_FADE_IN;
	struct wlr_box box;
	toplevel_frame_box(tl->server, tl, &box);

	anim_begin(a, ANIM_FADE_OUT, CONFIG_ANIM_FADE_MS);
	a->win_x = box.x;
	a->win_y = box.y;
	a->win_w = box.width;
	a->win_h = box.height;
	// 打断淡入时从当前透明度继续, 避免闪回 100%
	a->op_from = interrupting_fade_in ? a->op_cur : 1.0f;
	a->op_to = 0.0f;
	a->op_cur = a->op_from;

	rounded_window_set_opacity(tl, a->op_from);
	struct wlr_box sweep;
	anim_sweep_box(a, &sweep);
	anim_schedule_frames(tl->server, &sweep);
	return true;
}

// 窗口在动画结束前 unmap: 停止动画, 恢复可见状态
void animate_toplevel_cancel(struct toplevel *tl) {
	if (tl->anim == NULL || tl->anim->kind == ANIM_NONE) {
		return;
	}
	struct toplevel_anim *a = tl->anim;
	wlr_log(WLR_DEBUG, "animate: %s cancelled for app_id \"%s\"",
		anim_kind_name(a->kind),
		tl->app_id != NULL ? tl->app_id : "?");
	rounded_window_set_opacity(tl, 1.0f);
	anim_stop(a);
}
