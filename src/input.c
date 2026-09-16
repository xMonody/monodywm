// input.c - seat、键盘与合成器快捷键
//
// 键盘焦点由聚焦的 toplevel 驱动; 按键先与合成器自己的快捷键表匹配,
// 否则转发给聚焦客户端. 指针/键盘设备 (含用于测试的虚拟指针) 在这里
// 挂到共享 cursor 上.

#include "ipc.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libinput.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/util/log.h>

// ------------------------------------------------------------------
// seat 请求
// ------------------------------------------------------------------

static void client_cursor_surface_destroy(struct wl_listener *listener,
		void *data) {
	struct server *server =
		wl_container_of(listener, server, client_cursor_destroy);
	wl_list_remove(&server->client_cursor_destroy.link);
	server->client_cursor_surface = NULL;
	// 客户端光标 surface 消失: 若合成器没接管光标 (标题栏/resize 边缘),
	// 显示默认箭头
	if (server->cursor_override == NULL) {
		wlr_log(WLR_DEBUG, "cursor: default (cursor surface gone)");
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
			"left_ptr");
	}
}

static void client_cursor_shape_client_destroy(struct wl_listener *listener,
		void *data) {
	struct server *server = wl_container_of(listener, server,
		client_cursor_shape_client_destroy);
	wl_list_remove(&server->client_cursor_shape_client_destroy.link);
	server->client_cursor_shape = 0;
	server->client_cursor_shape_client = NULL;
	// 拥有光标形状的客户端消失: 除非合成器当前接管光标, 否则回到默认箭头
	if (server->cursor_override == NULL) {
		wlr_log(WLR_DEBUG, "cursor: default (shape client gone)");
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
			"left_ptr");
	}
}

// 只在监听器仍挂着时移除: 对已摘下的 link 调 wl_list_remove 会破坏它曾所在的链表
static void listener_remove_if_attached(struct wl_listener *listener) {
	if (listener->link.prev != NULL) {
		wl_list_remove(&listener->link);
	}
}

void seat_request_set_cursor(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		seat_request_set_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	// 合成器交互式移动/缩放期间接管光标: 抓取结束前忽略所有客户端光标请求
	// (labwc 的 input_mode 门控)
	if (server->input_mode != INPUT_MODE_PASSTHROUGH) {
		return;
	}
	// 隐式抓取: 客户端按住指针按钮时 (如文本选择), 光标冻结在抓取开始的状态;
	// 忽略拖动中途客户端的光标变更 (如 CSD 客户端靠近边缘时切换自己的 resize 光标)
	if (server->seat->pointer_state.button_count > 0) {
		return;
	}
	// 合成器自己的边框区 (标题栏/resize 边缘) 在那里接管光标, 忽略客户端请求 -
	// 客户端仍收到 motion 并保留悬停反馈, 只是不能改光标.
	// 被捕获的指针 (pointer-constraints) 即使在边框区也属于客户端,
	// 所以它可以在那里设置或隐藏自己的光标.
	if (!pointer_constraint_active(server) &&
			pointer_over_frame_zone(server)) {
		return;
	}
	if (event->seat_client == server->seat->pointer_state.focused_client) {
		// 光标 surface 覆盖此前设置的任何光标形状
		// (混用两种协议的客户端: 最后一次请求生效)
		server->client_cursor_shape = 0;
		if (server->client_cursor_shape_client != NULL) {
			listener_remove_if_attached(
				&server->client_cursor_shape_client_destroy);
			server->client_cursor_shape_client = NULL;
		}
		// 记住客户端光标, 以便合成器光标覆盖 (标题栏/resize 边缘) 结束后恢复
		if (server->client_cursor_surface != event->surface) {
			if (server->client_cursor_surface != NULL) {
				listener_remove_if_attached(&server->client_cursor_destroy);
			}
			server->client_cursor_surface = event->surface;
			if (event->surface != NULL) {
				server->client_cursor_destroy.notify =
					client_cursor_surface_destroy;
				wl_signal_add(&event->surface->events.destroy,
					&server->client_cursor_destroy);
			}
		}
		server->client_cursor_hotspot_x = event->hotspot_x;
		server->client_cursor_hotspot_y = event->hotspot_y;
		wlr_cursor_set_surface(server->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
		if (server->cursor_override != NULL) {
			// 进入 surface 会让客户端重新请求光标; 当我们接管光标
			// (标题区/resize 边缘) 时, 立即重新套用自己的光标,
			// 让样式在进入时就改变, 而不是等到下一次 motion
			wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
				server->cursor_override);
		}
	}
}

