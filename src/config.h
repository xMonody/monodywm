/*
 * config.h - compile-time configuration for xmonodywm
 *
 * All the tunables live here: the resize/move grab zone and the compositor
 * keyboard shortcuts.  Edit the values and rebuild.
 */

#ifndef XMONODYWM_CONFIG_H
#define XMONODYWM_CONFIG_H

#include <xkbcommon/xkbcommon.h>

#include <wlr/types/wlr_keyboard.h>

//#define CONFIG_CURSOR_THEME    "McMojave-cursors"      /* cursor theme  */
#define CONFIG_CURSOR_THEME    "Adwaita"      /* cursor theme  */
#define CONFIG_TITLEBAR_CURSOR "pointer"      /* cursor shown while hovering the title strip */
#define CONFIG_MOVE_CURSOR     "all-scroll"   /* cursor shown while dragging (moving) a window */

#define CONFIG_ROUNDED_RADIUS    8           /* 窗口圆角半径 (px) */
#define CONFIG_BORDER_WIDTH      2.0           /* 窗口边框宽度 (px) */
#define CONFIG_BORDER_UNFOCUSED_DRAW 2.0         /* 未聚焦窗口是否绘制边框 */

#define CONFIG_BORDER_FOCUSED    0x7c73b0    /* 有焦点边框颜色 0xRRGGBB */
#define CONFIG_BORDER_UNFOCUSED  0x7c73b0    /* 无焦点边框颜色 0xRRGGBB */

#define CONFIG_BORDER_TOP_LEFT   0x7c73b0    /* 顶部边框左1/3颜色 (最小化) */
#define CONFIG_BORDER_TOP_MID    0xb87898    /* 顶部边框中1/3颜色 (最大化) */
#define CONFIG_BORDER_TOP_RIGHT  0x7c73b0    /* 顶部边框右1/3颜色 (关闭) */
#define CONFIG_BORDER_GRADIENT_WIDTH 15       /* 顶部边框三段颜色拼接处渐变宽度 (px) */

#define CONFIG_FULLSCREEN_BORDER 1              /* 全屏窗口是否显示边框 0/1 */
#define CONFIG_FULLSCREEN_BORDER_COLOR 0xb87898 /* 全屏边框颜色 0xRRGGBB */

// 窗口阴影 (scenefx 风格高斯柔影, 颜色独立于边框色):
#define CONFIG_SHADOW_BLUR_SIGMA 20.0f
#define CONFIG_SHADOW_COLOR      0x24283b
#define CONFIG_SHADOW_ALPHA      0.8f

#define CONFIG_TITLEBAR_HEIGHT  6           /* 移动窗口标题栏范围 */
#define CONFIG_EDGE_THICKNESS   6           /* 调整窗口大小边框范围 */
#define CONFIG_RESIZE_DRAW_CONTENTS 1        /* 1 = 实时 resize 客户端; 0 = 拖动时只画轮廓、松开再应用 (边缘跟手) */
#define CONFIG_RESIZE_FINAL_TIMEOUT_MS 500  /* outline 模式: 松开后客户端迟迟不提交最终尺寸时的强制结束宽限 */

#define CONFIG_DOUBLE_CLICK_NS (400 * 1000000L) /* double-click window (ns) */
#define CONFIG_LONG_PRESS_NS (350 * 1000000L) /* holding the strip this long grabs the window (ns) */
#define CONFIG_DRAG_THRESHOLD 4.0         /* 判断是否移动窗口 */

#define CONFIG_WHEEL_DEBOUNCE_ENABLED false /* 控制是否启用骚鼠标 */
#define CONFIG_WHEEL_BURST_NS (800 * 1000000L)   /* one continuous scroll: max length = one action (0.8 s) */
#define CONFIG_WHEEL_TICK_GAP_NS (300 * 1000000L) /* two ticks this far apart = next action (0.3 s) */

/* 新窗口放置 (place.c): 创建时大小完全尊重客户端, 位置在屏幕上左右上下居中.
 * 默认以整个屏幕为基准严格居中; 设为 1 时改为以工作区为基准
 * (屏幕减去 layer-shell 状态栏的独占区), 保证新窗口不被状态栏盖住. */
#define CONFIG_CENTER_AVOID_BARS 0

