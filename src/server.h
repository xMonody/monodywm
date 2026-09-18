// server.h - xmonodywm 的共享类型与跨模块声明
//
// 合成器拆成多个小模块 (main、ipc、scene、border、toplevel、layer、
// output、input、pointer); 它们需要共享的东西都放在这里.

#ifndef XMONODYWM_SERVER_H
#define XMONODYWM_SERVER_H

#define _POSIX_C_SOURCE 200809L

// 所有可调项 (快捷键、边缘抓取区)
#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_dialog_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

// 场景树顺序 (自下而上)
enum scene_layer {
	LAYER_BACKGROUND = 0, // wlr-layer-shell 背景
	LAYER_BOTTOM,         // wlr-layer-shell bottom
	LAYER_TOPLEVELS,      // 普通窗口
	LAYER_TOP,            // wlr-layer-shell top
	LAYER_OVERLAY,        // wlr-layer-shell overlay + 拖拽图标
	LAYER_COUNT,
};

// 存在 wlr_scene_node.data 里的标签, 便于从任意命中测试到的场景节点
// 向上遍历父节点找到所属对象
enum scene_tag_type {
	TAG_TOPLEVEL,
	TAG_POPUP,
	TAG_LAYER,
};

struct scene_tag {
	enum scene_tag_type type;
	void *ptr;

	struct wl_listener destroy; // 节点销毁时释放该标签
};

#include <wlr/types/wlr_scene.h>

struct ipc_client;     // 定义在 ipc.c
struct wlr_swapchain;  // 定义在 wlr/render/swapchain.h
struct server;
struct toplevel;
struct toplevel_anim;  // 定义在 animate.c
struct layer_surface;
struct rounded_cache;  // 定义在 rounded.c

// 一个已连接的输入法 (fcitx5/ibus); 只使用第一个
struct ime {
	struct server *server;
	struct wl_list link; // server.imes

	struct wlr_input_method_v2 *input_method;
	struct wl_listener destroy;
	struct wl_listener grab_keyboard;
	struct wl_listener commit;
	struct wl_listener keyboard_grab_destroy;
	struct wl_listener new_popup_surface;

	// IM 持有键盘抓取期间的 seat 键盘. 按键由 input.c 通过 ime_forward_key() 转发,
	// 以便先检查合成器快捷键; 只有修饰符通过监听器同步.
	struct wlr_keyboard *keyboard;
	struct wl_listener keyboard_grab_modifiers;
	bool keyboard_grab_destroy_added; // 抓取 destroy 监听器已挂接

	// 已转发给 IM 抓取的按下键码; 用于抑制没有匹配按下的杂散释放事件
	uint32_t forwarded_keys[16];
	size_t forwarded_key_count;

	// IM (fcitx5) 显示的候选窗
	struct wlr_input_popup_surface_v2 *popup_surface;
	struct wlr_scene_surface *popup_scene_surface;
	struct wl_listener popup_commit;
	struct wl_listener popup_destroy;
};

// 客户端的一个 zwp_text_input_v3 对象 (通常每客户端一个)
struct text_input {
	struct server *server;
	struct wl_list link; // server.text_inputs

	struct wlr_text_input_v3 *text_input;
	// 合成器侧缓存的、该 text input 聚焦的 surface.
	// wlr_text_input_v3_send_enter()/send_leave() 维护 wlroots 内部的
	// text_input->focused_surface; 这里保留自己的副本, 便于在 surface 销毁时
	// 不依赖 wlroots 的字段就能决定焦点切换.
	struct wlr_surface *focused_surface;
	struct wl_listener destroy;
	struct wl_listener enable;
	struct wl_listener disable;
	struct wl_listener commit;
};

// toplevel 内容树下的一个 xdg popup (菜单/工具提示/下拉框);
// 用 wlr_scene_xdg_surface_create 渲染, 从而处理 subsurface,
// 并让嵌套 popup (Qt 子菜单) 递归挂接. 结构体生命周期绑定到 pp->tree:
// popup 的 xdg surface 销毁 (或 toplevel 的树销毁) 时 wlroots 会销毁该树,
// 然后 tree_destroy 释放 pp - 所以 popup 不会比 toplevel 活得久.
struct toplevel_popup {
	struct toplevel *tl;
	struct wlr_xdg_popup *popup;
	struct wlr_scene_tree *tree;
	struct wl_listener tree_destroy;  // 树销毁时释放 pp
	struct wl_listener commit;
	struct wl_listener new_popup;
	struct wl_listener destroy;
};