// 指针所在的 toplevel (其 surface 或它同客户端的 surface); 找不到返回 NULL.
// 用于抑制那些自己画 CSD 边框、但在这里被视为无装饰的客户端 (Firefox 等)
// 自己发的方向性窗口缩放形状: 这类窗口的缩放归合成器管, 那些形状会为
// 永远不会发生的缩放 (如最大化窗口, 合成器从不在其边缘缩放) 显示缩放光标.
static struct toplevel *toplevel_for_surface(struct server *server,
		struct wlr_surface *surface) {
	if (surface == NULL) {
		return NULL;
	}
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		if (tl->xdg_toplevel->base != NULL &&
				tl->xdg_toplevel->base->surface != NULL &&
				(tl->xdg_toplevel->base->surface == surface ||
				 tl->xdg_toplevel->base->surface->resource->client ==
					surface->resource->client)) {
			return tl;
		}
	}
	return NULL;
}

void seat_request_set_shape(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		cursor_shape_set_shape);
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	// 合成器交互式移动/缩放期间接管光标: 抓取结束前忽略所有客户端光标请求
	// (labwc 的 input_mode 门控)
	if (server->input_mode != INPUT_MODE_PASSTHROUGH) {
		return;
	}
	if (event->device_type !=
			WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER) {
		return; // 本合成器不支持数位板
	}
	// 隐式抓取: 客户端按住指针按钮时 (如文本选择), 光标冻结在抓取开始的状态;
	// 忽略拖动中途客户端的光标变更 (如 CSD 客户端靠近边缘时切换自己的 resize 光标)
	if (server->seat->pointer_state.button_count > 0) {
		return;
	}
	// 合成器自己的边框区在那里接管光标; 客户端仍收到 motion 和悬停反馈,
	// 只是不能改光标
	if (pointer_over_frame_zone(server)) {
		return;
	}
	if (event->seat_client != server->seat->pointer_state.focused_client) {
		return;
	}
	// 自己画装饰但从不协商 xdg-decoration 的客户端 (Firefox、无 CSD 的 Chromium 等)
	// 被视为无装饰窗口: 合成器接管其边框并负责 resize 边缘和光标.
	// 因此它自己的方向性窗口缩放形状 (e-resize、s-resize 等) 描述的是
	// 永远不会发生的缩放 (合成器在边缘抓走按压, 且从不缩放最大化窗口),
	// 直接忽略 - 只保留合成器的缩放光标.
	// Web 内容光标 (ew-resize 分隔条、text、pointer 等) 不受影响.
	struct wlr_surface *focused =
		server->seat->pointer_state.focused_surface;
	struct toplevel *tl = toplevel_for_surface(server, focused);
	if (tl != NULL &&
			tl->decoration_mode !=
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE &&
			event->shape >= WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE &&
			event->shape <= WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE) {
		wlr_log(WLR_DEBUG, "cursor: ignore %s resize shape from %s "
			"(compositor owns the frame)",
			wlr_cursor_shape_v1_name(event->shape),
			tl->app_id ? tl->app_id : "?");
		return;
	}
	// 记住该形状, 以便 pointer.c 在合成器光标覆盖结束后恢复;
	// 形状覆盖更早的光标 surface (混用协议: 最后一次请求生效).
	// 跟踪所属 seat client, 避免把过期形状显示给别的客户端.
	if (server->client_cursor_surface != NULL) {
		wl_list_remove(&server->client_cursor_destroy.link);
		server->client_cursor_surface = NULL;
	}
	if (server->client_cursor_shape_client != event->seat_client) {
		if (server->client_cursor_shape_client != NULL) {
			listener_remove_if_attached(
				&server->client_cursor_shape_client_destroy);
		}
		server->client_cursor_shape_client_destroy.notify =
			client_cursor_shape_client_destroy;
		wl_signal_add(&event->seat_client->events.destroy,
			&server->client_cursor_shape_client_destroy);
		server->client_cursor_shape_client = event->seat_client;
	}
	server->client_cursor_shape = event->shape;
	wlr_log(WLR_DEBUG, "cursor: shape %d", event->shape);
	if (server->cursor_override != NULL) {
		// 我们接管着光标 (标题区/resize 边缘): 保持覆盖,
		// 只记住客户端的偏好供以后使用
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
			server->cursor_override);
		return;
	}
	// 自己渲染形状: 图像来自合成器 xcursor 主题并按当前输出缩放,
	// 所以光标大小总能匹配合成器自己的光标 - 客户端无需猜测
	wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
		wlr_cursor_shape_v1_name(event->shape));
}

