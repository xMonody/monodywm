// config.h - xmonodywm 编译期配置
//
// 所有可调项都在这里: 移动/缩放抓取区、窗口动画、合成器快捷键等.
// 改完数值重新编译即可.

#ifndef XMONODYWM_CONFIG_H
#define XMONODYWM_CONFIG_H

#include <xkbcommon/xkbcommon.h>

#include <wlr/types/wlr_keyboard.h>

//#define CONFIG_CURSOR_THEME    "McMojave-cursors"      // 光标主题
#define CONFIG_CURSOR_THEME    "Adwaita"      // 光标主题
#define CONFIG_TITLEBAR_CURSOR "pointer"      // 悬停标题栏时显示的光标
#define CONFIG_MOVE_CURSOR     "all-scroll"   // 拖动(移动)窗口时显示的光标

#define CONFIG_ROUNDED_RADIUS    8           // 窗口圆角半径 (px)
#define CONFIG_BORDER_WIDTH      2.0           // 窗口边框宽度 (px)
#define CONFIG_BORDER_UNFOCUSED_DRAW 2.0         // 未聚焦窗口是否绘制边框

#define CONFIG_BORDER_FOCUSED    0x7c73b0    // 有焦点边框颜色 0xRRGGBB
#define CONFIG_BORDER_UNFOCUSED  0x7c73b0    // 无焦点边框颜色 0xRRGGBB

#define CONFIG_BORDER_TOP_LEFT   0x7c73b0    // 顶部边框左1/3颜色 (最小化)
#define CONFIG_BORDER_TOP_MID    0xb87898    // 顶部边框中1/3颜色 (最大化)
#define CONFIG_BORDER_TOP_RIGHT  0x7c73b0    // 顶部边框右1/3颜色 (关闭)
#define CONFIG_BORDER_GRADIENT_WIDTH 15       // 顶部边框三段颜色拼接处渐变宽度 (px)

#define CONFIG_FULLSCREEN_BORDER 1              // 全屏窗口是否显示边框 0/1
#define CONFIG_FULLSCREEN_BORDER_COLOR 0xb87898 // 全屏边框颜色 0xRRGGBB

// 窗口阴影 (scenefx 风格高斯柔影, 颜色独立于边框色):
#define CONFIG_SHADOW_BLUR_SIGMA 16.0f
#define CONFIG_SHADOW_COLOR      0x22222f
#define CONFIG_SHADOW_ALPHA      0.4f

#define CONFIG_TITLEBAR_HEIGHT  6           // 移动窗口标题栏范围
#define CONFIG_EDGE_THICKNESS   6           // 调整窗口大小边框范围
#define CONFIG_RESIZE_DRAW_CONTENTS 1        // 1 = 实时 resize 客户端; 0 = 拖动时只画轮廓、松开再应用 (边缘跟手)
#define CONFIG_RESIZE_FINAL_TIMEOUT_MS 500  // outline 模式: 松开后客户端迟迟不提交最终尺寸时的强制结束宽限

#define CONFIG_DOUBLE_CLICK_NS (400 * 1000000L) // 双击窗口判定窗口 (ns)
#define CONFIG_LONG_PRESS_NS (350 * 1000000L) // 标题栏按住这么久即抓取窗口 (ns)
#define CONFIG_DRAG_THRESHOLD 4.0         // 判断是否移动窗口

#define CONFIG_WHEEL_DEBOUNCE_ENABLED true // 控制是否启用鼠标组合手势
#define CONFIG_WHEEL_BURST_NS (800 * 1000000L)   // 一次连续滚动最长算一个动作 (0.8 s)
#define CONFIG_WHEEL_TICK_GAP_NS (300 * 1000000L) // 两次滚轮间隔达到此值即算下一个动作 (0.3 s)

// 新窗口放置 (place.c): 创建时大小完全尊重客户端, 位置在屏幕上左右上下居中.
// 默认以整个屏幕为基准严格居中; 设为 1 时改为以工作区为基准
// (屏幕减去 layer-shell 状态栏的独占区), 保证新窗口不被状态栏盖住.
#define CONFIG_CENTER_AVOID_BARS 0

// 设为 0 则关闭动画, 行为与原来一致: 瞬时切换
#define CONFIG_ANIM_ENABLE   1

#define CONFIG_ANIM_FADE_MS  50      // 创建淡入 / 关闭淡出 时长 (ms)

// 组合键修饰符, 按需组合使用
#define MODKEY0 (WLR_MODIFIER_ALT)                        // alt (单独按 Alt)
#define MODKEY1 (WLR_MODIFIER_LOGO)                        // win
#define MODKEY2 (WLR_MODIFIER_SHIFT | WLR_MODIFIER_ALT)    // shift+alt
#define MODKEY3 (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL)   // shift+ctrl
#define MODKEY4  (WLR_MODIFIER_ALT    | WLR_MODIFIER_CTRL)  // alt+ctrl


// 动作列表
enum config_action {
	CONFIG_ACTION_QUIT,          // 退出合成器
	CONFIG_ACTION_MAXIMIZE,      // 最大化 / 还原
	CONFIG_ACTION_MINIMIZE,      // 最小化
	CONFIG_ACTION_NEXT_WINDOW,   // 切换到下一个窗口
	CONFIG_ACTION_PREV_WINDOW,   // 切换到上一个窗口
	CONFIG_ACTION_CLOSE,         // 关闭当前窗口
	CONFIG_ACTION_CLOSE_OTHER,   // 关闭其他窗口 (保留当前聚焦窗口)
	CONFIG_ACTION_TASK,          // 切换到第 N 个窗口 (N = 数字键 1-9)
};

