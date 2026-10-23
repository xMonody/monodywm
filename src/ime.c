// ime.c - 输入法中继 (fcitx5 / ibus, 中文输入)
//
// 把 zwp_input_method_unstable_v2 (输入法, 如 fcitx5) 接到
// zwp_text_input_unstable_v3 (每窗口文本输入), 采用 labwc / dwl 的中继设计
// (dwl PR #235, DreamMaoMao 的补丁):
//
//   - 合成器为每个 seat 跟踪一个"聚焦 surface". 键盘焦点变化时,
//     客户端拥有该聚焦 surface 的所有 text input 收到 enter (其余收到 leave).
//   - 只有既聚焦、客户端又发送了 enable 的 text input 才算"活动".
//     存在这样的 text input 时激活输入法, 否则停用.
//   - 输入法状态 (周围文本/内容类型) 只中继给活动的 text input;
//     IM 的 preedit/commit/delete 字符串也只转发给活动的 text input.
//   - IM 持有键盘抓取期间, 所有真实键盘的按键/修饰符事件都转发给它
//     (抓取期间 input.c 跳过自己正常的按键处理). 来自 IM 自己虚拟键盘
//     (其 passthrough 重注入设备) 的按键绝不转发回抓取, 避免事件成环.
//
// 候选窗 (input popup surface) 放在 overlay 图层, 靠近文本光标
// (应用未提供矩形时用指针位置).

#include "server.h"

#include <stdlib.h>

#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/util/log.h>

// ------------------------------------------------------------------
// 辅助
// ------------------------------------------------------------------

static bool same_client(struct wlr_text_input_v3 *ti,
		struct wlr_surface *surface) {
	return wl_resource_get_client(ti->resource) ==
		wl_resource_get_client(surface->resource);
}

// 这个键盘是输入法自己的 passthrough 虚拟键盘吗?
// 来自它的按键本身就是 IM 的输出, 绝不能送回 IM 抓取
// (否则按键会在合成器与 IM 之间成环).
static bool is_keyboard_emulated_by_input_method(struct wlr_keyboard *keyboard,
		struct wlr_input_method_v2 *input_method) {
	if (keyboard == NULL || input_method == NULL) {
		return false;
	}
	struct wlr_virtual_keyboard_v1 *vk =
		wlr_input_device_get_virtual_keyboard(&keyboard->base);
	return vk != NULL &&
		wl_resource_get_client(vk->resource) ==
		wl_resource_get_client(input_method->resource);
}

// ---- 已转发按键记账: 只有匹配的按下被转发过, 才转发释放
//      (IM 可能在某个键已按下后才抓取键盘) ----

static bool ime_key_forwarded(struct ime *ime,
		struct wlr_keyboard *keyboard, uint32_t keycode) {
	for (size_t i = 0; i < ime->forwarded_key_count; i++) {
		if (ime->forwarded_keys[i].keyboard == keyboard &&
				ime->forwarded_keys[i].keycode == keycode) {
			return true;
		}
	}
	return false;
}

static void ime_key_forwarded_add(struct ime *ime,
		struct wlr_keyboard *keyboard, uint32_t keycode) {
	// 自动重复会反复投递 PRESSED; 同一个物理按键只能占一个槽位,
	// 否则长按一个键就会把数组填满, 导致之后其他键的 release 被吞.
	if (ime_key_forwarded(ime, keyboard, keycode)) {
		return;
	}
	if (ime->forwarded_key_count >=
			sizeof(ime->forwarded_keys) / sizeof(ime->forwarded_keys[0])) {
		return;
	}
	struct ime_forwarded_key *key =
		&ime->forwarded_keys[ime->forwarded_key_count++];
	key->keyboard = keyboard;
	key->keycode = keycode;
}

static void ime_key_forwarded_remove(struct ime *ime,
		struct wlr_keyboard *keyboard, uint32_t keycode) {
	for (size_t i = 0; i < ime->forwarded_key_count; i++) {
		if (ime->forwarded_keys[i].keyboard == keyboard &&
				ime->forwarded_keys[i].keycode == keycode) {
			ime->forwarded_keys[i] =
				ime->forwarded_keys[--ime->forwarded_key_count];
			return;
		}
	}
}

// 键盘被拔掉时其按键不会再有 release: 清掉该键盘的所有记账,
// 避免这些槽位一直占着并让后续按键无法配对.
static void ime_key_forwarded_remove_keyboard(struct ime *ime,
		struct wlr_keyboard *keyboard) {
	size_t i = 0;
	while (i < ime->forwarded_key_count) {
		if (ime->forwarded_keys[i].keyboard == keyboard) {
			ime->forwarded_keys[i] =
				ime->forwarded_keys[--ime->forwarded_key_count];
		} else {
			i++;
		}
	}
}