void seat_request_set_selection(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		seat_request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

void seat_request_set_primary_selection(struct wl_listener *listener,
		void *data) {
	struct server *server = wl_container_of(listener, server,
		seat_request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

void seat_request_start_drag(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		seat_request_start_drag);
	struct wlr_seat_request_start_drag_event *event = data;
	wlr_seat_start_drag(server->seat, event->drag, event->serial);
}

void seat_start_drag(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, seat_start_drag);
	struct wlr_drag *drag = data;
	if (drag->icon == NULL) {
		return;
	}
	server->drag_tree = wlr_scene_drag_icon_create(
		server->layers[LAYER_OVERLAY], drag->icon);
	if (server->drag_tree != NULL) {
		wlr_scene_node_set_position(&server->drag_tree->node,
			server->cursor->x, server->cursor->y);
	}
}

// ------------------------------------------------------------------
// 键盘
// ------------------------------------------------------------------

// 一个已连接的键盘设备 (真实或虚拟); 每个都有自己的监听器,
// 这样重注入设备 (如 fcitx5 的 passthrough 虚拟键盘) 的按键
// 也仍能到达聚焦客户端. 结构体定义在 server.h: ime.c 遍历同一链表
// 为 IM 抓取挑选真实键盘.
//
// 这个设备是真正的打字键盘吗? libinput 会把特殊按键 (Power Button、
// Lid Switch、Video Bus 等) 归类为键盘, 因为它们有按键能力; 这类伪键盘
// 绝不能成为 seat 键盘或输入法的抓取键盘, 否则真实键盘上的每次击键都会绕过 IM.
// 虚拟键盘 (IM 的 passthrough 设备、测试工具) 始终算打字键盘.
bool keyboard_is_typing(struct wlr_input_device *device) {
	if (!wlr_input_device_is_libinput(device)) {
		return true;
	}
	struct libinput_device *dev = wlr_libinput_get_device_handle(device);
	if (dev == NULL) {
		return true;
	}
	return libinput_device_keyboard_has_key(dev, KEY_Q) ||
		libinput_device_keyboard_has_key(dev, KEY_A) ||
		libinput_device_keyboard_has_key(dev, KEY_SPACE);
}

static void keyboard_modifiers(struct wl_listener *listener, void *data) {
	struct keyboard *kb = wl_container_of(listener, kb, modifiers);
	struct server *server = kb->server;
	struct wlr_keyboard *keyboard = data;
	wlr_seat_keyboard_notify_modifiers(server->seat, &keyboard->modifiers);
}

// 相对聚焦窗口的下一个/上一个已映射 toplevel, 会绕回链表.
// 含最小化窗口 (调用方会还原它们); 跳过未映射的 toplevel.
static struct toplevel *cycle_toplevel(struct server *server, bool next) {
	if (server->focused == NULL) {
		struct toplevel *tl;
		wl_list_for_each(tl, &server->toplevels, link) {
			if (tl->xdg_toplevel->base != NULL &&
					tl->xdg_toplevel->base->surface->mapped) {
				return tl;
			}
		}
		return NULL;
	}
	return neighbor_toplevel(server, server->focused, next, true);
}

// 按创建顺序的第 N 个已映射 toplevel (0 基); 没有则返回 NULL.
// 供 MOD+1..9 任务切换使用.
static struct toplevel *nth_toplevel(struct server *server, int index) {
	struct toplevel *tl;
	int i = 0;
	wl_list_for_each(tl, &server->toplevels, link) {
		if (tl->xdg_toplevel->base == NULL ||
				!tl->xdg_toplevel->base->surface->mapped) {
			continue;
		}
		if (i == index) {
			return tl;
		}
		i++;
	}
	return NULL;
}

// 聚焦一个 toplevel, 先把它从最小化还原
// (它会出现在记忆的位置, 因为最小化只是隐藏场景节点)
void focus_window(struct server *server, struct toplevel *tl) {
	if (tl == NULL) {
		return;
	}
	if (tl->minimized) {
		set_minimized(server, tl, false);
	}
	focus_toplevel(server, tl);
	update_cursor_style(server);
}

// 从 config_app_shortcuts 表启动应用
static void spawn_app(const struct config_app_shortcut *app) {
	wlr_log(WLR_DEBUG, "input: spawn app '%s' args='%s'",
		app->app, app->args != NULL ? app->args : "");
	if (app->args != NULL && app->args[0] != '\0') {
		size_t len = strlen(app->app) + 1 + strlen(app->args) + 1;
		char *cmd = malloc(len);
		if (cmd == NULL) {
			return;
		}
		snprintf(cmd, len, "%s %s", app->app, app->args);
		spawn_command(cmd);
		free(cmd);
	} else {
		spawn_command(app->app);
	}
}

// ------------------------------------------------------------------
// keyboard-shortcuts-inhibit
// ------------------------------------------------------------------

// 接受客户端创建的每个 inhibitor: 激活它, 让客户端知道其快捷键会被透传.
// 只有当它的 surface 持有键盘焦点时才生效 (见 shortcuts_suppressed()),
// 这与协议"仅在聚焦时生效"的规则一致.
void new_shortcuts_inhibitor(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		new_shortcuts_inhibitor);
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor = data;
	wlr_log(WLR_DEBUG, "input: shortcuts-inhibit new surface=%p seat=%p",
		(void *)inhibitor->surface, (void *)inhibitor->seat);
	wlr_keyboard_shortcuts_inhibitor_v1_activate(inhibitor);
}