// toplevel surface 的一个 subsurface (Firefox/Chromium 用 subsurface 覆盖
// 窗口边缘); 它的 commit 把圆角 FBO 缓存标记为脏, 内容变化时重绘离屏副本
struct toplevel_subsurface {
	struct toplevel *tl;
	struct wlr_subsurface *subsurface;
	struct wl_list link; // tl->subsurfaces
	struct wl_listener commit;
	struct wl_listener new_subsurface; // 嵌套 subsurface
	struct wl_listener destroy;

	// 自上次 FBO 渲染以来累积的 buffer damage, 采用 surface 局部坐标
	// (rounded.c 在渲染时, 即最终布局位置已知时, 映射到 FBO 空间)
	pixman_region32_t damage;
	// 上次 commit 时的几何 (相对父 surface 的位置 + surface 局部尺寸),
	// 这样移动/缩放时也能损坏旧区域
	int prev_x, prev_y, prev_w, prev_h;
};

struct toplevel {
	struct server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	struct wlr_foreign_toplevel_handle_v1 *fthandle;
	struct wlr_output *last_output;

	// 离屏圆角 FBO 缓存 (rounded.c); 禁用时为 NULL
	struct rounded_cache *rounded;

	// 窗口 surface 的 subsurface (它们的 commit 标记圆角缓存脏);
	// 随 toplevel 一起清理
	struct wl_list subsurfaces;      // struct toplevel_subsurface.link

	// popup 是 scene_tree 的子场景树, 其树销毁时自行清理, 无需显式列表

	// xdg-decoration
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	enum wlr_xdg_toplevel_decoration_v1_mode decoration_mode;
	bool decoration_configured;

	bool minimized;
	// 已请求关闭, 关闭淡出 (animate.c) 接管 wlr_xdg_toplevel 的关闭请求,
	// 窗口不可见后再发送. 在真正消失前窗口是惰性的: 不可聚焦、不可提升、
	// 排除在窗口循环之外, 且拒绝最小化/最大化. 淡出结束时其场景节点被禁用,
	// 这样不理会关闭请求的客户端也不会留下一个不可见却阻塞输入的窗口.
	// 只在 surface 销毁时清除 (关闭期间重新映射会被再次关闭, 绝不重新显示).
	bool closing;
	bool positioned; // 初始位置已分配

	// 每窗口动画状态 (animate.c); 场景树销毁时释放
	struct toplevel_anim *anim;

	// 已发目标尺寸 configure 但客户端 buffer 还没到位: 节点停在原位,
	// 等提交到目标尺寸再落框 (否则旧 buffer 会先闪到作区/输出左上角)
	bool pending_frame;
	struct wlr_box pending_frame_box;

	// 自动居中 (place.c): 新窗口在 surface 尺寸变化时重新居中,
	// 直到用户与之交互 (移动/缩放/最大化/全屏会置 user_moved 并停止).
	// Electron 窗口 (QQ) 常先映射一个小的占位 surface, 稍后才提交真实尺寸.
	bool user_moved;
	bool placed;   // 居中放置已执行; placed_w/h = 当时使用的尺寸
	int placed_w, placed_h;

	// 拖动取消最大化时要还原的几何 (Windows 风格)
	struct wlr_box restore_box;
	bool has_restore_box;

	// 离开全屏时还原的几何; 与 restore_box 分开, 这样从最大化进入全屏
	// 不会覆盖最大化保存的浮动几何
	struct wlr_box fullscreen_restore_box;
	bool has_fullscreen_restore_box;

	// 全屏状态 (跟踪 current.fullscreen, 它只在 ack 时更新)
	bool fullscreen;