// 把 text input 状态 (周围文本、内容类型、变更原因) 转发给输入法,
// 并用 done 结束这次交互
static void ime_send_text_input_state(struct server *server,
		struct wlr_text_input_v3 *text_input) {
	struct wlr_input_method_v2 *context = server->input_method;
	if (context == NULL) {
		return;
	}
	if (text_input->active_features &
			WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) {
		wlr_input_method_v2_send_surrounding_text(context,
			text_input->current.surrounding.text,
			text_input->current.surrounding.cursor,
			text_input->current.surrounding.anchor);
	}
	wlr_input_method_v2_send_text_change_cause(context,
		text_input->current.text_change_cause);
	if (text_input->active_features &
			WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE) {
		wlr_input_method_v2_send_content_type(context,
			text_input->current.content_type.hint,
			text_input->current.content_type.purpose);
	}
	wlr_input_method_v2_send_done(context);
}

// ------------------------------------------------------------------
// 聚焦 surface / 活动 text input
// ------------------------------------------------------------------

// 显式清空客户端侧的 preedit.
// text-input-v3 规定收到 leave 时客户端应自行重置 preedit, 但并非所有客户端
// (例如 wezterm) 都这么做: 组合串会残留在终端里, 直到下一次 preedit 更新
// 才被覆盖. 所以离开 / 提交清空时主动发一个空 preedit + done.
static void text_input_clear_preedit(struct wlr_text_input_v3 *input) {
	wlr_text_input_v3_send_preedit_string(input, "", 0, 0);
	wlr_text_input_v3_send_done(input);
}

// 向每个 text input 发送 enter/leave, 使恰好"聚焦 surface 所属客户端"的
// text input 处于聚焦状态. 发送 enter 让客户端知道可以启用文本输入了
// (GTK/Qt 只在 enter 之后才 enable).
static void update_text_inputs_focused_surface(struct server *server) {
	struct wlr_surface *surface = server->ime_focused_surface;
	struct text_input *ti;
	wl_list_for_each(ti, &server->text_inputs, link) {
		struct wlr_text_input_v3 *input = ti->text_input;
		struct wlr_surface *new_focused = NULL;
		if (server->input_method != NULL && surface != NULL &&
				same_client(input, surface)) {
			new_focused = surface;
		}
		if (ti->focused_surface == new_focused) {
			continue;
		}
		if (ti->focused_surface != NULL) {
			text_input_clear_preedit(input);
			wlr_text_input_v3_send_leave(input);
			ti->focused_surface = NULL;
		}
		if (new_focused != NULL) {
			wlr_text_input_v3_send_enter(input, new_focused);
			ti->focused_surface = new_focused;
		}
	}
}

// IM 应与之通信的 text input: 既聚焦、客户端又已启用
static struct text_input *get_active_text_input(struct server *server) {
	if (server->input_method == NULL) {
		return NULL;
	}
	struct text_input *ti;
	wl_list_for_each(ti, &server->text_inputs, link) {
		if (ti->focused_surface != NULL &&
				ti->text_input->current_enabled) {
			return ti;
		}
	}
	return NULL;
}

// (重新) 计算活动 text input, 变化时激活/停用输入法
static void update_active_text_input(struct server *server) {
	struct text_input *active = get_active_text_input(server);
	struct wlr_text_input_v3 *new_active =
		active != NULL ? active->text_input : NULL;
	if (server->input_method != NULL &&
			server->focused_text_input != new_active) {
		if (new_active != NULL) {
			wlr_log(WLR_DEBUG, "ime: sending activate (serial %u -> %u)",
				server->input_method->current_serial,
				server->input_method->current_serial + 1);
			wlr_input_method_v2_send_activate(server->input_method);
		} else {
			wlr_log(WLR_DEBUG, "ime: sending deactivate");
			wlr_input_method_v2_send_deactivate(server->input_method);
		}
		wlr_input_method_v2_send_done(server->input_method);
	}
	server->focused_text_input = new_active;
	// 兜底 (当前注释): 协议要求 deactivate 后候选窗不可见, 但 fcitx5 会自己
	// 销毁 popup, 实测合成器无需介入. 若以后遇到某个 IM 失焦后候选窗残留,
	// 取消下面一行注释, 并配合 ime_update_popup() 里的 set_enabled.
	// ime_update_popup(server);
}

static void ime_focused_surface_destroy(struct wl_listener *listener,
		void *data) {
	struct server *server =
		wl_container_of(listener, server, ime_focused_surface_destroy);
	struct wlr_surface *destroyed = data;
	if (server->ime_focused_surface != destroyed) {
		return;
	}
	// 聚焦 surface 已经在销毁中. 这里不要调 ime_set_focus(NULL):
	// 那会用正在消失的 surface resource 发 leave (Qt 会记录为 null leave surface).
	// wlroots 自己的 per-text-input surface destroy 监听器会清空
	// text_input->focused_surface; 这里只清合成器侧缓存并停用输入法.
	wl_list_remove(&server->ime_focused_surface_destroy.link);
	wl_list_init(&server->ime_focused_surface_destroy.link);
	server->ime_focused_surface = NULL;

	struct text_input *ti;
	wl_list_for_each(ti, &server->text_inputs, link) {
		if (ti->focused_surface == destroyed) {
			ti->focused_surface = NULL;
		}
	}

	update_active_text_input(server);
	ime_update_popup(server);
}