/* 窗口动画 (animate.c):  设为 0 则关闭所有动画 行为与原来一致: 瞬时切换 */
#define CONFIG_ANIM_ENABLE   1

/* 各动画时长互相独立, 可单独调整, 互不影响: */
#define CONFIG_ANIM_FALL_MS  90      /* 最小化落下 / 从最小化还原(掉回原位) 时长 (ms) */
#define CONFIG_ANIM_FADE_MS  50      /* 创建淡入 / 关闭淡出 时长 (ms) */
#define CONFIG_ANIM_FALL_GAP 10      /* 落出屏幕底边/从窗口上方起跳时保留的间隙 (px) */
/* 最小化落下 / 还原掉回时伴随的缩放 (围绕窗口自身中心):
 *   落下时窗口从 1.0 逐渐缩小到 CONFIG_ANIM_FALL_SCALE;
 *   还原掉回时从 CONFIG_ANIM_FALL_SCALE 逐渐放大回 1.0.
 * 设为 1.0 则关闭缩放, 只保留纯落下/掉回. */
#define CONFIG_ANIM_FALL_SCALE 1.0f /* 落下/掉回缩放到的比例 (1.0 = 不缩放, 直接掉落) */
/* 最大化 / 取消最大化(还原) 的 Windows 式缩放动画:
 *   CONFIG_ANIM_MAXIMIZE_MS = 缩放时长基准 (与上面的 FALL_MS 无关, 是
 *   另一个独立时长项). 大跨度缩放 (小窗口铺满全屏) 会在此基准上自动增加
 *   少许时长 (跨度/12, 上限 2 倍基准) 以保持每帧位移平滑, 增加量有界,
 *   不会脱离这个旋钮的控制.
 *   CONFIG_ANIM_MAXIMIZE_WAIT_MS = 等待客户端把内容重排成目标尺寸的时间
 *   上限: 必须大于几次客户端往返 (约 5 帧以上), 否则稍慢的客户端会在
 *   缩放开始前被强制"直接跳变", 表现为没有动画. */
#define CONFIG_ANIM_MAXIMIZE_MS 70  /* 最大化/还原 缩放时长基准 (ms) */
#define CONFIG_ANIM_MAXIMIZE_WAIT_MS 120 /* 等待目标尺寸内容重排的上限 (ms) */

/* 创建/关闭动画是纯淡入/淡出 (窗口保持自然大小), 缩放效果已移除.
 * 只有最大化/还原保留 Windows 式缩放 (见上方的 MAXIMIZE 注释). */
/* 组合键修饰符, 按需组合使用 */
#define MODKEY0 (WLR_MODIFIER_ALT)                        // alt (单独按 Alt)
#define MODKEY1 (WLR_MODIFIER_LOGO)                        // win
#define MODKEY2 (WLR_MODIFIER_SHIFT | WLR_MODIFIER_ALT)    // shift+alt
#define MODKEY3 (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL)   // shift+ctrl
#define MODKEY4  (WLR_MODIFIER_ALT    | WLR_MODIFIER_CTRL)  // alt+ctrl


/* 动作列表 */
enum config_action {
	CONFIG_ACTION_QUIT,          /* 退出合成器 */
	CONFIG_ACTION_MAXIMIZE,      /* 最大化 / 还原 */
	CONFIG_ACTION_MINIMIZE,      /* 最小化 */
	CONFIG_ACTION_NEXT_WINDOW,   /* 切换到下一个窗口 */
	CONFIG_ACTION_PREV_WINDOW,   /* 切换到上一个窗口 */
	CONFIG_ACTION_CLOSE,         /* 关闭当前窗口 */
	CONFIG_ACTION_CLOSE_OTHER,   /* 关闭其他窗口 (保留当前聚焦窗口) */
	CONFIG_ACTION_TASK,          /* 切换到第 N 个窗口 (N = 数字键 1-9) */
};

/* 组合快捷键: mods + key 触发 action.
 * 同一个 action 可以绑定多个快捷键, 直接在数组里加一行即可. */
struct config_action_shortcut {
	enum config_action action;
	uint32_t mods;
	xkb_keysym_t key;  /* keysyms 见 /usr/include/xkbcommon/xkbcommon-keysyms.h */
};