	// 通过 IPC 套接字暴露给状态栏的 id
	int id;
	bool ipc_added; // 已发出 window_added
	bool transient_seen; // 曾是对话框/瞬态窗口 (父窗口消失后仍不占任务栏)
	char *app_id;   // 缓存的 app_id (跨拆除保留, 供 window_removed 使用)
	pid_t pid;      // 客户端进程 id (便于状态栏匹配托盘项)
	int after_id;   // 本窗口由哪个 id 的窗口启动 (0 = 无); 关闭时焦点回到那里; 优先同进程的兄弟窗口而非终端

	struct wl_list link; // server.toplevels

	struct wl_listener destroy;
	struct wl_listener toplevel_destroy;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener request_maximize;
	struct wl_listener request_minimize;
	struct wl_listener request_fullscreen;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
	struct wl_listener set_parent;
	struct wl_listener new_popup;
	struct wl_listener new_subsurface;

	struct wl_listener ft_request_maximize;
	struct wl_listener ft_request_minimize;
	struct wl_listener ft_request_activate;
	struct wl_listener ft_request_close;
	struct wl_listener ft_destroy;

	struct wl_listener deco_request_mode;
	struct wl_listener deco_destroy;
};

struct layer_surface {
	struct server *server;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene_layer;

	struct wl_list link; // server.layer_surfaces

	// 上次的独占区签名: 它变化时 (状态栏出现、改尺寸或消失) 作区变化,
	// 已有窗口必须重新排布出独占区
	uint32_t last_anchor;
	int32_t last_zone;
	int32_t last_margin_top, last_margin_bottom;
	int32_t last_margin_left, last_margin_right;
	bool last_mapped;
	bool has_last_state;

	// 键盘焦点: 要求键盘交互的 layer surface (rofi/wofi 启动器覆盖层等)
	// 只要保持映射就持有 seat 键盘. last_keyboard_interactive 记住上次提交的
	// 交互状态, 所以只在 map/交互状态切换时抓取焦点, 之后的提交绝不重复抢占
	// (每秒重绘的状态栏不能抢走键盘).
	enum zwlr_layer_surface_v1_keyboard_interactivity last_keyboard_interactive;
	bool keyboard_focused;

	struct wl_listener destroy;
	struct wl_listener commit;
};

// 顶部标题栏按压臂置的双击动作: 在没有拖动的情况下 (第二次) 点击释放时触发
enum zone_action {
	ZONE_NONE = 0,     // 未臂置动作
	ZONE_MINIMIZE,     // 标题栏左三分之一: 最小化
	ZONE_MAXIMIZE,     // 中间三分之一: 切换最大化/还原
	ZONE_CLOSE,        // 右三分之一: 关闭窗口
};

// 指针交互模式, 镜像 labwc 的 input_mode 状态机:
// PASSTHROUGH = 事件和光标交给聚焦客户端; MOVE/RESIZE = 合成器消费指针事件并接管光标
enum input_mode {
	INPUT_MODE_PASSTHROUGH = 0,
	INPUT_MODE_MOVE,
	INPUT_MODE_RESIZE,
};

// 合成器吞掉的按钮按下 (其释放也被吞掉, 绝不到达客户端); 镜像 labwc 的 bound_buttons
#define BOUND_BUTTONS_MAX 8
struct bound_buttons {
	uint32_t values[BOUND_BUTTONS_MAX];
	int size;
};