// 把聚焦 surface 与 seat 的 text input (重新) 关联; 键盘焦点变化时调用.
// surface == NULL 会清除关联并停用输入法.
void ime_set_focus(struct server *server, struct wlr_surface *surface) {
	if (server->ime_focused_surface == surface) {
		return;
	}
	if (server->ime_focused_surface != NULL) {
		wl_list_remove(&server->ime_focused_surface_destroy.link);
	}
	server->ime_focused_surface = surface;
	if (surface != NULL) {
		server->ime_focused_surface_destroy.notify =
			ime_focused_surface_destroy;
		wl_signal_add(&surface->events.destroy,
			&server->ime_focused_surface_destroy);
	}
	update_text_inputs_focused_surface(server);
	update_active_text_input(server);
	ime_update_popup(server);
}

// ------------------------------------------------------------------
// 候选窗 (input popup surface)
// ------------------------------------------------------------------

// 一个候选窗. 协议/ wlroots 都允许同一个 IM 同时存在多个 popup surface,
// 必须逐个跟踪, 不能共用一个 wl_listener.
struct ime_popup {
	struct ime *ime;
	struct wl_list link; // ime.popups
	struct wlr_input_popup_surface_v2 *popup_surface;
	struct wlr_scene_surface *scene_surface;
	struct wl_listener commit;
	struct wl_listener popup_destroy;
	struct wl_listener node_destroy; // 场景节点比 popup 对象先消失时清空指针
};

static void ime_popup_free(struct ime_popup *popup) {
	if (popup->commit.link.prev != NULL) {
		wl_list_remove(&popup->commit.link);
	}
	if (popup->popup_destroy.link.prev != NULL) {
		wl_list_remove(&popup->popup_destroy.link);
	}
	if (popup->node_destroy.link.prev != NULL) {
		wl_list_remove(&popup->node_destroy.link);
	}
	// popup 角色销毁时底层 wl_surface 可能还活着 (客户端可以保留它):
	// 显式销毁场景节点, 否则候选窗会留在 overlay 层直到 surface 真正销毁.
	if (popup->scene_surface != NULL) {
		struct wlr_scene_buffer *buffer = popup->scene_surface->buffer;
		popup->scene_surface = NULL;
		wlr_scene_node_destroy(&buffer->node);
	}
	if (popup->link.prev != NULL) {
		wl_list_remove(&popup->link);
	}
	free(popup);
}

static void ime_popup_handle_commit(struct wl_listener *listener,
		void *data) {
	struct ime_popup *popup = wl_container_of(listener, popup, commit);
	ime_update_popup(popup->ime->server);
}

static void ime_popup_handle_popup_destroy(struct wl_listener *listener,
		void *data) {
	struct ime_popup *popup = wl_container_of(listener, popup,
		popup_destroy);
	// listener 在自己的信号发射期间被移除是安全的; 随后销毁场景节点
	// 和结构体.
	ime_popup_free(popup);
}

static void ime_popup_handle_node_destroy(struct wl_listener *listener,
		void *data) {
	struct ime_popup *popup = wl_container_of(listener, popup, node_destroy);
	wl_list_remove(&popup->node_destroy.link);
	// 场景节点被 wl_surface 销毁路径先干掉: 只清指针, popup 对象可能
	// 还会发 destroy, 由那个回调负责释放结构体.
	popup->scene_surface = NULL;
}

