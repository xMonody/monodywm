// monodywm - 基于 wlroots 0.20 的极简浮动 Wayland 合成器
//
// 入口: 创建 display、backend、renderer 和 scene, 创建所有协议 global
// (由 wlroots 在服务端实现; XML 描述在 protocol/ 目录, 构建时由
// wayland-scanner 生成 .h/.c), 注册各模块监听器并运行事件循环.
//
// 模块:
//   ipc.c      - 状态栏套接字 (JSON 事件)
//   scene.c    - 场景图打标签 / 命中测试
//   toplevel.c - xdg-shell 窗口与窗口状态
//   decor.c    - scenefx 圆角 / 边框 / 阴影 / 模糊
//   place.c    - 新窗口放置 (自动居中)
//   layer.c    - wlr-layer-shell surface + 作区
//   output.c   - 显示器 + wlr-output-management
//   input.c    - seat、键盘、快捷键
//   ime.c      - 输入法中继 (fcitx5 / ibus)
//   pointer.c  - 光标交互 (移动 / 缩放 / 手势)

#include "server.h"

#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wayland-server-core.h>

#include <scenefx/render/fx_renderer/fx_renderer.h>

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>

#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/util/log.h>

// 在独立子进程中运行命令:
//   - setsid(): 新会话, 无控制终端, 不受终端信号 (Ctrl+C、tty 关闭时的 SIGHUP)
//     影响, 合成器终端消失后仍继续运行;
//   - stdin/stdout/stderr -> /dev/null: 不污染合成器 tty, 进程也不会阻塞在终端读写上;
//   - exec 失败时 _exit(127): 绝不会落回合成器代码.
void spawn_command(const char *cmd) {
	if (cmd == NULL || cmd[0] == '\0') {
		return;
	}

	pid_t pid = fork();

	if (pid < 0) {
		wlr_log(WLR_ERROR, "spawn: fork failed for \"%s\": %s", cmd, strerror(errno));
		return;
	}

	if (pid > 0) {
		wlr_log(WLR_DEBUG, "spawn: spawned \"%s\" (pid %ld)", cmd, (long)pid);
		return;
	}

	if (setsid() == -1) {
		_exit(127);
	}

	// 合成器忽略了 SIGPIPE (避免状态栏断开时杀死自己), 但被忽略的信号
	// 会跨 exec 继承. 启动的应用必须恢复默认行为, 否则 shell 管道/CLI
	// 在写已关闭管道时不会按预期退出.
	signal(SIGPIPE, SIG_DFL);

	// 合成器用 signalfd (wl_event_loop_add_signal) 处理 SIGINT/SIGTERM/SIGHUP,
	// 那会把这些信号加入本进程的信号屏蔽字; 屏蔽字跨 fork+exec 继承. 不重置的话
	// 启动的应用会收不到 Ctrl+C / SIGTERM. 子进程必须按默认 (空) 屏蔽字运行.
	sigset_t empty_mask;
	sigemptyset(&empty_mask);
	sigprocmask(SIG_SETMASK, &empty_mask, NULL);

	// 不要把孩子留在合成器的工作目录: 合成器运行期间其 CWD 可能被删除
	// (例如构建目录被删掉重建), 某些应用 - 尤其是会启动 shell 的 foot -
	// 在 CWD 不存在时会卡在映射窗口之前. 固定目录也能避免应用继承奇怪的位置.
	const char *home = getenv("HOME");
	if (home == NULL || home[0] == '\0' || chdir(home) == -1) {
		chdir("/");
	}

	int devnull = open("/dev/null", O_RDWR);

	if (devnull < 0) {
		_exit(127);
	}

	if (dup2(devnull, STDIN_FILENO) == -1) {
		close(devnull);
		_exit(127);
	}

	if (dup2(devnull, STDOUT_FILENO) == -1) {
		close(devnull);
		_exit(127);
	}

	if (dup2(devnull, STDERR_FILENO) == -1) {
		close(devnull);
		_exit(127);
	}

	if (devnull > STDERR_FILENO) {
		close(devnull);
	}

	execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);

	_exit(127);
}