struct server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_scene *scene;
	struct wlr_scene_tree *layers[LAYER_COUNT];
	struct wlr_output_layout *output_layout;
	struct wlr_output_manager_v1 *output_manager;
	struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;
	// xdg-dialog-v1: 客户端可显式把 toplevel 标记为对话框 (比 set_parent 可靠)
	struct wlr_xdg_wm_dialog_v1 *xdg_dialog_manager;

	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor_manager;
	struct wl_list keyboards; // struct keyboard.link (每个已连接设备)

	// xdg-activation-v1: 客户端驱动的焦点/激活请求 (防焦点抢夺);
	// request_activate 信号由 toplevel.c 处理.
	// wp_fractional_scale_v1 是配套的分数 surface 缩放 global.
	struct wlr_xdg_activation_v1 *activation;
	struct wl_listener activation_request_activate;
	struct wlr_fractional_scale_manager_v1 *fractional_scale_manager;

	// linux-drm-syncobj-v1: 显式 buffer 同步. 只有渲染器和后端都声明
	// timeline 支持时才创建管理器; DRM fd 来自渲染器, 所以客户端在与
	// 合成器渲染相同的设备上导入 timeline.
	struct wlr_linux_drm_syncobj_manager_v1 *linux_drm_syncobj_manager;

	// cursor-shape-v1: 让客户端选择光标形状, 合成器用自己的主题渲染
	// (大小总能匹配输出缩放, 无需客户端猜测); 处理方式类似 wl_pointer.set_cursor
	struct wlr_cursor_shape_manager_v1 *cursor_shape_manager;
	struct wl_listener cursor_shape_set_shape;

	// ext-data-control-v1: 面向 wl-clipboard (wl-copy/wl-paste) 和剪贴板管理器
	// 的剪贴板/选择区特权控制, vim 这类终端编辑器就是这样访问 Wayland 剪贴板的.
	struct wlr_ext_data_control_manager_v1 *ext_data_control_manager;

	// wlr-data-control-unstable-v1: 旧的剪贴板控制协议, CopyQ 和较旧的
	// 剪贴板工具仍只绑定它, 而不绑定 ext- 版本.
	struct wlr_data_control_manager_v1 *data_control_manager;

	// pointer-constraints-v1 + relative-pointer-v1: 客户端 (QEMU/GTK 捕获鼠标,
	// 游戏 mouselook) 把指针锁定/限定到自己的 surface; relative-pointer 提供
	// 原始增量, 使锁定的指针仍能移动客户端自己的光标. 同一时刻只有一个约束活动 -
	// 指针聚焦 surface 上的那个.
	struct wlr_pointer_constraints_v1 *pointer_constraints;
	struct wlr_relative_pointer_manager_v1 *relative_pointer_manager;
	struct wl_listener new_pointer_constraint;
	struct wl_listener new_shortcuts_inhibitor;
	struct wl_listener pointer_focus_change;
	// 监视活动约束 surface 的 commit: 光标提示在锁定请求后的那次 commit
	// 才进入 `current`
	struct wl_listener constraint_commit;
	struct wlr_pointer_constraint_v1 *active_constraint;

	// keyboard-shortcuts-inhibit-v1: 客户端 (GTK4 应用、远程桌面、
	// 虚拟机界面) 在被聚焦时要求合成器停止处理自己的快捷键.
	struct wlr_keyboard_shortcuts_inhibit_manager_v1 *shortcuts_inhibit;

	// primary-selection-v1: 中键粘贴 (PRIMARY 选择区).
	// 下面的 seat 请求监听器驱动 wlr_seat_set_primary_selection().
	struct wlr_primary_selection_v1_device_manager *primary_selection_manager;


	struct wl_list toplevels;      // struct toplevel.link
	struct wl_list layer_surfaces; // struct layer_surface.link
	struct toplevel *focused;
	struct layer_surface *layer_focused; // 当前持有 seat 键盘的 layer surface
					       // (rofi 覆盖层等); toplevel 持有键盘时为 NULL

	// 输入法中继 (fcitx5 / ibus)
	struct wl_list imes;            // struct ime.link
	struct wl_list text_inputs;     // struct text_input.link
	struct wlr_input_method_v2 *input_method;     // 活动输入法
	struct wlr_text_input_v3 *focused_text_input; // 聚焦 surface 上已启用的
						      // text input, 即 IM 通信对象 (无则为 NULL)
	struct wlr_surface *ime_focused_surface; // 拥有 text input 的 surface
	struct wl_listener ime_focused_surface_destroy;
	// 状态栏用的 IPC 套接字 (JSON 事件)
	int ipc_fd;
	struct wl_event_source *ipc_source;
	struct wl_list ipc_clients; // ipc_client.link
	uint32_t next_window_id;

	struct wlr_scene_tree *drag_tree;
	// 拖拽图标的场景树被 wlroots 在其图标销毁时一并销毁; 监听它以便
	// 及时把 drag_tree 置空, 避免指针移动访问已释放的节点
	struct wl_listener drag_tree_destroy;

	// 指针交互状态
	struct toplevel *zone_toplevel; // 活动标题栏按压下的 toplevel
	bool zone_press;                // 标题栏区的按压已被吞掉
	bool right_button_held;         // 右键当前按下
	bool left_button_held;          // 左键当前按下
	// 和弦手势: 按住一个按钮再按另一个 (双击另一个 = 切换最大化/还原,
	// 按住另一个 = 移动光标下的窗口; 释放时恢复光标样式)
	bool chord_active;               // 和弦手势进行中
	uint32_t chord_button;           // 第二个按下的按钮 (触发键)
	bool chord_pending;              // 触发键首次按下: 区分双击还是按住
	bool chord_moving;               // 按住已变成窗口移动
	struct toplevel *chord_toplevel; // 和弦作用的窗口
	bool chord_swallow_left;         // 左键按下被和弦消费: 其释放也要吞掉
	bool chord_swallow_right;        // 右键同理
	struct wl_event_source *chord_timer; // 双击与按住的判定定时器
	bool moving;                    // 窗口移动进行中
	struct toplevel *move_toplevel;
	enum input_mode input_mode;      // PASSTHROUGH / MOVE / RESIZE
	struct bound_buttons bound_buttons; // 被合成器吞掉的按压
	double grab_x, grab_y; // 光标相对窗口原点的偏移
	// 移动开始时的光标位置: 最大化窗口在拖动中途被还原后用它重新锚定抓取
	// (客户端自己的标题栏在普通点击时也会发 xdg_toplevel.move,
	// 所以延迟到用户真正拖动时才还原)
	double move_ref_x, move_ref_y;
	double move_max_w, move_max_h; // 移动开始时的最大化尺寸: 按压偏移按比例映射到还原框
	bool move_deferred_restore;    // 来自最大化窗口的 xdg_toplevel.move: 首次移动时还原并按比例重锚 (zone/和弦拖动已在 begin_move 前重锚)
	double press_x, press_y;
	bool dragged;       // 按压期间移动超过了 CONFIG_DRAG_THRESHOLD
	enum zone_action zone_action; // 标题栏上臂置的双击动作, 释放时执行;
	                                // 没有双击进行中时为 ZONE_NONE
	struct wl_event_source *zone_timer; // 标题栏按住这么久就抓取窗口移动
	uint32_t zone_button;         // 标题栏区里按住的按钮
	struct timespec last_release_time;
	struct timespec wheel_burst_start; // 当前滚轮突发的首个 tick:
	                                      // 突发窗口内的 tick 算一个动作
	struct timespec wheel_last_tick;   // 上一个滚轮 tick 的时刻:
	                                      // 相隔 CONFIG_WHEEL_TICK_GAP_NS 即开始新动作
	bool last_was_click;
	uint32_t last_click_button;

	// 边缘缩放状态
	bool resizing;
	struct toplevel *resize_toplevel;
	uint32_t resize_edges;     // enum wlr_edges
	struct wlr_box resize_orig; // 抓取开始时的窗口框
	int resize_last_w, resize_last_h; // 最后发给客户端的尺寸

	// 缩放轮廓 (CONFIG_RESIZE_DRAW_CONTENTS=0): 拖动时在场景上画一个目标框,
	// 而不是实时缩放客户端, 使拖动的边缘像窗口移动一样 1:1 跟随光标
	struct wlr_scene_tree *resize_outline;
	struct wlr_scene_rect *resize_outline_edges[4];
	struct wlr_box resize_target; // 轮廓模式下的目标框
	bool resize_final_pending;    // 轮廓模式: 等待最终提交
	struct wl_event_source *resize_final_timer; // 轮廓模式: 客户端始终不提交时的看门狗

	// animate.c: 全局节拍看门狗, 有任何窗口动画运行时臂置
	// (动画状态本身在 output.c 的 frame 处理器里逐渲染帧推进, 与 vsync 同步 -
	// 这个定时器只在场景 damage 无法驱动帧流时兜底, 并执行墙钟超时).
	// 惰性创建, 空闲时解除.
	struct wl_event_source *anim_timer;

	// 当前合成器驱动的光标名, 显示客户端光标时为 NULL (用于避免重复更新)
	const char *cursor_override;

	// 聚焦客户端最后设置的光标图像 (wl_pointer.set_cursor 或
	// wp_cursor_shape_device_v1.set_shape); 合成器光标覆盖结束时由 pointer.c 恢复,
	// 避免光标卡在 resize/move 状态. client_cursor_shape 是与 xcursor 主题无关的
	// 形状枚举 (0 = 无); 形状由合成器渲染, surface 由客户端渲染.
	// 两者都设置时 (混用协议的客户端) 形状优先: 它总能按正确缩放渲染.
	struct wlr_surface *client_cursor_surface;
	int client_cursor_hotspot_x, client_cursor_hotspot_y;
	enum wp_cursor_shape_device_v1_shape client_cursor_shape;
	// 拥有 client_cursor_shape 的 seat client (0 = 无): 只在该客户端聚焦时
	// 才恢复形状, 避免显示上一个客户端的过期形状
	struct wlr_seat_client *client_cursor_shape_client;
	struct wl_listener client_cursor_shape_client_destroy;
	struct wl_listener client_cursor_destroy;

	struct wl_listener new_output;
	struct wl_listener new_input;
	struct wl_listener new_virtual_pointer;
	struct wl_listener new_virtual_keyboard;
	struct wl_listener layout_change;
	struct wl_listener output_manager_apply;
	struct wl_listener output_manager_test;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_dialog;
	struct wl_listener new_layer_surface;
	struct wl_listener new_decoration;
	struct wl_listener new_ime;
	struct wl_listener new_text_input;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener seat_request_set_cursor;
	struct wl_listener seat_request_set_selection;
	struct wl_listener seat_request_set_primary_selection;
	struct wl_listener seat_request_start_drag;
	struct wl_listener seat_start_drag;
};