// 让候选窗贴着文本光标 (应用没提供光标矩形时用指针位置)
void ime_update_popup(struct server *server) {
	// 文本插入光标的全局矩形 (应用提供时); 同一时刻所有候选窗共用.
	struct wlr_text_input_v3 *ti = server->focused_text_input;
	struct wlr_box caret = {0};
	bool have_caret = false;
	if (ti != NULL &&
			(ti->current.features &
				WLR_TEXT_INPUT_V3_FEATURE_CURSOR_RECTANGLE)) {
		caret = ti->current.cursor_rectangle;
		have_caret = true;
	}

	int lx = (int)server->cursor->x;
	int ly = (int)server->cursor->y;
	int caret_global_x = 0;
	int caret_global_y = 0;
	int surface_global_x = 0;
	int surface_global_y = 0;
	bool caret_mapped = false;
	if (have_caret && server->focused != NULL &&
			server->focused->xdg_toplevel != NULL &&
			server->focused->xdg_toplevel->base != NULL &&
			server->focused->scene_tree != NULL &&
			server->focused->xdg_toplevel->base->surface ==
				server->ime_focused_surface) {
		// text-input-v3 的 cursor_rectangle 是 surface 局部逻辑坐标.
		// xdg 场景树锚定在窗口 geometry 左上角, surface 位于树内 -geometry 处,
		// 所以减去 geometry 偏移得到 surface 左上角的布局坐标.
		// 不要套用输出/分数缩放: 该矩形不是 buffer 像素.
		struct wlr_xdg_surface *focused_base =
			server->focused->xdg_toplevel->base;
		wlr_scene_node_coords(&server->focused->scene_tree->node,
			&surface_global_x, &surface_global_y);
		surface_global_x -= focused_base->geometry.x;
		surface_global_y -= focused_base->geometry.y;
		caret_global_x = surface_global_x + caret.x;
		caret_global_y = surface_global_y + caret.y;
		caret_mapped = true;
		// fcitx5 把候选窗锚在光标矩形的左边缘 (见 classicui/xcbinputwindow.cpp),
		// 所以把 popup 放在文本行下方、水平对齐光标左边缘.
		// 下面的 text-input rectangle 告诉 IM 光标相对该锚点的位置.
		lx = caret_global_x;
		ly = caret_global_y + caret.height;
	}

	struct ime *ime;
	wl_list_for_each(ime, &server->imes, link) {
		struct ime_popup *popup;
		wl_list_for_each(popup, &ime->popups, link) {
			struct wlr_scene_surface *scene_surface =
				popup->scene_surface;
			if (scene_surface == NULL) {
				continue;
			}

			int popup_x = lx;
			int popup_y = ly;
			// 钳制到光标所在的输出内
			struct wlr_output *output = wlr_output_layout_output_at(
				server->output_layout, lx, ly);
			if (output != NULL) {
				struct wlr_box box;
				wlr_output_layout_get_box(server->output_layout,
					output, &box);
				int pw = scene_surface->surface->current.width;
				int ph = scene_surface->surface->current.height;
				if (popup_x + pw > box.x + box.width) {
					popup_x = box.x + box.width - pw;
				}
				if (popup_y + ph > box.y + box.height) {
					popup_y = box.y + box.height - ph;
				}
				if (popup_x < box.x) {
					popup_x = box.x;
				}
				if (popup_y < box.y) {
					popup_y = box.y;
				}
			}

			wlr_scene_node_set_position(&scene_surface->buffer->node,
				popup_x, popup_y);
			// 确保候选列表浮在 layer-shell surface 之上
			wlr_scene_node_raise_to_top(&scene_surface->buffer->node);
			// 兜底 (当前注释): deactivate 后按协议候选窗应不可见.
			// fcitx5 会自行销毁 popup, 不需要这里隐藏; 保留代码以防某个 IM
			// 不销毁 popup 导致候选窗残留.
			// wlr_scene_node_set_enabled(&scene_surface->buffer->node,
			// 	ime->input_method != NULL && ime->input_method->active);

			// 告诉 IM 文本光标的位置 (相对 popup).
			// 该矩形是光标在 surface 局部的框, 换算到 popup surface 的坐标系;
			// 必须用光标左上角, 而不是 popup 锚点 (光标下方).
			if (popup->popup_surface != NULL) {
				struct wlr_box sbox = {
					.x = caret_mapped ? caret_global_x - popup_x
						: lx - popup_x,
					.y = caret_mapped ? caret_global_y - popup_y
						: ly - popup_y,
					.width = caret.width,
					.height = caret.height,
				};
				wlr_input_popup_surface_v2_send_text_input_rectangle(
					popup->popup_surface, &sbox);
			}
		}
	}
}

// ------------------------------------------------------------------
// 输入法 (fcitx5)
// ------------------------------------------------------------------

// 把按键事件转发给抓取挂接在此键盘上的每个输入法.
// 由 input.c 的单一按键处理器在检查完合成器快捷键后调用,
// 所以被合成器消费的键根本不会传到这里 - 不依赖跨监听器的握手或顺序.

static struct ime_keyboard *ime_keyboard_find(struct ime *ime,
		struct wlr_keyboard *keyboard) {
	struct ime_keyboard *ik;
	wl_list_for_each(ik, &ime->keyboards, link) {
		if (ik->keyboard == keyboard) {
			return ik;
		}
	}
	return NULL;
}

// 这个键盘是否已连接到该 IM 的抓取.
static bool ime_keyboard_has(struct ime *ime,
		struct wlr_keyboard *keyboard) {
	return ime_keyboard_find(ime, keyboard) != NULL;
}