static void reap_children(int sig) {
	(void)sig;

	int saved_errno = errno;
	while (waitpid(-1, NULL, WNOHANG) > 0) {
	}

	errno = saved_errno;
}

// 安装 SIGCHLD 处理器 (main() 中只调用一次)
static void init_reaper(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = reap_children;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigemptyset(&sa.sa_mask);

	if (sigaction(SIGCHLD, &sa, NULL) == -1) {
		wlr_log(WLR_ERROR,
			"init_reaper: failed to install SIGCHLD handler: %s",
			strerror(errno));
	}

	// 绝不能因为向已断开的 IPC 客户端 (退出的状态栏) 写入而死掉:
	// IPC 层会清理该客户端并上报失败, 而不是让 SIGPIPE 杀死整个合成器
	signal(SIGPIPE, SIG_IGN);
}

// SIGINT/SIGTERM/SIGHUP: 经事件循环 (signalfd) 转成 wl_display_terminate,
// 让 wl_display_run() 返回并走 main() 末尾的完整拆除路径. 否则这些信号按
// 默认动作直接杀死进程, 跳过 destroy_clients/destroy_display, 留下
// wayland/IPC 套接字和未释放的协议对象.
static int quit_signal(int sig, void *data) {
	struct wl_display *display = data;
	wlr_log(WLR_INFO, "signal %d received, terminating", sig);
	wl_display_terminate(display);
	return 0;
}

static void run_startup_file(void) {
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");

	char path[4096];

	if (xdg != NULL && xdg[0] != '\0') {
		int ret = snprintf(path, sizeof(path), "%s/monodywm/run", xdg);

		if (ret < 0 || (size_t)ret >= sizeof(path)) {
			wlr_log(WLR_ERROR, "run_startup_file: configuration path is too long");
			return;
		}
	} else if (home != NULL && home[0] != '\0') {
		int ret = snprintf(path, sizeof(path), "%s/.config/monodywm/run", home);
		if (ret < 0 || (size_t)ret >= sizeof(path)) { wlr_log(WLR_ERROR,
				"run_startup_file: configuration path is too long");
			return;
		}
	} else {
		wlr_log(WLR_DEBUG,
			"run_startup_file: HOME and XDG_CONFIG_HOME are not set");
		return;
	}

	FILE *f = fopen(path, "r");

	if (f == NULL) {
		if (errno == ENOENT) {
			wlr_log(WLR_DEBUG, "run_startup_file: no %s, nothing to run", path);
		} else {
			wlr_log(WLR_ERROR, "run_startup_file: failed to open %s: %s", path, strerror(errno));
		}

		return;
	}

	char *line = NULL;
	size_t cap = 0;
	ssize_t len;

	while ((len = getline(&line, &cap, f)) != -1) {
		// 去掉行尾的换行/回车
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			line[--len] = '\0';
		}

		// 跳过前导空白、空行和注释
		char *cmd = line;
		while (*cmd == ' ' || *cmd == '\t') {
			cmd++;
		}
		if (*cmd == '\0' || *cmd == '#') {
			continue;
		}

		wlr_log(WLR_INFO, "run_startup_file: executing: %s", cmd);
		spawn_command(cmd);
	}
	free(line);
	fclose(f);
}

// --- 日志 ---
//
// 默认: INFO 及以上写 stderr. WLR_DEBUG=1 时额外打开一个日志文件并把所有行
// (含 DEBUG) 也写进去 - 自带测试会 grep `WLR_DEBUG=1 monodywm` 的标准输出,
// 所以 stderr 流必须保留; 文件只是方便你自己离线看.
// 若显式指定 MONODYWM_LOG=<路径>, 则只写该文件, stderr 保持干净
// (旧拼写 MONODYWWM_LOG 仍兼容).
// 未指定时 WLR_DEBUG=1 的日志文件为 $XDG_RUNTIME_DIR/monodywm.log (否则 /tmp).
static FILE *log_file = NULL;
static bool log_to_stderr = true;