// 当前聚焦客户端的合成器快捷键处理是否被抑制?
// 当 keyboard-shortcuts-inhibit-v1 的 inhibitor 在持有键盘的 surface 上激活时为真.
// 此时客户端 (GTK4 应用、远程桌面、虚拟机界面) 拥有键盘: 合成器转发每个按键,
// 而不是运行自己的绑定.
static bool shortcuts_suppressed(struct server *server) {
	if (server->shortcuts_inhibit == NULL) {
		return false;
	}
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor;
	wl_list_for_each(inhibitor, &server->shortcuts_inhibit->inhibitors,
			link) {
		if (inhibitor->active && inhibitor->seat == server->seat &&
				inhibitor->surface ==
					server->seat->keyboard_state.focused_surface) {
			return true;
		}
	}
	return false;
}

// 合成器键盘快捷键 (修饰符组合和 keysym 定义在 config.h). 按键被消费时返回 true.
static bool keyboard_shortcut(struct server *server,
		struct wlr_keyboard *keyboard, uint32_t keycode) {
	if (keyboard->xkb_state == NULL) {
		return false; // 还没有 keymap: 无从匹配
	}
	// 用按键的基础 keysym (当前布局组的第 0 层) 匹配, 而不是 xkb_state_key_get_syms:
	// 修饰符会改变上报的 keysym (Ctrl+字母变成控制字符, Shift+字母变成大写,
	// 某些布局会重映射 Alt+字母), 这曾静默破坏 Alt+Ctrl+F 这类字母绑定.
	// 配置里的 keysym 都是未修饰的字母/数字/符号, 基础 keysym 正是它们所指的.
	xkb_layout_index_t layout =
		xkb_state_key_get_layout(keyboard->xkb_state, keycode);
	const xkb_keysym_t *syms;
	int nsyms = xkb_keymap_key_get_syms_by_level(keyboard->keymap,
		keycode, layout, 0, &syms);
	uint32_t mods = keyboard->modifiers.depressed | keyboard->modifiers.latched;

	// keyboard-shortcuts-inhibit: 客户端拥有键盘, 合成器不得处理自己的绑定;
	// 所有按键都落下去转发给客户端.
	if (shortcuts_suppressed(server)) {
		return false;
	}

	for (int i = 0; i < nsyms; i++) {
		// 基础 sym 本来就不带 Shift, 但对基础层是大写字母的布局再做一次小写化
		xkb_keysym_t sym = xkb_keysym_to_lower(syms[i]);
		// 遍历配置驱动的动作表 (config.h): 同一个动作可绑定多个 mods+key 组合
		for (size_t j = 0;
				j < sizeof(config_action_shortcuts) /
					sizeof(config_action_shortcuts[0]); j++) {
			const struct config_action_shortcut *sc =
				&config_action_shortcuts[j];
			if ((mods & sc->mods) != sc->mods || sym != sc->key) {
				continue;
			}
			switch (sc->action) {
			case CONFIG_ACTION_QUIT:
				wl_display_terminate(server->display);
				return true;
			case CONFIG_ACTION_MAXIMIZE:
				if (server->focused != NULL) {
					if (server->focused->xdg_toplevel->current.maximized) {
						// 切换: 还原最大化前保存的尺寸/位置
						restore_maximized_toplevel(server->focused, true);
					} else {
						set_maximized(server, server->focused, true);
					}
				}
				return true;
			case CONFIG_ACTION_MINIMIZE:
				if (server->focused != NULL) {
					set_minimized(server, server->focused, true);
				}
				return true;
			case CONFIG_ACTION_NEXT_WINDOW:
				focus_window(server, cycle_toplevel(server, true));
				return true;
			case CONFIG_ACTION_PREV_WINDOW:
				focus_window(server, cycle_toplevel(server, false));
				return true;
			case CONFIG_ACTION_CLOSE:
				if (server->focused != NULL) {
					close_toplevel(server->focused);
				}
				return true;
			case CONFIG_ACTION_CLOSE_OTHER:
				// 关闭除当前聚焦窗口外的所有窗口;
				// close_toplevel 对最小化/最大化/全屏 toplevel 同样有效
				{
					struct toplevel *tl, *tmp;
					wl_list_for_each_safe(tl, tmp, &server->toplevels, link) {
						if (tl != server->focused) {
							close_toplevel(tl);
						}
					}
				}
				return true;
			case CONFIG_ACTION_TASK:
				if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
					struct toplevel *tl = nth_toplevel(server,
						sym - XKB_KEY_1);
					if (tl != NULL) {
						focus_window(server, tl);
					}
				}
				return true;
			}
		}
		for (size_t i = 0;
				i < sizeof(config_app_shortcuts) /
					sizeof(config_app_shortcuts[0]); i++) {
			const struct config_app_shortcut *app =
				&config_app_shortcuts[i];
			if ((mods & app->mods) == app->mods &&
					sym == app->key) {
				spawn_app(app);
				return true;
			}
		}
	}
	return false;
}