// 每块键盘自己的修饰符监听器. 输入法协议只有一个键盘抓取, 但一个客户端
// 可能同时有内置键盘和外接键盘; 转发按键前会补发该键盘的 modifiers,
// 所以这里只在最后转发过按键的键盘上同步 modifiers, 避免不同键盘的状态
// 互相覆盖.
static void ime_keyboard_grab_modifiers(struct wl_listener *listener,
		void *data) {
	struct ime_keyboard *ik = wl_container_of(listener, ik, modifiers);
	struct ime *ime = ik->ime;
	(void)data;
	if (ime->input_method == NULL ||
			ime->input_method->keyboard_grab == NULL) {
		return;
	}
	if (ime->last_keyboard != NULL && ime->last_keyboard != ik->keyboard) {
		return;
	}
	wlr_input_method_keyboard_grab_v2_send_modifiers(
		ime->input_method->keyboard_grab, &ik->keyboard->modifiers);
}

static void ime_keyboard_grab_destroy(struct wl_listener *listener,
		void *data) {
	struct ime *ime = wl_container_of(listener, ime, keyboard_grab_destroy);
	(void)data;
	struct ime_keyboard *ik, *tmp;
	wl_list_for_each_safe(ik, tmp, &ime->keyboards, link) {
		wl_list_remove(&ik->modifiers.link);
		wl_list_remove(&ik->link);
		free(ik);
	}
	wl_list_remove(&ime->keyboard_grab_destroy.link);
	ime->keyboard_grab_destroy_added = false;
	ime->keyboard = NULL;
	ime->last_keyboard = NULL;
	ime->forwarded_key_count = 0;
}

// 把一块键盘连接到 IM 抓取. 第一块键盘同时成为抓取的 keymap/repeat
// 提供者; 之后出现的键盘只增加转发路径, 不会重置输入法的键盘状态.
// 幂等.
static void ime_keyboard_add(struct ime *ime,
		struct wlr_keyboard *keyboard) {
	if (keyboard == NULL || ime->input_method == NULL ||
			ime->input_method->keyboard_grab == NULL) {
		return;
	}
	if (ime_keyboard_has(ime, keyboard)) {
		return;
	}
	struct ime_keyboard *ik = calloc(1, sizeof(*ik));
	if (ik == NULL) {
		return;
	}
	ik->ime = ime;
	ik->keyboard = keyboard;
	ik->modifiers.notify = ime_keyboard_grab_modifiers;
	wl_signal_add(&keyboard->events.modifiers, &ik->modifiers);
	wl_list_insert(ime->keyboards.prev, &ik->link);

	if (!ime->keyboard_grab_destroy_added) {
		ime->keyboard_grab_destroy.notify = ime_keyboard_grab_destroy;
		wl_signal_add(&ime->input_method->keyboard_grab->events.destroy,
			&ime->keyboard_grab_destroy);
		ime->keyboard_grab_destroy_added = true;
	}

	wlr_log(WLR_DEBUG,
		"ime: adding keyboard %p to grab (seat=%p primary=%p vk=%d)",
		(void *)keyboard, (void *)wlr_seat_get_keyboard(ime->server->seat),
		(void *)ime->keyboard,
		is_keyboard_emulated_by_input_method(keyboard, ime->input_method));

	if (ime->keyboard != NULL) {
		return;
	}

	// 第一块键盘: 设置抓取的 keymap/repeat 信息, 并发送一次当前
	// modifiers; 后续按键由 input.c 通过 ime_forward_key() 转发.
	ime->keyboard = keyboard;
	ime->last_keyboard = NULL;
	wlr_input_method_keyboard_grab_v2_set_keyboard(
		ime->input_method->keyboard_grab, keyboard);
	wlr_input_method_keyboard_grab_v2_send_modifiers(
		ime->input_method->keyboard_grab, &keyboard->modifiers);
	ime->last_keyboard = keyboard;
}

// 从 IM 抓取中移除一块键盘 (设备销毁). 若它是提供 keymap 的主键盘,
// 从剩余键盘里重新选一块; 有别的真实键盘时输入法不会失去键盘抓取.
static void ime_keyboard_remove(struct ime *ime,
		struct wlr_keyboard *keyboard) {
	struct ime_keyboard *ik = ime_keyboard_find(ime, keyboard);
	if (ik == NULL) {
		return;
	}
	bool was_primary = ime->keyboard == keyboard;
	bool was_last = ime->last_keyboard == keyboard;

	wl_list_remove(&ik->modifiers.link);
	wl_list_remove(&ik->link);
	free(ik);

	// 设备已经销毁, 它的按键不会再产生 release.
	ime_key_forwarded_remove_keyboard(ime, keyboard);
	if (was_last) {
		ime->last_keyboard = NULL;
	}
	if (!was_primary) {
		return;
	}

	ime->keyboard = NULL;
	ime->last_keyboard = NULL;
	if (ime->input_method == NULL ||
			ime->input_method->keyboard_grab == NULL ||
			wl_list_empty(&ime->keyboards)) {
		return;
	}
	struct ime_keyboard *next =
		wl_container_of(ime->keyboards.next, next, link);
	ime->keyboard = next->keyboard;
	// 新主键盘的 modifiers 可能和旧的不同, 换主键盘后要补发一次.
	wlr_input_method_keyboard_grab_v2_set_keyboard(
		ime->input_method->keyboard_grab, next->keyboard);
	wlr_input_method_keyboard_grab_v2_send_modifiers(
		ime->input_method->keyboard_grab, &next->keyboard->modifiers);
	ime->last_keyboard = next->keyboard;
}