static void log_write(FILE *out, enum wlr_log_importance importance,
		const char *msg) {
	static const char *const names[] = {
		[WLR_SILENT] = "SILENT",
		[WLR_ERROR] = "ERROR",
		[WLR_INFO] = "INFO",
		[WLR_DEBUG] = "DEBUG",
	};
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	unsigned long sec = (unsigned long)ts.tv_sec;
	fprintf(out, "%02lu:%02lu:%02lu.%03u [%s] %s\n",
		(sec / 3600) % 100, (sec / 60) % 60, sec % 60,
		(unsigned)(ts.tv_nsec / 1000000),
		names[importance] != NULL ? names[importance] : "?", msg);
	fflush(out);
}

static void log_callback(enum wlr_log_importance importance,
		const char *fmt, va_list args) {
	if (importance > wlr_log_get_verbosity()) {
		return;
	}
	static char buf[8192];
	vsnprintf(buf, sizeof(buf), fmt, args);
	if (log_to_stderr) {
		log_write(stderr, importance, buf);
	}
	if (log_file != NULL) {
		log_write(log_file, importance, buf);
	}
}

static void init_logging(void) {
	const char *dbg = getenv("WLR_DEBUG");
	bool debug = dbg != NULL && dbg[0] != '\0';
	const char *path = getenv("MONODYWM_LOG");
	if (path == NULL || path[0] == '\0') {
		// 兼容旧拼写 (多一个 w), 老脚本仍可用
		path = getenv("MONODYWWM_LOG");
	}
	if (path != NULL && path[0] != '\0') {
		log_file = fopen(path, "w");
		if (log_file == NULL) {
			fprintf(stderr, "monodywm: cannot open log %s: %s\n",
				path, strerror(errno));
		} else {
			log_to_stderr = false; // 显式指定文件: 只写文件
		}
	} else if (debug) {
		char fallback[512];
		const char *rt = getenv("XDG_RUNTIME_DIR");
		snprintf(fallback, sizeof(fallback), "%s/monodywm.log",
			rt != NULL && rt[0] != '\0' ? rt : "/tmp");
		log_file = fopen(fallback, "w");
	}
	wlr_log_init(debug ? WLR_DEBUG : WLR_INFO, log_callback);
}