// 组合快捷键: mods + key 触发 action.
// 同一个 action 可以绑定多个快捷键, 直接在数组里加一行即可.
struct config_action_shortcut {
	enum config_action action;
	uint32_t mods;
	xkb_keysym_t key;  // keysyms 见 /usr/include/xkbcommon/xkbcommon-keysyms.h
};

// MODKEY0(alt) MODKEY1(win) MODKEY2(shift+alt) MODKEY3(shift+ctrl) MODKEY4(ctrl+alt)
static const struct config_action_shortcut config_action_shortcuts[] = {
	//{ CONFIG_ACTION_QUIT,        MODKEY1, XKB_KEY_q },       // win+q 退出合成器
	//{ CONFIG_ACTION_NEXT_WINDOW, MODKEY1, XKB_KEY_n },       // win+n
	//{ CONFIG_ACTION_PREV_WINDOW, MODKEY1, XKB_KEY_p },       // win+p
	//{ CONFIG_ACTION_CLOSE,       MODKEY1, XKB_KEY_c },       // win+c
	//{ CONFIG_ACTION_MINIMIZE,    MODKEY1, XKB_KEY_m },       // win+m
	//{ CONFIG_ACTION_CLOSE_OTHER, MODKEY1, XKB_KEY_q },       // win+q

	{ CONFIG_ACTION_MAXIMIZE,    MODKEY1, XKB_KEY_Return },  // win+Enter
	{ CONFIG_ACTION_MAXIMIZE,    MODKEY2, XKB_KEY_Return },  // shift+alt+Enter
	{ CONFIG_ACTION_MAXIMIZE,    MODKEY4, XKB_KEY_Return },  // ctrl+alt+Enter
	{ CONFIG_ACTION_QUIT,        MODKEY3, XKB_KEY_q },       // shift+ctrl+q 退出合成器
	{ CONFIG_ACTION_NEXT_WINDOW, MODKEY4, XKB_KEY_n },       // ctrl+alt+n
	{ CONFIG_ACTION_PREV_WINDOW, MODKEY4, XKB_KEY_p },       // ctrl+alt+p
	{ CONFIG_ACTION_CLOSE,       MODKEY4, XKB_KEY_c },       // ctrl+alt+c
	{ CONFIG_ACTION_MINIMIZE,    MODKEY2, XKB_KEY_m },       // shift+alt+m
	{ CONFIG_ACTION_CLOSE_OTHER, MODKEY4, XKB_KEY_q },       // ctrl+alt+q

	{ CONFIG_ACTION_TASK,        MODKEY1, XKB_KEY_1 },       // win+1..9 切换窗口
	{ CONFIG_ACTION_TASK,        MODKEY1, XKB_KEY_2 },
	{ CONFIG_ACTION_TASK,        MODKEY1, XKB_KEY_3 },
	{ CONFIG_ACTION_TASK,        MODKEY1, XKB_KEY_4 },
	{ CONFIG_ACTION_TASK,        MODKEY1, XKB_KEY_5 },
	{ CONFIG_ACTION_TASK,        MODKEY1, XKB_KEY_6 },
	{ CONFIG_ACTION_TASK,        MODKEY1, XKB_KEY_7 },
	{ CONFIG_ACTION_TASK,        MODKEY1, XKB_KEY_8 },
	{ CONFIG_ACTION_TASK,        MODKEY1, XKB_KEY_9 },
};

// 应用启动快捷键: mods + key 启动 app, args 可为 NULL 或空串
struct config_app_shortcut {
	uint32_t mods;
	xkb_keysym_t key;
	const char *app;
	const char *args;
};

// MODKEY0(alt) MODKEY1(win) MODKEY2(shift+alt) MODKEY3(shift+ctrl) MODKEY4(ctrl+alt)
static const struct config_app_shortcut config_app_shortcuts[] = {
	{ MODKEY1, XKB_KEY_t, "foot",    NULL },
	{ MODKEY1, XKB_KEY_f, "firefox", NULL },
	{ MODKEY1, XKB_KEY_w, "wezterm", NULL },
	{ MODKEY1, XKB_KEY_k, "kitty",   NULL },
	{ MODKEY1, XKB_KEY_q, "qq",      NULL },

	{ MODKEY1, XKB_KEY_s, "rofi -show drun", NULL },
	{ MODKEY1, XKB_KEY_p, "rofi -show drun", NULL },
};

// 强制「无装饰」窗口 (合成器接管边框): 这些客户端自己画 CSD, 但它们的
// 边缘调整依赖合成器, 所以由合成器提供 resize 边缘 + resize 光标, 并忽略
// 它们自己的 resize 光标形状.
// 匹配是大小写不敏感的 app_id 前缀匹配 (xdg_toplevel.set_app_id), 所以
// 一条前缀能覆盖多个变体 (如 "org.chromium" 覆盖 org.chromium.Chromium).
// 以后装了新应用 (例如 VS Code) 直接在这里加一行前缀即可, 不用改 toplevel.c.
static const char *const config_force_undecorated[] = {
	"qq",
	"chromium",
	"google-chrome",
	"firefox",
	"org.chromium",
	"org.mozilla",
	"code",                 // VS Code / VS Codium
    "clash-verge",
    "cn.MoeKoe.Music",
	NULL,
};

#endif // XMONODYWM_CONFIG_H