// 把当前所有真实键盘接入抓取. seat 键盘优先作为主键盘 (提供 keymap/repeat);
// IM 自己的 passthrough 虚拟键盘绝不接入, 否则会形成按键回环.
static void ime_keyboards_attach_all(struct server *server, struct ime *ime) {
	if (ime->input_method == NULL ||
			ime->input_method->keyboard_grab == NULL) {
		return;
	}
	struct wlr_keyboard *seat_keyboard = wlr_seat_get_keyboard(server->seat);
	if (seat_keyboard != NULL && keyboard_is_typing(&seat_keyboard->base) &&
			!is_keyboard_emulated_by_input_method(seat_keyboard,
				ime->input_method)) {
		ime_keyboard_add(ime, seat_keyboard);
	}
	struct keyboard *kb;
	wl_list_for_each(kb, &server->keyboards, link) {
		if (kb->keyboard == seat_keyboard) {
			continue;
		}
		if (keyboard_is_typing(&kb->keyboard->base) &&
				!is_keyboard_emulated_by_input_method(kb->keyboard,
					ime->input_method)) {
			ime_keyboard_add(ime, kb->keyboard);
		}
	}
}

void ime_forward_key(struct server *server, struct wlr_keyboard *keyboard,
		struct wlr_keyboard_key_event *event) {
	struct ime *ime;
	wl_list_for_each(ime, &server->imes, link) {
		if (!ime_keyboard_has(ime, keyboard) ||
				ime->input_method == NULL ||
				ime->input_method->keyboard_grab == NULL) {
			continue;
		}
		struct wlr_input_method_keyboard_grab_v2 *grab =
			ime->input_method->keyboard_grab;
		// 绝不转发没有匹配按下记录的释放
		if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED &&
				!ime_key_forwarded(ime, keyboard, event->keycode)) {
			continue;
		}
		if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
			ime_key_forwarded_add(ime, keyboard, event->keycode);
		} else {
			ime_key_forwarded_remove(ime, keyboard, event->keycode);
		}
		// 不同键盘有各自的 modifiers 状态. 输入法只保留一份状态,
		// 所以在键盘切换时补发当前键盘的 modifiers.
		if (ime->last_keyboard != keyboard) {
			wlr_input_method_keyboard_grab_v2_send_modifiers(grab,
				&keyboard->modifiers);
			ime->last_keyboard = keyboard;
		}
		wlr_input_method_keyboard_grab_v2_send_key(grab, event->time_msec,
			event->keycode, event->state);
	}
}

static void ime_grab_keyboard(struct wl_listener *listener, void *data) {
	struct ime *ime = wl_container_of(listener, ime, grab_keyboard);
	struct server *server = ime->server;
	(void)data;
	wlr_log(WLR_DEBUG, "ime: grab_keyboard event (seat=%p)",
		(void *)wlr_seat_get_keyboard(server->seat));
	ime_keyboards_attach_all(server, ime);
}

// input.c 在被抓键盘销毁时调用: 从抓取中移除该键盘, 并让其他真实键盘
// 继续工作; 全部键盘都消失时输入法保留抓取, 等新键盘出现再接入.
void ime_detach_keyboard(struct server *server,
		struct wlr_keyboard *keyboard) {
	struct ime *ime;
	wl_list_for_each(ime, &server->imes, link) {
		ime_keyboard_remove(ime, keyboard);
	}
}

// input.c 在键盘设备出现时调用: 新真实键盘也接入已有的 IM 抓取, 不再只
// 支持单块键盘. IM 自己的重注入设备永远不会接入.
void ime_attach_keyboard(struct server *server,
		struct wlr_keyboard *keyboard) {
	struct ime *ime;
	wl_list_for_each(ime, &server->imes, link) {
		if (ime->input_method != NULL &&
				ime->input_method->keyboard_grab != NULL &&
				keyboard_is_typing(&keyboard->base) &&
				!is_keyboard_emulated_by_input_method(keyboard,
					ime->input_method)) {
			ime_keyboard_add(ime, keyboard);
		}
	}
}

// 这个键盘是否会被转发给某个 IM 抓取?
bool ime_keyboard_grabbed(struct server *server,
		struct wlr_keyboard *keyboard) {
	struct ime *ime;
	wl_list_for_each(ime, &server->imes, link) {
		if (ime->input_method != NULL &&
				ime->input_method->keyboard_grab != NULL &&
				ime_keyboard_has(ime, keyboard)) {
			return true;
		}
	}
	return false;
}