static void keyboard_key(struct wl_listener *listener, void *data) {
	struct keyboard *kb = wl_container_of(listener, kb, key);
	struct server *server = kb->server;
	struct wlr_keyboard_key_event *event = data;
	uint32_t keycode = event->keycode + 8;
	wlr_log(WLR_DEBUG, "input: KEY kb=%p code=%u grabbed=%d seat=%p",
		(void *)kb->keyboard, event->keycode,
		ime_keyboard_grabbed(server, kb->keyboard),
		(void *)wlr_seat_get_keyboard(server->seat));

	// 虚拟键盘 (wlr_virtual_keyboard_v1 和 IM 中继的 passthrough 设备)
	// 以 update_state=false 投递按键: wlroots 从不推进它们的 xkb 状态,
	// 没有这一步修饰符掩码会一直为空, 所有 Shift/Alt 组合快捷键都会静默失效.
	// wlr_keyboard 的 modifiers 字段在本监听器返回后立即重算,
	// 所以下一个按键就能看到新的掩码.
	if (!event->update_state && kb->keyboard->xkb_state != NULL) {
		xkb_state_update_key(kb->keyboard->xkb_state, keycode,
			event->state == WL_KEYBOARD_KEY_STATE_PRESSED
				? XKB_KEY_DOWN : XKB_KEY_UP);
	}

	// 先检查合成器快捷键: 输入法持有键盘抓取期间 (光标在文本框、fcitx5/ibus 激活)
	// 它们也必须继续工作 - 否则 Ctrl+Alt+P 之类会被 IM 吞掉, 无法从输入框里
	// 启动 rofi/终端. 路由在这个单一处理器里决定:
	//  - 被抓取的键盘属于 IM: 除非合成器快捷键消费了该键, 否则转发给 IM;
	//  - 其他键盘正常到达聚焦客户端.
	bool handled = false;
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		handled = keyboard_shortcut(server, kb->keyboard, keycode);
	}
	if (ime_keyboard_grabbed(server, kb->keyboard)) {
		if (!handled) {
			ime_forward_key(server, kb->keyboard, event);
		}
		wlr_log(WLR_DEBUG, "input: key from grabbed keyboard, skipping client");
		return;
	}
	if (!handled) {
		wlr_seat_keyboard_notify_key(server->seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_destroy(struct wl_listener *listener, void *data) {
	struct keyboard *kb = wl_container_of(listener, kb, destroy);
	struct server *server = kb->server;
	ime_detach_keyboard(server, kb->keyboard);
	// 摘掉所有监听器, 以便通过 wlr 销毁时的断言
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->destroy.link);
	wl_list_remove(&kb->link);
	free(kb);
}

// 把 seat 键盘移到某个 surface (NULL 清空焦点). 由 toplevel 聚焦和
// layer-shell 键盘交互共用, 让所有焦点切换都走同一条 notify_enter 路径.
void seat_keyboard_focus(struct server *server, struct wlr_surface *surface) {
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (surface != NULL) {
		if (keyboard != NULL) {
			wlr_seat_keyboard_notify_enter(server->seat, surface,
				keyboard->keycodes, keyboard->num_keycodes,
				&keyboard->modifiers);
		} else {
			wlr_seat_keyboard_notify_enter(server->seat, surface,
				NULL, 0, NULL);
		}
	} else {
		wlr_seat_keyboard_clear_focus(server->seat);
	}
}

// ------------------------------------------------------------------
// 设备热插拔
// ------------------------------------------------------------------

void server_new_virtual_pointer(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		new_virtual_pointer);
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	wlr_cursor_attach_input_device(server->cursor,
		&event->new_pointer->pointer.base);
}

