// util.h - 跨模块共用的小工具

#ifndef MONODYWM_UTIL_H
#define MONODYWM_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <wayland-server-core.h>

// 单调时钟: 事件时间戳 (wl_pointer 的 time_msec) 与节流窗口共用.
static inline uint32_t current_time_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)now.tv_sec * 1000u +
		(uint32_t)(now.tv_nsec / 1000000u);
}

// a - b, 单位纳秒 (两者都来自 CLOCK_MONOTONIC)
static inline int64_t time_diff_ns(const struct timespec *a,
		const struct timespec *b) {
	return (int64_t)(a->tv_sec - b->tv_sec) * 1000000000LL +
		(int64_t)(a->tv_nsec - b->tv_nsec);
}

// wayland 的 wl_list_remove() 会把 link 置空 (prev/next 变 NULL),
// 对同一监听器第二次移除会解引用 NULL.
// 只在监听器仍挂在信号上时才移除它.
static inline void listener_remove_if_attached(struct wl_listener *listener) {
	if (listener->link.prev != NULL) {
		wl_list_remove(&listener->link);
	}
}

// 有界按键集合: 线性查找, 满了就丢弃新项.
// 三处使用同一份逻辑:
//   - keyboard.consumed: 合成器快捷键吞掉的按下键码 (owner = NULL)
//   - ime.forwarded:     已转发给输入法的 (keyboard, keycode)
//   - server.bound_buttons: 合成器吞掉的鼠标按钮 (owner = NULL)
// 用尾元素交换删除, 不保持顺序 (调用方都只关心成员关系).
#define KEY_SET_MAX 16

struct key_slot {
	void *owner; // 区分来源 (如 wlr_keyboard); NULL 表示不区分
	uint32_t code;
};

struct key_set {
	struct key_slot slots[KEY_SET_MAX];
	size_t count;
};

static inline bool key_set_contains(const struct key_set *set,
		const void *owner, uint32_t code) {
	for (size_t i = 0; i < set->count; i++) {
		if (set->slots[i].owner == owner && set->slots[i].code == code) {
			return true;
		}
	}
	return false;
}

static inline void key_set_add(struct key_set *set, void *owner,
		uint32_t code) {
	if (key_set_contains(set, owner, code)) {
		return; // 自动重复/重复按下只占一个槽位
	}
	if (set->count >= KEY_SET_MAX) {
		return; // 记录满: 退回旧行为 (释放可能被转发)
	}
	set->slots[set->count++] = (struct key_slot){
		.owner = owner,
		.code = code,
	};
}

static inline bool key_set_remove(struct key_set *set, const void *owner,
		uint32_t code) {
	for (size_t i = 0; i < set->count; i++) {
		if (set->slots[i].owner == owner && set->slots[i].code == code) {
			set->slots[i] = set->slots[--set->count];
			return true;
		}
	}
	return false;
}

// 清掉某个来源的所有记录 (键盘拔掉时该来源不会再有 release)
static inline void key_set_remove_owner(struct key_set *set,
		const void *owner) {
	size_t i = 0;
	while (i < set->count) {
		if (set->slots[i].owner == owner) {
			set->slots[i] = set->slots[--set->count];
		} else {
			i++;
		}
	}
}

static inline void key_set_clear(struct key_set *set) {
	set->count = 0;
}

#endif // MONODYWM_UTIL_H