// ---- main.c ----
void spawn_command(const char *cmd);

// ---- scene.c: 场景图打标签 / 命中测试 ----
void xdg_surface_tag(struct wlr_scene_tree *tree, enum scene_tag_type type,
	void *ptr);
void *scene_tag_at(struct server *server, enum scene_tag_type type,
	double lx, double ly);
struct toplevel *toplevel_at(struct server *server);
// 光标位于 popup surface (菜单/下拉框/工具提示) 上: popup 在合成器边框
// (resize 边缘、标题栏) 之前赢得指针
bool pointer_over_popup(struct server *server);
// 光标位于 layer-shell surface (状态栏/菜单覆盖层) 上: 与 popup 一样赢得指针 -
// 光标在其上时不启动窗口移动/缩放抓取, 点击到达 layer surface
bool pointer_over_layer_surface(struct server *server);
// 对话框/瞬态窗口 (通过 xdg_toplevel.set_parent 声明父 toplevel, GTK/Qt 对话框会这样):
// 不可缩放、不可最大化/最小化, 其顶部边框是一个关闭按钮
bool toplevel_is_dialog(struct toplevel *tl);
// 固定尺寸窗口 (min == max): 不能缩放/最大化/最小化
bool toplevel_is_fixed_size(struct toplevel *tl);
// 状态栏可见的"归属"窗口: 对话框沿 parent 链归到主窗口; 无归属时返回 NULL
struct toplevel *toplevel_ipc_owner(struct toplevel *tl);