// 挂接一个 (真实或虚拟) 键盘: 给它自己的 key/modifiers 监听器,
// 这样每个设备的按键都能被看到; 若还没有 seat 键盘就让它成为 seat 键盘
// (第一个键盘胜出, 输入法的辅助虚拟键盘永远不会劫持 seat)
static void keyboard_attach(struct server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *keyboard = wlr_keyboard_from_input_device(device);
	wlr_log(WLR_DEBUG, "input: keyboard attach %p name='%s' type=%d",
		(void *)keyboard, device->name ? device->name : "(null)",
		device->type);

	struct keyboard *kb = calloc(1, sizeof(*kb));
	if (kb == NULL) {
		return;
	}
	kb->server = server;
	kb->keyboard = keyboard;
	kb->key.notify = keyboard_key;
	wl_signal_add(&keyboard->events.key, &kb->key);
	kb->modifiers.notify = keyboard_modifiers;
	wl_signal_add(&keyboard->events.modifiers, &kb->modifiers);
	kb->destroy.notify = keyboard_destroy;
	wl_signal_add(&device->events.destroy, &kb->destroy);
	wl_list_insert(server->keyboards.prev, &kb->link);

	// 每个键盘都需要 xkb keymap: keyboard_shortcut() 在按键时解引用 xkb_state,
	// 没有 keymap 的键盘会崩溃
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (context != NULL) {
		struct xkb_keymap *keymap = xkb_keymap_new_from_names(context,
			NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (keymap != NULL) {
			wlr_keyboard_set_keymap(keyboard, keymap);
			wlr_keyboard_set_repeat_info(keyboard, 25, 600);
			xkb_keymap_unref(keymap);
		}
		xkb_context_unref(context);
	}

	// 给输入法的抓取一个挂接本键盘的机会: 每个新设备都会被尝试,
	// 这样当 seat 还被 IM 自己的虚拟键盘占着时出现的真实键盘也能连上
	// (fcitx5 开启 PersistentVirtualKeyboard 时会在任何其他键盘之前启动其 vk).
	// ime_attach_keyboard() 负责决定.
	ime_attach_keyboard(server, keyboard);

	// 第一个真实的打字键盘成为 seat 键盘 (这样输入法的辅助虚拟键盘和
	// Power Button 这类特殊按键伪键盘永远不会劫持 seat);
	// 之后的键盘保留自己的监听器, 仍能到达聚焦客户端
	bool first = wlr_seat_get_keyboard(server->seat) == NULL &&
		keyboard_is_typing(device);
	if (first) {
		wlr_seat_set_keyboard(server->seat, keyboard);
		if (server->focused != NULL &&
				server->focused->xdg_toplevel->base != NULL &&
				server->focused->xdg_toplevel->base->surface->mapped) {
			wlr_seat_keyboard_notify_enter(server->seat,
				server->focused->xdg_toplevel->base->surface,
				keyboard->keycodes, keyboard->num_keycodes,
				&keyboard->modifiers);
		}
	}
}

void server_new_virtual_keyboard(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *virtual_keyboard = data;
	keyboard_attach(server, &virtual_keyboard->keyboard.base);
}

void server_new_input(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		keyboard_attach(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		wlr_cursor_attach_input_device(server->cursor, device);
		break;
	default:
		break;
	}
}