// MODKEY0(alt) MODKEY1(win) MODKEY2(shift+alt) MODKEY3(shift+ctrl) MODKEY4(ctrl+alt)
static const struct config_action_shortcut config_action_shortcuts[] = {
	//{ CONFIG_ACTION_QUIT,        MODKEY1, XKB_KEY_q },       /* win+q 退出合成器*/
	//{ CONFIG_ACTION_NEXT_WINDOW, MODKEY1, XKB_KEY_n },       /*  win+n */
	//{ CONFIG_ACTION_PREV_WINDOW, MODKEY1, XKB_KEY_p },       /*  win+p */
	//{ CONFIG_ACTION_CLOSE,       MODKEY1, XKB_KEY_c },       /*  win+c */
	//{ CONFIG_ACTION_MINIMIZE,    MODKEY1, XKB_KEY_m },       /*  win+m */
	//{ CONFIG_ACTION_CLOSE_OTHER, MODKEY1, XKB_KEY_q },       /*  win+q */

	{ CONFIG_ACTION_MAXIMIZE,    MODKEY1, XKB_KEY_Return },  /*  win+Enter */
	{ CONFIG_ACTION_MAXIMIZE,    MODKEY2, XKB_KEY_Return },  /*  shift+alt+Enter */
	{ CONFIG_ACTION_MAXIMIZE,    MODKEY4, XKB_KEY_Return },  /*  win+Enter */
	{ CONFIG_ACTION_QUIT,        MODKEY3, XKB_KEY_q },       /*  ctrl+Alt+q 退出合成器*/
	{ CONFIG_ACTION_NEXT_WINDOW, MODKEY4, XKB_KEY_n },       /*  ctrl+Alt+n */
	{ CONFIG_ACTION_PREV_WINDOW, MODKEY4, XKB_KEY_p },       /*  ctrl+Alt+p */
	{ CONFIG_ACTION_CLOSE,       MODKEY4, XKB_KEY_c },       /*  ctrl+Alt+c */
	{ CONFIG_ACTION_MINIMIZE,    MODKEY2, XKB_KEY_m },       /*  ctrl+Alt+m */
	{ CONFIG_ACTION_CLOSE_OTHER, MODKEY4, XKB_KEY_q },       /*  ctrl+Alt+q */

	{ CONFIG_ACTION_TASK,        MODKEY1, XKB_KEY_1 },       /* alt+.. 切换1-9*/
	{ CONFIG_ACTION_TASK,        MODKEY1, XKB_KEY_2 },
	{ CONFIG_ACTION_TASK,        MODKEY1, XKB_KEY_3 },
	{ CONFIG_ACTION_TASK,        MODKEY1, XKB_KEY_4 },
	{ CONFIG_ACTION_TASK,        MODKEY1, XKB_KEY_5 },
	{ CONFIG_ACTION_TASK,        MODKEY1, XKB_KEY_6 },
	{ CONFIG_ACTION_TASK,        MODKEY1, XKB_KEY_7 },
	{ CONFIG_ACTION_TASK,        MODKEY1, XKB_KEY_8 },
	{ CONFIG_ACTION_TASK,        MODKEY1, XKB_KEY_9 },
};

/* 应用启动快捷键: mods + key 启动 app，args 可为 NULL 或空串 */
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

/* 强制「无装饰」窗口（合成器接管边框）：这些客户端自己画 CSD，但它们的
  边缘调整依赖合成器，所以由合成器提供 resize 边缘 + resize 光标，并忽略
  它们自己的 resize 光标形状。
  匹配是大小写不敏感的 app_id 前缀匹配（xdg_toplevel.set_app_id），所以
  一条前缀能覆盖多个变体（如 "org.chromium" 覆盖 org.chromium.Chromium）。
  以后装了新应用（例如 VS Code）直接在这里加一行前缀即可，不用改 toplevel.c。 */
static const char *const config_force_undecorated[] = {
	"qq",
	"chromium",
	"google-chrome",
	"firefox",
	"org.chromium",
	"org.mozilla",
	"code",                 /* VS Code / VS Codium */
    "clash-verge",
    "cn.MoeKoe.Music",
	NULL,
};

#endif /* XMONODYWM_CONFIG_H */