// ---- toplevel.c: xdg-shell 窗口、窗口状态、装饰 ----
void toplevel_box(struct toplevel *tl, struct wlr_box *box);
void toplevel_frame_box(struct server *server, struct toplevel *tl,
	struct wlr_box *box);
struct wlr_output *toplevel_output(struct server *server,
	struct toplevel *tl);
struct toplevel *neighbor_toplevel(struct server *server,
	struct toplevel *tl, bool next, bool include_minimized);
struct toplevel *toplevel_by_id(struct server *server, int id);
void close_toplevel(struct toplevel *tl);
void set_fullscreen(struct server *server, struct toplevel *tl,
	bool fullscreen);
void set_maximized(struct server *server, struct toplevel *tl,
	bool maximized);
void restore_maximized_toplevel(struct toplevel *tl);
void set_minimized(struct server *server, struct toplevel *tl,
	bool minimized);
void focus_toplevel(struct server *server, struct toplevel *tl);
void xdg_activation_request_activate(struct wl_listener *listener, void *data);
void update_toplevel_output(struct server *server, struct toplevel *tl);
void arrange_toplevels_work_area(struct server *server,
	struct wlr_output *output);
// 某个输出的最大化窗口几何 (见 toplevel.c): 供还原/拖动钳制使用,
// 使其落在与最大化框相同的像素上
void maximized_box(struct server *server, struct wlr_output *output,
	struct wlr_box *box);