int main(int argc, char *argv[]) {
	// 正常输出只到 stderr; WLR_DEBUG=1 时额外写 $XDG_RUNTIME_DIR/monodywm.log,
	// 显式 MONODYWM_LOG=<path> 则只写该文件
	init_logging();
	// 不要让生成的后台进程变成僵尸
	init_reaper();

	char *startup_cmd = NULL;
	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			fprintf(stderr, "usage: %s [-s startup-command]\n", argv[0]);
			return EXIT_FAILURE;
		}
	}

	struct server server = {0};

	server.display = wl_display_create();
	if (server.display == NULL) {
		return EXIT_FAILURE;
	}
	// 终止信号也走优雅退出: 注册到事件循环而不是用 sigaction. 信号会被
	// 加入本进程的屏蔽字 (spawn_command() 会在 exec 前给子进程清空).
	struct wl_event_loop *event_loop =
		wl_display_get_event_loop(server.display);
	server.sigint_source = wl_event_loop_add_signal(event_loop, SIGINT,
		quit_signal, server.display);
	server.sigterm_source = wl_event_loop_add_signal(event_loop, SIGTERM,
		quit_signal, server.display);
	server.sighup_source = wl_event_loop_add_signal(event_loop, SIGHUP,
		quit_signal, server.display);
	if (server.sigint_source == NULL || server.sigterm_source == NULL ||
			server.sighup_source == NULL) {
		wlr_log(WLR_ERROR,
			"failed to install SIGINT/SIGTERM/SIGHUP handlers");
	}
	server.backend = wlr_backend_autocreate(
		wl_display_get_event_loop(server.display), NULL);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create backend");
		return EXIT_FAILURE;
	}
	server.renderer = fx_renderer_create(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create renderer");
		return EXIT_FAILURE;
	}
	wlr_renderer_init_wl_display(server.renderer, server.display);
	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create allocator");
		return EXIT_FAILURE;
	}
	server.scene = wlr_scene_create();
	if (server.scene == NULL) {
		wlr_log(WLR_ERROR, "failed to create scene");
		return EXIT_FAILURE;
	}
	// 背景模糊强度: 只覆盖 radius / passes, 其余 (noise/亮度/对比度/饱和度)
	// 保持 scenefx 默认值, 这样玻璃颜色仍由窗口自身背景决定.
	wlr_scene_set_blur_radius(server.scene, CONFIG_BLUR_RADIUS);
	wlr_scene_set_blur_num_passes(server.scene, CONFIG_BLUR_PASSES);
	server.output_layout = wlr_output_layout_create(server.display);
	server.output_manager = wlr_output_manager_v1_create(server.display);
	// xdg-output-unstable-v1: 告诉客户端 (grim、xwayland 等) 每个输出的
	// 逻辑几何和名称. grim 在缺少该 global 时不肯信任核心 wl_output 几何,
	// 会退回到猜测, 在本布局上会产生 0x0 的截图.
	wlr_xdg_output_manager_v1_create(server.display, server.output_layout);
	server.foreign_toplevel_manager =
		wlr_foreign_toplevel_manager_v1_create(server.display);
	// xdg-dialog-v1: 客户端把无 parent 的弹窗 (如 QQ "资料卡") 显式标记为对话框
	server.xdg_dialog_manager =
		wlr_xdg_wm_dialog_v1_create(server.display, 1);

	server.seat = wlr_seat_create(server.display, "seat0");
	server.cursor = wlr_cursor_create();
	if (server.cursor == NULL) {
		wlr_log(WLR_ERROR, "failed to create cursor");
		return EXIT_FAILURE;
	}
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	server.xcursor_manager = wlr_xcursor_manager_create(CONFIG_CURSOR_THEME, 24);
	if (server.xcursor_manager == NULL) {
		wlr_log(WLR_ERROR, "failed to create xcursor manager");
		return EXIT_FAILURE;
	}
	// 立即显示默认箭头, 这样首个输出出现时指针就可见 - 否则在第一次
	// 指针移动之前都没有光标图像, 桌面启动时没有光标
	wlr_cursor_set_xcursor(server.cursor, server.xcursor_manager, "left_ptr");

	for (int i = 0; i < LAYER_COUNT; i++) {
		server.layers[i] = wlr_scene_tree_create(&server.scene->tree);
		if (server.layers[i] == NULL) {
			wlr_log(WLR_ERROR, "failed to create scene tree");
			return EXIT_FAILURE;
		}
		// 预模糊缓存树夹在 bottom 层和窗口层之间: 它只能看到 background/bottom,
		// 正好是窗口/面板毛玻璃需要的背景层
		if (i == LAYER_BOTTOM) {
			server.blur_layer = wlr_scene_tree_create(&server.scene->tree);
			if (server.blur_layer == NULL) {
				wlr_log(WLR_ERROR, "failed to create blur tree");
				return EXIT_FAILURE;
			}
		}
	}
	wl_list_init(&server.toplevels);
	wl_list_init(&server.layer_surfaces);
	wl_list_init(&server.layer_focus_stack);
	wl_list_init(&server.monitors);
	wl_list_init(&server.imes);
	wl_list_init(&server.text_inputs);
	wl_list_init(&server.keyboards);
	wl_list_init(&server.ipc_clients);

	// ---- 核心与稳定协议 ----
	// 不显式创建 wl_shm: 上面的 wlr_renderer_init_wl_display() 已经注册了
	// wl_shm global (以及, 在带 DRM fd 且支持 dmabuf 的渲染器上, linux-dmabuf global).
	// 这里再创建会多出一个重复的 wl_shm global.
	wlr_compositor_create(server.display, 6, server.renderer);
	wlr_subcompositor_create(server.display);

	// 这里不再单独创建 linux-dmabuf: wlr_renderer_init_wl_display() 在渲染器
	// 支持 dmabuf 且能拿到 DRM fd 时已注册了 v4 global.
	wlr_data_device_manager_create(server.display);
	// ext-data-control-v1: 面向 wl-clipboard (wl-copy/wl-paste) 和剪贴板管理器等
	// 工具的剪贴板/选择区特权控制, vim 这类编辑器就是这样访问 Wayland 剪贴板的.
	// wlroots 在服务端实现该协议并桥接到 seat 的 selection/primary-selection,
	// 所以注册 global 就够了.
	server.ext_data_control_manager =
		wlr_ext_data_control_manager_v1_create(server.display, 1);
	if (server.ext_data_control_manager == NULL) {
		wlr_log(WLR_ERROR, "failed to create ext-data-control-v1 global");
	}
	// wlr-data-control-unstable-v1: 旧的 wlroots 剪贴板控制协议.
	// CopyQ (以及较旧的 wl-clipboard/独立剪贴板管理器) 仍只绑定这个,
	// 不绑定 ext-data-control-v1, 所以要继续提供, 它们才能访问剪贴板.
	// 与 ext- 版本一样由 wlroots 在服务端实现.
	server.data_control_manager =
		wlr_data_control_manager_v1_create(server.display);
	if (server.data_control_manager == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr-data-control-v1 global");
	}
	// primary-selection-v1: 标准 PRIMARY 选择区, 用于中键粘贴. seat 的请求
	// 监听已驱动 wlr_seat_set_primary_selection(); 没有这个 global 客户端就
	// 无法拥有/提供选择内容, 中键粘贴会失效.
	server.primary_selection_manager =
		wlr_primary_selection_v1_device_manager_create(server.display);
	if (server.primary_selection_manager == NULL) {
		wlr_log(WLR_ERROR, "failed to create primary-selection-v1 global");
	}
	struct wlr_xdg_shell *xdg_shell =
		wlr_xdg_shell_create(server.display, 6);
	wlr_viewporter_create(server.display);
	// wp_single_pixel_buffer_manager_v1: 客户端用一个纯色 buffer 填满
	// surface (solid 背景/遮罩), 而不必为 1x1 像素分配 wl_shm 或 dmabuf.
	// wlroots 在服务端实现整个协议, 这里只注册 global.
	if (wlr_single_pixel_buffer_manager_v1_create(server.display) == NULL) {
		wlr_log(WLR_ERROR,
			"failed to create single-pixel-buffer-v1 global");
	}
	wlr_presentation_create(server.display, server.backend, 2);

	// ---- wlroots 协议 ----
	struct wlr_layer_shell_v1 *layer_shell =
		wlr_layer_shell_v1_create(server.display, 5);
	struct wlr_xdg_decoration_manager_v1 *decoration_manager =
		wlr_xdg_decoration_manager_v1_create(server.display);
	struct wlr_virtual_pointer_manager_v1 *virtual_pointer_manager =
		wlr_virtual_pointer_manager_v1_create(server.display);

	// wlr-screencopy-v1: 为 grim/slurp/wf-recorder 提供屏幕捕获.
	// wlroots 在服务端实现整个协议 (帧捕获、damage、光标叠加), 合成器只注册 global.
	// 选它而不选 ext-image-copy-capture-v1: 这是当前捕获工具所期望的,
	// 且与自带的 DMABUF/SHM 路径兼容.
	struct wlr_screencopy_manager_v1 *screencopy_manager = wlr_screencopy_manager_v1_create(server.display);
	if (screencopy_manager == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr-screencopy-v1 global");
	}

	// cursor-shape-v1: 客户端选择形状, 合成器用自带的 xcursor 主题按输出
	// 的 (分数) 缩放渲染, 所以光标大小总能匹配 - 无需客户端猜测
	server.cursor_shape_manager =
		wlr_cursor_shape_manager_v1_create(server.display, 1);

	// pointer-constraints-v1 + relative-pointer-v1: QEMU (捕获鼠标) 和游戏等
	// 客户端把指针锁定/限定到自己的某个 surface, 并在锁定期间读取原始相对增量.
	// wlroots 在服务端实现两者; pointer.c 负责约束的激活和增量处理,
	// 这里只注册 global.
	server.pointer_constraints =
		wlr_pointer_constraints_v1_create(server.display);
	if (server.pointer_constraints == NULL) {
		wlr_log(WLR_ERROR,
			"failed to create pointer-constraints-v1 global");
	}
	server.relative_pointer_manager =
		wlr_relative_pointer_manager_v1_create(server.display);
	if (server.relative_pointer_manager == NULL) {
		wlr_log(WLR_ERROR, "failed to create relative-pointer-v1 global");
	}

	// keyboard-shortcuts-inhibit-v1: 聚焦的客户端 (GTK4 应用/远程桌面/虚拟机界面)
	// 可以要求合成器停止处理自己的快捷键, 让按键直达客户端.
	server.shortcuts_inhibit =
		wlr_keyboard_shortcuts_inhibit_v1_create(server.display);
	if (server.shortcuts_inhibit == NULL) {
		wlr_log(WLR_ERROR,
			"failed to create keyboard-shortcuts-inhibit-v1 global");
	}

	// xdg-activation-v1: 客户端可以通过激活 token 请求焦点;
	// 合成器在收到请求时聚焦匹配的 toplevel
	server.activation = wlr_xdg_activation_v1_create(server.display);
	if (server.activation == NULL) {
		wlr_log(WLR_ERROR, "failed to create xdg-activation-v1 global");
	}

	// wp_fractional_scale_v1: 告诉 surface 输出的精确分数缩放;
	// wlroots 场景 surface (layer-shell、subsurface、光标、toplevel) 自动处理
	server.fractional_scale_manager =
		wlr_fractional_scale_manager_v1_create(server.display, 1);
	if (server.fractional_scale_manager == NULL) {
		wlr_log(WLR_ERROR, "failed to create fractional-scale-v1 global");
	}

	// ---- 输入法 (fcitx5 / ibus) ----
	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_manager =
		wlr_virtual_keyboard_manager_v1_create(server.display);
	struct wlr_input_method_manager_v2 *input_method_manager =
		wlr_input_method_manager_v2_create(server.display);
	struct wlr_text_input_manager_v3 *text_input_manager =
		wlr_text_input_manager_v3_create(server.display);

	// ---- 监听器 ----
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.new_virtual_pointer.notify = server_new_virtual_pointer;
	wl_signal_add(&virtual_pointer_manager->events.new_virtual_pointer, &server.new_virtual_pointer);
	server.new_virtual_keyboard.notify = server_new_virtual_keyboard;
	wl_signal_add(&virtual_keyboard_manager->events.new_virtual_keyboard, &server.new_virtual_keyboard);
	server.layout_change.notify = server_layout_change;
	wl_signal_add(&server.output_layout->events.change, &server.layout_change);
	server.output_manager_apply.notify = output_manager_apply;
	wl_signal_add(&server.output_manager->events.apply, &server.output_manager_apply);
	server.output_manager_test.notify = output_manager_test;
	wl_signal_add(&server.output_manager->events.test, &server.output_manager_test);
	server.new_xdg_toplevel.notify = server_new_toplevel;
	wl_signal_add(&xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	if (server.xdg_dialog_manager != NULL) {
		server.new_xdg_dialog.notify = server_new_xdg_dialog;
		wl_signal_add(&server.xdg_dialog_manager->events.new_dialog,
			&server.new_xdg_dialog);
	}
	server.new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&layer_shell->events.new_surface, &server.new_layer_surface);
	server.new_decoration.notify = server_new_decoration;
	wl_signal_add(&decoration_manager->events.new_toplevel_decoration, &server.new_decoration);
	server.new_ime.notify = ime_new_input_method;
	wl_signal_add(&input_method_manager->events.new_input_method, &server.new_ime);
	server.new_text_input.notify = ime_new_text_input;
	wl_signal_add(&text_input_manager->events.new_text_input, &server.new_text_input);

	server.cursor_motion.notify = cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
	server.cursor_button.notify = cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);
	if (server.pointer_constraints != NULL) {
		server.new_pointer_constraint.notify = new_pointer_constraint;
		wl_signal_add(&server.pointer_constraints->events.new_constraint,
			&server.new_pointer_constraint);
	}
	if (server.shortcuts_inhibit != NULL) {
		server.new_shortcuts_inhibitor.notify = new_shortcuts_inhibitor;
		wl_signal_add(&server.shortcuts_inhibit->events.new_inhibitor,
			&server.new_shortcuts_inhibitor);
	}
	server.pointer_focus_change.notify = pointer_focus_change;
	wl_signal_add(&server.seat->pointer_state.events.focus_change,
		&server.pointer_focus_change);

	server.seat_request_set_cursor.notify = seat_request_set_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor, &server.seat_request_set_cursor);
	server.cursor_shape_set_shape.notify = seat_request_set_shape;
	wl_signal_add(&server.cursor_shape_manager->events.request_set_shape, &server.cursor_shape_set_shape);
	if (server.activation != NULL) {
		server.activation_request_activate.notify = xdg_activation_request_activate;
		wl_signal_add(&server.activation->events.request_activate,
			&server.activation_request_activate);
	}
	server.seat_request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection, &server.seat_request_set_selection);
	server.seat_request_set_primary_selection.notify = seat_request_set_primary_selection;
	wl_signal_add(&server.seat->events.request_set_primary_selection, &server.seat_request_set_primary_selection);
	server.seat_request_start_drag.notify = seat_request_start_drag;
	wl_signal_add(&server.seat->events.request_start_drag, &server.seat_request_start_drag);
	server.seat_start_drag.notify = seat_start_drag;
	wl_signal_add(&server.seat->events.start_drag, &server.seat_start_drag);

	wlr_seat_set_capabilities(server.seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

	const char *socket = wl_display_add_socket_auto(server.display);
	if (socket == NULL) {
		wlr_log(WLR_ERROR, "failed to add socket");
		return EXIT_FAILURE;
	}

	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "failed to start backend");
		return EXIT_FAILURE;
	}

	// linux-drm-syncobj-v1: 只有渲染器和后端都支持 timeline wait/signal 时才提供
	// 显式同步. 使用渲染器的 DRM fd (实际用于渲染的设备), 绝不用硬编码的
	// /dev/dri/card0.
	if (server.renderer->features.timeline && server.backend->features.timeline) {
		int drm_fd = wlr_renderer_get_drm_fd(server.renderer);
		if (drm_fd >= 0) {
			server.linux_drm_syncobj_manager =
				wlr_linux_drm_syncobj_manager_v1_create(server.display, 1,
					drm_fd);
			if (server.linux_drm_syncobj_manager == NULL) {
				wlr_log(WLR_ERROR,
					"failed to create linux-drm-syncobj-v1 global");
			}
		} else {
			wlr_log(WLR_INFO,
				"renderer has no DRM fd, disabling linux-drm-syncobj-v1");
		}
	} else {
		wlr_log(WLR_INFO,
			"renderer/backend timeline support unavailable, disabling "
			"linux-drm-syncobj-v1");
	}

	setenv("WAYLAND_DISPLAY", socket, true);

	// 状态栏用的 IPC 套接字 (Unix 域套接字上的 JSON).
	// 必须在启动用户守护进程之前建立: 状态栏/notifier 启动后会立刻连接,
	// 若此时 socket 尚未创建, 连接就失败, 表现为状态栏偶尔起不来.
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	char ipc_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	int path_len;
	if (runtime_dir != NULL && runtime_dir[0] != '\0') {
		path_len = snprintf(ipc_path, sizeof(ipc_path), "%s/monodywm.sock",
			runtime_dir);
	} else {
		path_len = snprintf(ipc_path, sizeof(ipc_path),
			"/tmp/monodywm.sock");
	}
	server.ipc_fd = -1;
	if (path_len < 0 || (size_t)path_len >= sizeof(ipc_path)) {
		// 静默截断会让 IPC 绑定到一个错误路径; 宁可不启动 IPC
		wlr_log(WLR_ERROR, "IPC socket path is too long");
	} else if (!ipc_server_init(&server, ipc_path)) {
		wlr_log(WLR_ERROR, "failed to start IPC socket at %s", ipc_path);
	}

	// Wayland 套接字和 IPC 套接字都已就绪: WM 起来了, 现在启动用户的守护进程
	// (来自 ~/.config/monodywm/run, 外加任何 -s 命令)
	run_startup_file();
	if (startup_cmd != NULL) {
		spawn_command(startup_cmd);
	}

	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
		socket);
	wl_display_run(server.display);

	// wlroots 断言它拥有的对象 (输出布局、输出管理器、seat、协议管理器等)
	// 销毁时没有残留监听器, 所以拆除前先摘掉合成器的每个监听器.
	// 按 surface 挂的监听器 (toplevel、layer surface、ime、光标) 由各自的
	// destroy 处理器在 destroy_clients() 期间移除.
	wl_list_remove(&server.new_output.link);
	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.new_virtual_pointer.link);
	wl_list_remove(&server.new_virtual_keyboard.link);
	wl_list_remove(&server.layout_change.link);
	wl_list_remove(&server.output_manager_apply.link);
	wl_list_remove(&server.output_manager_test.link);
	wl_list_remove(&server.new_xdg_toplevel.link);
	if (server.xdg_dialog_manager != NULL) {
		wl_list_remove(&server.new_xdg_dialog.link);
	}
	wl_list_remove(&server.new_layer_surface.link);
	wl_list_remove(&server.new_decoration.link);
	wl_list_remove(&server.new_ime.link);
	wl_list_remove(&server.new_text_input.link);
	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);
	if (server.pointer_constraints != NULL) {
		wl_list_remove(&server.new_pointer_constraint.link);
	}
	if (server.shortcuts_inhibit != NULL) {
		wl_list_remove(&server.new_shortcuts_inhibitor.link);
	}
	wl_list_remove(&server.pointer_focus_change.link);
	// 按 surface 挂的 commit 监听器通常由约束的 destroy 处理器摘掉;
	// 这里也摘一次, 确保拆除时不会触发 wlroots 的"signal 监听器为空"断言
	if (server.constraint_commit.link.prev != NULL) {
		wl_list_remove(&server.constraint_commit.link);
	}
	wl_list_remove(&server.seat_request_set_cursor.link);
	wl_list_remove(&server.cursor_shape_set_shape.link);
	if (server.activation != NULL) {
		wl_list_remove(&server.activation_request_activate.link);
	}
	wl_list_remove(&server.seat_request_set_selection.link);
	wl_list_remove(&server.seat_request_set_primary_selection.link);
	wl_list_remove(&server.seat_request_start_drag.link);
	wl_list_remove(&server.seat_start_drag.link);
	// 进行中的拖拽: 摘掉销毁监听器, 避免稍后销毁 client 时在已拆解的
	// server 上回调 refresh_pointer_focus
	if (server.drag_destroy.link.prev != NULL) {
		wl_list_remove(&server.drag_destroy.link);
	}
	// display 销毁后再收到终止信号会解引用已释放的 display: 先摘掉信号源
	if (server.sigint_source != NULL) {
		wl_event_source_remove(server.sigint_source);
		server.sigint_source = NULL;
	}
	if (server.sigterm_source != NULL) {
		wl_event_source_remove(server.sigterm_source);
		server.sigterm_source = NULL;
	}
	if (server.sighup_source != NULL) {
		wl_event_source_remove(server.sighup_source);
		server.sighup_source = NULL;
	}

	ipc_server_destroy(&server);
	wl_display_destroy_clients(server.display);

	// 光标与 xcursor 管理器不依赖 display 销毁过程中的任何对象, 在这里
	// 显式释放 (先光标后主题: 光标可能持有主题 buffer 的引用).
	wlr_cursor_destroy(server.cursor);
	server.cursor = NULL;
	wlr_xcursor_manager_destroy(server.xcursor_manager);
	server.xcursor_manager = NULL;

	// backend 必须显式销毁: wl_display 不拥有它. 销毁时会一并销毁输出,
	// 触发 monitor_destroy() 清掉各输出的预模糊节点 (scene 此时仍活着).
	wlr_backend_destroy(server.backend);
	server.backend = NULL;

	wl_display_destroy(server.display);
	server.display = NULL;

	// wlr_scene 也不随 display 销毁; 它持有 buffer/texture 引用, 必须在
	// 渲染器/分配器之前释放. 此时 backend 已销毁, monitor 的 blur 节点
	// 都已移除, 不会 double free.
	wlr_scene_node_destroy(&server.scene->tree.node);
	server.scene = NULL;

	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);

	// 日志每行都 fflush, 这里只是关掉 fd (同样只为 LSan 干净)
	if (log_file != NULL) {
		fclose(log_file);
		log_file = NULL;
	}
	return EXIT_SUCCESS;
}