// IM 提交了状态变化: 把 preedit/commit/delete 转发给活动 text input
static void ime_commit(struct wl_listener *listener, void *data) {
	struct ime *ime = wl_container_of(listener, ime, commit);
	// wlroots 0.20 的 input_method->events.commit 以 NULL data 发出
	// (上下文就是 ime 自己的 input method); 0.19 会把 input method 作为 data 传入 -
	// 两种情况下 ime->input_method 都是正确对象
	(void)data;
	struct wlr_input_method_v2 *context = ime->input_method;
	struct server *server = ime->server;
	struct wlr_text_input_v3 *text_input = server->focused_text_input;
	wlr_log(WLR_DEBUG, "ime: IM commit active=%d text_input=%p serial=%u commit='%s' preedit='%s'",
		context->active, (void *)text_input, context->current_serial,
		context->current.commit_text ? context->current.commit_text : "",
		context->current.preedit.text ? context->current.preedit.text : "");
	if (text_input == NULL) {
		return;
	}
	struct wlr_input_method_v2_state *state = &context->current;
	// 始终转发 preedit, 空串也要发: 输入法用"本次 commit 没有 set_preedit_string"
	// 表示清空组合串. 若这里跳过, 客户端会一直显示上一次的 preedit
	// (终端里残留的 "nihao" 就是这么来的).
	wlr_text_input_v3_send_preedit_string(text_input,
		state->preedit.text != NULL ? state->preedit.text : "",
		state->preedit.text != NULL ? state->preedit.cursor_begin : 0,
		state->preedit.text != NULL ? state->preedit.cursor_end : 0);
	if (state->commit_text != NULL) {
		wlr_text_input_v3_send_commit_string(text_input,
			state->commit_text);
	}
	if (state->delete.before_length != 0 ||
			state->delete.after_length != 0) {
		wlr_text_input_v3_send_delete_surrounding_text(text_input,
			state->delete.before_length, state->delete.after_length);
	}
	wlr_text_input_v3_send_done(text_input);
}

// 候选窗: 把 popup surface 放到 overlay 图层, 让 fcitx5 的候选列表可见
static void ime_new_popup_surface(struct wl_listener *listener, void *data) {
	struct ime *ime = wl_container_of(listener, ime, new_popup_surface);
	struct wlr_input_popup_surface_v2 *popup_surface = data;
	struct server *server = ime->server;

	struct ime_popup *popup = calloc(1, sizeof(*popup));
	if (popup == NULL) {
		return;
	}
	popup->ime = ime;
	popup->popup_surface = popup_surface;

	struct wlr_scene_surface *scene_surface = wlr_scene_surface_create(
		server->layers[LAYER_OVERLAY], popup_surface->surface);
	if (scene_surface == NULL) {
		free(popup);
		return;
	}
	popup->scene_surface = scene_surface;
	wlr_scene_node_set_position(&scene_surface->buffer->node,
		(int)server->cursor->x, (int)server->cursor->y);
	wlr_scene_node_raise_to_top(&scene_surface->buffer->node);

	popup->commit.notify = ime_popup_handle_commit;
	wl_signal_add(&popup_surface->surface->events.commit, &popup->commit);
	popup->popup_destroy.notify = ime_popup_handle_popup_destroy;
	wl_signal_add(&popup_surface->events.destroy, &popup->popup_destroy);
	// 场景节点可能随底层 wl_surface 先销毁: 监听它, 避免留下悬空
	// scene_surface 指针. 结构体本身仍由 popup destroy 回调释放.
	popup->node_destroy.notify = ime_popup_handle_node_destroy;
	wl_signal_add(&scene_surface->buffer->node.events.destroy,
		&popup->node_destroy);

	wl_list_insert(ime->popups.prev, &popup->link);
	ime_update_popup(server);
}

static void ime_destroy(struct wl_listener *listener, void *data) {
	struct ime *ime = wl_container_of(listener, ime, destroy);
	struct server *server = ime->server;

	// 先摘掉所有键盘监听器: 这些信号可能比 input method 活得更久.
	struct ime_keyboard *ik, *ik_tmp;
	wl_list_for_each_safe(ik, ik_tmp, &ime->keyboards, link) {
		wl_list_remove(&ik->modifiers.link);
		wl_list_remove(&ik->link);
		free(ik);
	}
	if (ime->keyboard_grab_destroy_added) {
		wl_list_remove(&ime->keyboard_grab_destroy.link);
		ime->keyboard_grab_destroy_added = false;
	}
	ime->keyboard = NULL;
	ime->last_keyboard = NULL;
	ime->forwarded_key_count = 0;
	// 正常路径下 input_method 先销毁所有 popup surface, 这里列表已经空了;
	// 仍做一次兜底清理, 避免异常销毁顺序留下场景节点/监听器.
	struct ime_popup *popup, *popup_tmp;
	wl_list_for_each_safe(popup, popup_tmp, &ime->popups, link) {
		ime_popup_free(popup);
	}
	wl_list_remove(&ime->destroy.link);
	wl_list_remove(&ime->grab_keyboard.link);
	wl_list_remove(&ime->commit.link);
	wl_list_remove(&ime->new_popup_surface.link);
	wl_list_remove(&ime->link);
	if (server->input_method == ime->input_method) {
		server->input_method = NULL;
	}
	server->focused_text_input = NULL;
	// 没有 IM 时, text input 不能保持"聚焦" (enter 只是因为 IM 才发送的):
	// 给所有 text input 发 leave
	update_text_inputs_focused_surface(server);
	free(ime);
}