void server_new_toplevel(struct wl_listener *listener, void *data);
void server_new_xdg_dialog(struct wl_listener *listener, void *data);
void server_new_decoration(struct wl_listener *listener, void *data);

// ---- rounded.c: 离屏圆角 FBO 缓存 ----
struct rounded_cache *rounded_cache_create(struct server *server,
	struct toplevel *tl);
void rounded_cache_destroy(struct rounded_cache *rc);
void rounded_cache_dirty(struct toplevel *tl);
void rounded_cache_dirty_content(struct toplevel *tl);
void rounded_cache_dirty_mask(struct toplevel *tl);
void rounded_cache_content_commit(struct toplevel *tl);
void rounded_cache_subsurface_commit(struct toplevel *tl,
	struct toplevel_subsurface *ts);
void rounded_cache_hide_content(struct toplevel *tl);
// 整体淡入淡出窗口 (animate.c): 通常只有圆角 FBO 节点可见
// (原始客户端内容以透明度 0 隐藏), 所以动画作用于该节点;
// 在首次 FBO 发布之前 / 关闭圆角时, 改为淡出窗口树下的每个场景 buffer
void rounded_window_set_opacity(struct toplevel *tl, float opacity);
void rounded_render_all(struct server *server);

// ---- border.c: 窗口边框宽度与依赖焦点的颜色 ----
float border_width(struct toplevel *tl);
float border_gradient_width(struct toplevel *tl);
struct wlr_render_color border_color(struct server *server,
	struct toplevel *tl);
void border_top_colors(struct toplevel *tl,
	struct wlr_render_color *left, struct wlr_render_color *mid,
	struct wlr_render_color *right);
void border_focus_changed(struct toplevel *tl, struct toplevel *prev);

// ---- shadow.c: 窗口阴影宽度/透明度策略 ----
float shadow_sigma(struct toplevel *tl);
float shadow_alpha(void);
struct wlr_render_color shadow_color(void);
int shadow_padding(void);

// ---- place.c: 初始窗口放置 ----
// 尺寸完全由客户端决定, 位置在输出 (屏幕) 上居中
bool place_toplevel(struct server *server, struct toplevel *tl);

// ---- animate.c: 窗口动画 (仅淡入淡出) ----
// 返回 false 时调用方必须瞬时应用状态变化.
//   fade_in:  新窗口 0 -> 1
//   close:    1 -> 0, 不可见后再发 xdg close (期间返回 true, 调用方不要自行 close)
//   cancel:   窗口提前 unmaps, 停止动画并恢复可见状态
bool animate_toplevel_fade_in(struct server *server, struct toplevel *tl);
bool animate_toplevel_close(struct toplevel *tl);
void animate_toplevel_cancel(struct toplevel *tl);
// 推进所有动画到给定时刻; 由输出 frame 处理器在场景渲染前调用 (output.c)
void anim_frame_tick(struct server *server, uint32_t now_ms);

// ---- layer.c: wlr-layer-shell + 作区 ----
void get_work_area(struct server *server, struct wlr_output *output,
	struct wlr_box *area);
void server_new_layer_surface(struct wl_listener *listener, void *data);
// 释放交互 layer surface 的键盘持有 (若有), 并在同一步把 seat 键盘交给 `surface`
// (NULL 清空)
void layer_keyboard_clear(struct server *server, struct wlr_surface *surface);

// ---- output.c: 显示器 + 输出管理 ----
void server_new_output(struct wl_listener *listener, void *data);
void server_layout_change(struct wl_listener *listener, void *data);
void output_manager_apply(struct wl_listener *listener, void *data);
void output_manager_test(struct wl_listener *listener, void *data);

// ---- input.c: seat、键盘、快捷键 ----
void focus_window(struct server *server, struct toplevel *tl);
void seat_request_set_cursor(struct wl_listener *listener, void *data);
void seat_request_set_shape(struct wl_listener *listener, void *data);
void seat_request_set_selection(struct wl_listener *listener, void *data);
void seat_request_set_primary_selection(struct wl_listener *listener,
	void *data);