void ime_new_input_method(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_ime);
	struct wlr_input_method_v2 *input_method = data;

	if (server->input_method != NULL) {
		wlr_log(WLR_INFO, "ignoring additional input method");
		wlr_input_method_v2_send_unavailable(input_method);
		return;
	}
	server->input_method = input_method;

	struct ime *ime = calloc(1, sizeof(*ime));
	if (ime == NULL) {
		server->input_method = NULL;
		return;
	}
	ime->server = server;
	ime->input_method = input_method;
	wl_list_init(&ime->keyboards);
	wl_list_init(&ime->popups);

	ime->destroy.notify = ime_destroy;
	wl_signal_add(&input_method->events.destroy, &ime->destroy);
	ime->grab_keyboard.notify = ime_grab_keyboard;
	wl_signal_add(&input_method->events.grab_keyboard, &ime->grab_keyboard);
	ime->commit.notify = ime_commit;
	wl_signal_add(&input_method->events.commit, &ime->commit);
	ime->new_popup_surface.notify = ime_new_popup_surface;
	wl_signal_add(&input_method->events.new_popup_surface,
		&ime->new_popup_surface);

	wl_list_insert(server->imes.prev, &ime->link);

	// IM 在 surface 聚焦之后才出现: 现在有了输入法, 聚焦客户端的 text input
	// 可以收到 enter, 并在客户端 enable 后激活 IM
	update_text_inputs_focused_surface(server);
	update_active_text_input(server);
	ime_update_popup(server);
}

// ------------------------------------------------------------------
// text input (聚焦应用)
// ------------------------------------------------------------------

static void text_input_commit(struct wl_listener *listener, void *data) {
	struct text_input *ti = wl_container_of(listener, ti, commit);
	struct server *server = ti->server;
	if (server->focused_text_input != ti->text_input) {
		return;
	}
	// 应用更新了文本 (周围文本、光标等): 中继
	ime_send_text_input_state(server, ti->text_input);
	ime_update_popup(server);
}

static void text_input_enable(struct wl_listener *listener, void *data) {
	struct text_input *ti = wl_container_of(listener, ti, enable);
	struct server *server = ti->server;
	update_active_text_input(server);
	if (server->focused_text_input == ti->text_input) {
		// 应用在聚焦 surface 上启用了文本输入: 确保 IM 知道客户端已就绪,
		// 并把当前状态给它
		ime_send_text_input_state(server, ti->text_input);
		ime_update_popup(server);
	}
}

static void text_input_disable(struct wl_listener *listener, void *data) {
	struct text_input *ti = wl_container_of(listener, ti, disable);
	// 应用停用自己的文本输入后它就不再活动; 若没有其他活动的就停用 IM
	// (这也覆盖 disable 在焦点已移到别的客户端之后才到达的情况)
	update_active_text_input(ti->server);
}

static void text_input_destroy(struct wl_listener *listener, void *data) {
	struct text_input *ti = wl_container_of(listener, ti, destroy);
	struct server *server = ti->server;
	wl_list_remove(&ti->destroy.link);
	wl_list_remove(&ti->enable.link);
	wl_list_remove(&ti->disable.link);
	wl_list_remove(&ti->commit.link);
	wl_list_remove(&ti->link);
	update_active_text_input(server);
	free(ti);
}

void ime_new_text_input(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_text_input);
	struct wlr_text_input_v3 *input = data;
	wlr_log(WLR_DEBUG, "ime: new text input %p (client pid?)",
		(void *)input);

	struct text_input *ti = calloc(1, sizeof(*ti));
	if (ti == NULL) {
		return;
	}
	ti->server = server;
	ti->text_input = input;

	ti->destroy.notify = text_input_destroy;
	wl_signal_add(&input->events.destroy, &ti->destroy);
	ti->enable.notify = text_input_enable;
	wl_signal_add(&input->events.enable, &ti->enable);
	ti->disable.notify = text_input_disable;
	wl_signal_add(&input->events.disable, &ti->disable);
	ti->commit.notify = text_input_commit;
	wl_signal_add(&input->events.commit, &ti->commit);

	wl_list_insert(server->text_inputs.prev, &ti->link);

	// text input 可能在其 surface 聚焦之后才创建 (如 GTK 在 map 时创建):
	// 若合适现在发送 enter, 并 (重新) 计算活动 text input
	update_text_inputs_focused_surface(server);
	update_active_text_input(server);
}