void new_shortcuts_inhibitor(struct wl_listener *listener, void *data);
void seat_request_start_drag(struct wl_listener *listener, void *data);
void seat_start_drag(struct wl_listener *listener, void *data);
void server_new_virtual_pointer(struct wl_listener *listener, void *data);
void server_new_virtual_keyboard(struct wl_listener *listener, void *data);
void server_new_input(struct wl_listener *listener, void *data);

// 一个已连接的键盘设备 (真实或虚拟): 保留每设备的 key/modifiers 监听器,
// 并持有 IM 抓取的按键转发状态
struct keyboard {
	struct server *server;
	struct wlr_keyboard *keyboard;
	struct wl_list link; // server.keyboards

	struct wl_listener key;
	struct wl_listener modifiers;
	struct wl_listener destroy;
};

// ---- input.c: seat、键盘焦点、快捷键 ----
void server_new_virtual_keyboard(struct wl_listener *listener, void *data);
bool keyboard_is_typing(struct wlr_input_device *device);
// 把 seat 键盘移到某个 surface (NULL 清空焦点)
void seat_keyboard_focus(struct server *server, struct wlr_surface *surface);

// ---- ime.c: 输入法中继 (fcitx5 / ibus) ----
void ime_set_focus(struct server *server, struct wlr_surface *surface);
void ime_attach_keyboard(struct server *server,
	struct wlr_keyboard *keyboard);
void ime_detach_keyboard(struct server *server,
	struct wlr_keyboard *keyboard);
// 输入法的键盘抓取是否连到这个键盘
bool ime_keyboard_grabbed(struct server *server,
	struct wlr_keyboard *keyboard);
// 把按键事件转发给抓取连到本键盘的输入法;
// 由 input.c 的单一按键处理器在检查完合成器快捷键后调用,
// 所以被消费的键根本不会传到这里
void ime_forward_key(struct server *server, struct wlr_keyboard *keyboard,
	struct wlr_keyboard_key_event *event);
void ime_update_popup(struct server *server);
void ime_new_input_method(struct wl_listener *listener, void *data);
void ime_new_text_input(struct wl_listener *listener, void *data);

// ---- pointer.c: 光标交互 (移动 / 缩放 / 手势) ----
void begin_move(struct server *server, struct toplevel *tl,
	double ref_x, double ref_y);
void begin_resize(struct server *server, struct toplevel *tl, uint32_t edges);
void end_move(struct server *server);
void end_resize(struct server *server);
// 结束进行中的和弦手势 (不含 chord_toplevel 的清理; 调用方若持有该窗口
// 的销毁路径需自行置空, 见 toplevel_unfocus)
void end_chord(struct server *server);
// 清空缩放抓取状态但不应用几何 (轮廓模式下最终提交落地时, 以及 toplevel
// 在抓取中途消失时使用)
void resize_grab_clear(struct server *server);
// 光标是否位于任意窗口的合成器边框区 (标题栏/resize 边缘)?
// 那里由合成器接管光标, 所以客户端光标请求被忽略 (input.c) -
// 客户端仍收到 motion 并保留悬停反馈, 只是不能改光标.
bool pointer_over_frame_zone(struct server *server);
void update_cursor_style(struct server *server);
// 重新做一次光标下的命中测试并更新指针焦点/光标样式 (窗口被隐藏/销毁时用)
void refresh_pointer_focus(struct server *server);
// 显示聚焦客户端当前想要的光标 (形状、surface 或默认箭头);
// 合成器光标覆盖结束时使用
void reapply_client_cursor(struct server *server);
void cursor_motion(struct wl_listener *listener, void *data);
void cursor_motion_absolute(struct wl_listener *listener, void *data);
void cursor_button(struct wl_listener *listener, void *data);
void cursor_axis(struct wl_listener *listener, void *data);
void new_pointer_constraint(struct wl_listener *listener, void *data);
void pointer_focus_change(struct wl_listener *listener, void *data);
bool pointer_constraint_active(struct server *server);
void cursor_frame(struct wl_listener *listener, void *data);

#endif // XMONODYWM_SERVER_H
