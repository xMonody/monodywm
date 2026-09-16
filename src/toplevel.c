// toplevel.c - xdg-shell toplevel 窗口
//
// 窗口生命周期 (map/unmap/commit/destroy)、窗口状态 (最大化、最小化、全屏、
// 移动、关闭)、xdg-decoration 模式协商, 以及为任务栏镜像每个窗口的
// foreign-toplevel 句柄.
//
// 合成器不绘制任何自己的窗口装饰: 客户端自带装饰的窗口保留其原生控件,
// 无装饰窗口只获得合成器的隐形抓取区 (标题栏和 resize 边缘), 由 pointer.c 处理.

#include "server.h"

#include "ipc.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>

// 从 /proc/<pid>/stat 读取 pid 的父进程 id; 失败返回 0.
static pid_t process_parent_pid(pid_t pid) {
	if (pid <= 1)
		return 0;
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	FILE *f = fopen(path, "re");
	if (f == NULL)
		return 0;
	char buf[1024];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	// comm 在括号里, 可能含空格和 ')': 最后一个 ')' 是分隔符,
	// 其后是 " state ppid ..."
	char *close = strrchr(buf, ')');
	if (close == NULL)
		return 0;
	char state = 0;
	pid_t ppid = 0;
	if (sscanf(close + 2, "%c %d", &state, &ppid) != 2)
		return 0;
	return ppid;
}

// pid 最近的祖先窗口, 沿进程树向上找 (从终端启动的窗口是终端的后代).
// 多个窗口同 pid 时优先聚焦的那个. 没有祖先窗口时返回 NULL
// (由合成器或状态栏启动, 或启动进程已守护化/被 init 收养).
static struct toplevel *window_ancestor(struct server *server, pid_t pid) {
	pid_t cur = pid;
	for (int hops = 0; hops < 16 && cur > 1; ++hops) {
		struct toplevel *fallback = NULL;
		struct toplevel *tl;
		wl_list_for_each(tl, &server->toplevels, link) {
			if (tl->pid != 0 && tl->pid == cur) {
				if (tl == server->focused)
					return tl;
				if (fallback == NULL)
					fallback = tl;
			}
		}
		if (fallback != NULL)
			return fallback;
		cur = process_parent_pid(cur);
		if (cur <= 0)
			break;
	}
	return NULL;
}

// 布局坐标下有效的窗口几何框: xdg 窗口 geometry
// (按 xdg-shell 的窗口边界, 不含 CSD 边距/投影).
// xdg 场景树锚定在该框的左上角.
void toplevel_box(struct toplevel *tl, struct wlr_box *box) {
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	box->x = tl->scene_tree->node.x;
	box->y = tl->scene_tree->node.y;
	box->width = base != NULL ? base->geometry.width : 0;
	box->height = base != NULL ? base->geometry.height : 0;
}

// 对话框/瞬态窗口: 通过 xdg_toplevel.set_parent 声明了父 toplevel 的窗口
// (GTK/Qt 对话框会这样). wlroots 会保持 toplevel->parent 更新
// (父窗口 unmaps 时也会清除), 所以实时检查总是最新的.
bool toplevel_is_dialog(struct toplevel *tl) {
	return tl->xdg_toplevel != NULL && tl->xdg_toplevel->parent != NULL;
}

// 通过 min == max 约束钉死尺寸的窗口 (如 QQ 登录窗口、启动画面):
// 无法有意义地缩放、最大化或最小化. 只有 min > 0 且等于 max 时该维度才算固定
// (min=0/max=0 表示"无约束").
bool toplevel_is_fixed_size(struct toplevel *tl) {
	if (tl->xdg_toplevel == NULL) {
		return false;
	}
	return tl->xdg_toplevel->current.min_width > 0 &&
		tl->xdg_toplevel->current.min_width ==
			tl->xdg_toplevel->current.max_width &&
		tl->xdg_toplevel->current.min_height > 0 &&
		tl->xdg_toplevel->current.min_height ==
			tl->xdg_toplevel->current.max_height;
}

// 状态栏可见的"归属"窗口: 弹窗没有独立的 IPC id,
// 先沿 xdg parent 链向上找到真正占据任务栏条目 (ipc_added) 的主窗口;
// 没有 parent 的 Electron 弹窗 (如 QQ "资料卡", 见 toplevel_hidden_from_taskbar)
// 退回同进程已有任务栏条目的窗口. 都没有时返回 NULL,
// 让状态栏清空高亮而不是收到未知 id.
struct toplevel *toplevel_ipc_owner(struct server *server, struct toplevel *tl) {
	if (tl == NULL) {
		return NULL;
	}
	pid_t pid = tl->pid;
	while (tl != NULL && !tl->ipc_added) {
		struct wlr_xdg_toplevel *parent =
			tl->xdg_toplevel != NULL ? tl->xdg_toplevel->parent : NULL;
		if (parent == NULL || parent->base == NULL) {
			tl = NULL;
			break;
		}
		tl = parent->base->data;
	}
	if (tl != NULL) {
		return tl;
	}
	struct toplevel *other;
	wl_list_for_each(other, &server->toplevels, link) {
		if (other->ipc_added && other->pid != 0 && other->pid == pid) {
			return other;
		}
	}
	return NULL;
}

// 向状态栏报告焦点: 弹窗归到其主窗口 (ipc_added 的窗口), 找不到则 id 0
static void ipc_send_focus(struct server *server, struct toplevel *tl) {
	ipc_send_window_event(server, "window_focus",
		tl != NULL ? toplevel_ipc_owner(server, tl) : NULL);
}

// toplevel 所在的输出 (按其中心), 或中心输出
struct wlr_output *toplevel_output(struct server *server,
		struct toplevel *tl) {
	struct wlr_box box;
	toplevel_box(tl, &box);
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, box.x + box.width / 2.0,
		box.y + box.height / 2.0);
	if (output == NULL) {
		output = wlr_output_layout_get_center_output(server->output_layout);
	}
	return output;
}

// 钳制位置, 使窗口永远不会落在 layer-shell 状态栏独占区下面:
// 左上角进入作区 (窗口比作区大时至少保留 40px 可见)
static void clamp_to_work_area(struct server *server, int *x, int *y,
		int width, int height) {
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, *x + width / 2, *y + height / 2);
	if (output == NULL) {
		output = wlr_output_layout_get_center_output(server->output_layout);
	}
	if (output == NULL) {
		return;
	}
	struct wlr_box area;
	get_work_area(server, output, &area);
	struct wlr_box out;
	wlr_output_layout_get_box(server->output_layout, output, &out);
	int top_limit = area.y;
	if (*x < area.x) {
		*x = area.x;
	}
	if (*y < top_limit) {
		*y = top_limit;
	}
	// 右边: 那里有状态栏时把窗口贴着作区边缘钳制 (绝不压到栏下);
	// 没有状态栏时, 窗口比作区大时至少保留 40px 可见
	if (area.x + area.width < out.x + out.width) { // 右侧有栏
		if (*x + width > area.x + area.width) {
			*x = area.x + area.width - width;
		}
	} else if (*x + 40 > area.x + area.width) {
		*x = area.x + area.width - 40;
	}
	// 底边: 同样规则 - 那里有栏时把窗口底边钳制到作区边缘,
	// 这样还原/映射的窗口绝不会滑到栏下面
	if (area.y + area.height < out.y + out.height) { // 底部有栏
		if (*y + height > area.y + area.height) {
			*y = area.y + area.height - height;
		}
	} else if (*y + 40 > area.y + area.height) {
		*y = area.y + area.height - 40;
	}
}

// 把最大化/全屏窗口还原到保存的位置, Windows 风格:
// 位置精确还原 - 包括用户故意放在屏幕边缘、只露出一部分的情况. 只有两道安全网,
// 且都不会移动仅仅半出屏的窗口:
//   - 完全在输出之外的落点不可达, 把窗口拉回作区;
//   - 窗口缩放期间出现的 layer-shell 状态栏 (其独占区缩小了作区)
//     不得遮挡窗口: 只在那一侧把它推出栏条带.
static void restore_box_position(struct server *server,
		const struct wlr_box *box, int *x, int *y) {
	*x = box->x;
	*y = box->y;
	if (box->width <= 0 || box->height <= 0) {
		return;
	}
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, box->x + box->width / 2,
		box->y + box->height / 2);
	if (output == NULL) {
		output = wlr_output_layout_get_center_output(server->output_layout);
	}
	if (output == NULL) {
		return;
	}
	struct wlr_box out;
	wlr_output_layout_get_box(server->output_layout, output, &out);
	struct wlr_box area;
	get_work_area(server, output, &area);

	// 完全在所有输出之外: 把左上角拉进作区
	struct wlr_box b = { *x, *y, box->width, box->height };
	struct wlr_box inter;
	if (!wlr_box_intersection(&inter, &b, &out)) {
		*x = area.x;
		*y = area.y;
		return;
	}

	// 从那一侧新出现的状态栏下面挪出来
	// (窗口拖动本来就不会把窗口滑到栏下)
	if (area.y > out.y && *y < area.y) { // 顶部有栏
		*y = area.y;
	}
	if (area.x > out.x && *x < area.x) { // 左侧有栏
		*x = area.x;
	}
	if (area.x + area.width < out.x + out.width &&
			*x + box->width > area.x + area.width) { // 右侧有栏
		*x = area.x + area.width - box->width;
	}
	if (area.y + area.height < out.y + out.height &&
			*y + box->height > area.y + area.height) { // 底部有栏
		*y = area.y + area.height - box->height;
	}
}

// 最大化窗口的几何: 恰好是作区 (窗口填满作区, 紧贴 layer-shell 状态栏的独占区)
void maximized_box(struct server *server, struct wlr_output *output,
		struct wlr_box *box) {
	get_work_area(server, output, box);
}

// 全屏窗口的几何: 整个输出框 (全屏会盖住 layer-shell 状态栏)
static void fullscreen_box(struct server *server, struct wlr_output *output,
		struct wlr_box *box) {
	wlr_output_layout_get_box(server->output_layout, output, box);
}

// layer-shell 独占区变化后, 把现在落在状态栏下的已有窗口移回作区
// (状态栏绝不能盖住它们), 并把最大化窗口重新适配到新作区
void arrange_toplevels_work_area(struct server *server,
		struct wlr_output *output) {
	struct wlr_box area;
	get_work_area(server, output, &area);
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
		if (base == NULL || !base->surface->mapped || tl->minimized ||
				tl->closing || tl->fullscreen) {
			// 全屏窗口有意覆盖整个输出 (含状态栏)
			continue;
		}
		struct wlr_box box;
		toplevel_box(tl, &box);
		if (box.width <= 0 || box.height <= 0) {
			continue;
		}
		if (toplevel_output(server, tl) != output) {
			continue;
		}
		if (tl->xdg_toplevel->current.maximized) {
			// 把最大化窗口重新适配到 (可能已缩小的) 作区
			struct wlr_box mbox;
			maximized_box(server, output, &mbox);
			if (box.x != mbox.x || box.y != mbox.y ||
					box.width != mbox.width || box.height != mbox.height) {
				wlr_xdg_toplevel_set_size(tl->xdg_toplevel, mbox.width,
					mbox.height);
				wlr_scene_node_set_position(&tl->scene_tree->node, mbox.x,
					mbox.y);
			}
			continue;
		}
		int x = box.x;
		int y = box.y;
		clamp_to_work_area(server, &x, &y, box.width, box.height);
		if (x != box.x || y != box.y) {
			wlr_scene_node_set_position(&tl->scene_tree->node, x, y);
		}
	}
}

// tl 的下一个/上一个已映射 toplevel, 会绕回链表.
// include_minimized 决定隐藏窗口是否可选. 没有则返回 NULL.
struct toplevel *neighbor_toplevel(struct server *server,
		struct toplevel *tl, bool next, bool include_minimized) {
	struct wl_list *head = &server->toplevels;
	struct wl_list *cur = &tl->link;
	struct wl_list *iter = next ? cur->next : cur->prev;
	while (iter != cur) {
		if (iter == head) {
			// 绕回循环链表
			iter = next ? head->next : head->prev;
			continue;
		}
		struct toplevel *candidate = wl_container_of(iter, candidate, link);
		if (candidate->xdg_toplevel->base != NULL &&
				candidate->xdg_toplevel->base->surface->mapped &&
				!candidate->closing &&
				(include_minimized || !candidate->minimized)) {
			return candidate;
		}
		iter = next ? iter->next : iter->prev;
	}
	return NULL;
}

// 按 IPC id 查找存活的 toplevel (与 ipc.c 共用)
struct toplevel *toplevel_by_id(struct server *server, int id) {
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		if (tl->id == id) {
			return tl;
		}
	}
	return NULL;
}

// 聚焦的 toplevel `tl` 消失时, 应接收焦点的窗口:
// 对话框把焦点交回其 xdg 父窗口 (它所属的主窗口), 否则优先启动它的窗口
// (after_id, 优先同进程的兄弟窗口而非终端), 再退回创建顺序中的上一个可见窗口.
static struct toplevel *focus_fallback(struct server *server,
		struct toplevel *tl) {
	// 通过 xdg_toplevel.set_parent 声明父窗口的瞬态/对话框:
	// 它关闭时焦点必须交回父主窗口, 而不是启动对话框的窗口
	// (如从终端运行 fcitx5-config - 终端是主窗口的启动者,
	// 但关闭对话框应保持焦点在主窗口上).
	if (tl->xdg_toplevel != NULL && tl->xdg_toplevel->parent != NULL) {
		struct wlr_xdg_surface *parent_base =
			tl->xdg_toplevel->parent->base;
		if (parent_base != NULL) {
			struct toplevel *parent = parent_base->data;
			if (parent != NULL && parent != tl &&
					parent->xdg_toplevel != NULL &&
					parent->xdg_toplevel->base != NULL &&
					parent->xdg_toplevel->base->surface->mapped) {
				return parent;
			}
		}
	}
	if (tl->after_id > 0) {
		struct toplevel *launcher = toplevel_by_id(server, tl->after_id);
		if (launcher != NULL && launcher != tl &&
				launcher->xdg_toplevel->base != NULL &&
				launcher->xdg_toplevel->base->surface->mapped) {
			return launcher;
		}
	}
	return neighbor_toplevel(server, tl, false, false);
}

// 只移动逻辑焦点 (激活状态/边框/IPC), 不碰 seat 键盘和输入法焦点.
// 当 layer-shell 覆盖层 (rofi/wofi) 持有键盘时窗口关闭/最小化, 覆盖层要保留
// 键盘直到 unmaps (那时 layer_surface_keyboard_unfocus 再交给 server->focused),
// 但聚焦窗口/边框/状态栏仍需跟随回退窗口.
static void focus_toplevel_state_only(struct server *server,
		struct toplevel *prev, struct toplevel *next) {
	server->focused = next;
	if (next != NULL) {
		wlr_xdg_toplevel_set_activated(next->xdg_toplevel, true);
		if (next->fthandle != NULL) {
			wlr_foreign_toplevel_handle_v1_set_activated(next->fthandle, true);
		}
		border_focus_changed(next, prev);
	}
	ipc_send_focus(server, next);
}

// ------------------------------------------------------------------
// 窗口状态
// ------------------------------------------------------------------

void close_toplevel(struct toplevel *tl) {
	if (tl->xdg_toplevel->base == NULL) {
		return;
	}
	if (tl->closing) {
		return; // 关闭已在等待中; 淡出接管该请求
	}
	if (animate_toplevel_close(tl)) {
		// 淡出正在运行, 窗口不可见后再发送关闭请求 (animate.c).
		// 关闭现在是粘性的: 窗口在真正消失前都是惰性的 - 不可重新聚焦、
		// 最小化、最大化或还原, 所以之后的用户操作无法悄悄取消关闭 -
		// 淡出结束时 animate.c 会禁用其场景节点, 不理会关闭请求的客户端
		// 也绝不会留下不可见却阻塞输入的窗口.
		struct server *server = tl->server;
		tl->closing = true;
		if (server->focused == tl) {
			// 立即把键盘焦点交给本窗口死亡时会接收它的窗口 (类似最小化):
			// 输入绝不能继续指向正在淡出的窗口
			struct toplevel *prev = focus_fallback(server, tl);
			server->focused = NULL;
			if (tl->xdg_toplevel->base != NULL) {
				wlr_xdg_toplevel_set_activated(tl->xdg_toplevel, false);
			}
			if (server->layer_focused != NULL) {
				// 覆盖层持有键盘: 保留, 只移动逻辑焦点
				focus_toplevel_state_only(server, tl, prev);
			} else {
				wlr_seat_keyboard_clear_focus(server->seat);
				if (prev != NULL) {
					focus_toplevel(server, prev);
				} else {
					ipc_send_window_event(server, "window_focus", NULL);
					ime_set_focus(server, NULL);
				}
			}
			// 淡出必须保持在新聚焦窗口之上可见
			// (对应最小化掉落: 焦点移走后把掉落窗口提升到顶部)
			if (tl->scene_tree != NULL) {
				wlr_scene_node_raise_to_top(&tl->scene_tree->node);
			}
		}
		return;
	}
	// 没有运行淡出 (动画被禁用, 或窗口已隐藏): 立即关闭
	wlr_xdg_toplevel_send_close(tl->xdg_toplevel);
}

void set_fullscreen(struct server *server, struct toplevel *tl,
		bool fullscreen) {
	if (tl->closing || tl->xdg_toplevel->base == NULL ||
			tl->xdg_toplevel->current.fullscreen == fullscreen) {
		return;
	}
	if (tl->morph_active) {
		// 最大化/还原缩放正在运行: 让全屏干净地接管, 而不是与运行中的缩放相互干扰
		animate_toplevel_abort_geometry(tl);
	}
	tl->user_moved = true; // 全屏状态: 停止自动居中
	tl->fullscreen = fullscreen;
	// 边框宽度/颜色依赖全屏状态
	rounded_cache_dirty(tl);
	if (fullscreen) {
		// 记住全屏前的几何, 供之后还原. 与 restore_box 分开保存:
		// 从最大化进入全屏不得覆盖最大化保存的浮动几何.
		toplevel_box(tl, &tl->fullscreen_restore_box);
		tl->has_fullscreen_restore_box = true;

		struct wlr_output *output = toplevel_output(server, tl);
		if (output != NULL) {
			struct wlr_box fbox;
			fullscreen_box(server, output, &fbox);
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel, fbox.width,
				fbox.height);
			// Windows 式缩放进入全屏框 (animate.c): 先继续显示浮动窗口,
			// 直到客户端提交全屏内容, 再放大. 缩放无法运行时退回瞬时跳变.
			struct wlr_box from_box;
			toplevel_box(tl, &from_box);
			struct wlr_box to_box = { fbox.x, fbox.y, fbox.width, fbox.height };
			if (!animate_toplevel_geometry(server, tl, &from_box, &to_box)) {
				wlr_scene_node_set_position(&tl->scene_tree->node, fbox.x,
					fbox.y);
			}
		} else {
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel, 0, 0);
		}
	} else {
		if (tl->has_fullscreen_restore_box &&
				tl->fullscreen_restore_box.width > 0) {
			int x = tl->fullscreen_restore_box.x;
			int y = tl->fullscreen_restore_box.y;
			// 精确回到原位; 期间出现的状态栏是唯一可能移动它的东西
			// (restore_box_position)
			restore_box_position(server, &tl->fullscreen_restore_box,
				&x, &y);
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel,
				tl->fullscreen_restore_box.width,
				tl->fullscreen_restore_box.height);
			// Windows 式缩放回浮动框 (animate.c), 与最大化还原对称:
			// 在客户端提交还原后的内容前保持全屏窗口在屏, 再缩小到位
			struct wlr_box from_box;
			toplevel_box(tl, &from_box);
			struct wlr_box to_box = { x, y, tl->fullscreen_restore_box.width,
				tl->fullscreen_restore_box.height };
			if (!animate_toplevel_geometry(server, tl, &from_box, &to_box)) {
				wlr_scene_node_set_position(&tl->scene_tree->node, x, y);
			}
		}
	}
	wlr_xdg_toplevel_set_fullscreen(tl->xdg_toplevel, fullscreen);
	if (tl->fthandle != NULL) {
		wlr_foreign_toplevel_handle_v1_set_fullscreen(tl->fthandle,
			fullscreen);
	}
	// 通知状态栏 (对话框不占任务栏条目, 无 id 可报告)
	if (tl->ipc_added) {
		ipc_send_window_event(server, "window_full", tl);
	}
}

void set_maximized(struct server *server, struct toplevel *tl,
		bool maximized) {
	if (tl->closing || tl->xdg_toplevel->base == NULL) {
		return;
	}
	if (tl->morph_active) {
		// 正在进行的最大化/还原缩放拥有场景节点: 让新的最大化/还原请求
		// 干净地替换它, 而不是与运行中的缩放相互干扰
		// (快速切换, 或客户端在首个 configure ack 前重复声明 set_maximized)
		animate_toplevel_abort_geometry(tl);
	}
	if (tl->fullscreen) {
		// 全屏期间不可最大化/还原; 只有离开全屏才回到之前的状态
		return;
	}
	if (toplevel_is_dialog(tl) || toplevel_is_fixed_size(tl)) {
		// 对话框和固定尺寸窗口 (如 QQ 登录) 从不最大化
		// (其顶部边框是关闭按钮, 不是标题栏)
		return;
	}
	if (maximized && tl->xdg_toplevel->current.maximized) {
		// 已最大化: 重新适配到最大化几何. 最大化窗口可能偏离它
		// (如客户端拖动一个从未还原的最大化窗口), 而客户端自己的最大化按钮
		// 会发送 set_maximized, 否则就是静默空操作 - 窗口会卡住.
		// 重新声明该框能让最大化请求总有响应.
		struct wlr_output *output = toplevel_output(server, tl);
		if (output != NULL) {
			struct wlr_box box;
			maximized_box(server, output, &box);
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel, box.width,
				box.height);
			wlr_scene_node_set_position(&tl->scene_tree->node, box.x, box.y);
		}
		return;
	}
	if (tl->xdg_toplevel->current.maximized == maximized) {
		return;
	}
	tl->user_moved = true; // 最大化/还原状态: 停止自动居中
	if (maximized) {
		// 记住浮动几何, 使还原能把窗口精确送回最大化前的位置 (Windows 行为).
		// 只在客户端仍报告窗口为浮动 (current.maximized 为 false) 时捕获:
		// QQ 这类客户端会在首个请求 ack 前重复声明 set_maximized,
		// 那时重新保存会用已最大化的框覆盖浮动几何.
		// 只要浮动框相比上次捕获有变化就重新捕获, 这样两次最大化/还原循环之间的
		// 移动或缩放绝不会让下次还原回到旧位置或旧尺寸.
		if (!tl->xdg_toplevel->current.maximized) {
			struct wlr_box fbox;
			toplevel_box(tl, &fbox);
			if (!tl->has_restore_box ||
					tl->restore_box.x != fbox.x ||
					tl->restore_box.y != fbox.y ||
					tl->restore_box.width != fbox.width ||
					tl->restore_box.height != fbox.height) {
				tl->restore_box = fbox;
				tl->has_restore_box = true;
			}
		}

		struct wlr_output *output = toplevel_output(server, tl);
		if (output != NULL) {
			struct wlr_box box;
			maximized_box(server, output, &box);
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel, box.width,
				box.height);
			// Windows 式缩放进入最大化框 (animate.c): 先继续显示浮动窗口,
			// 直到客户端提交最大化内容, 再放大. 缩放无法运行时退回瞬时跳变.
			struct wlr_box from_box;
			toplevel_box(tl, &from_box);
			struct wlr_box to_box = { box.x, box.y, box.width, box.height };
			if (!animate_toplevel_geometry(server, tl, &from_box, &to_box)) {
				wlr_scene_node_set_position(&tl->scene_tree->node, box.x,
					box.y);
			}
		} else {
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel, 0, 0);
		}
	} else {
		// 还原到最大化时保存的几何 (位置 + 尺寸), 让客户端自己的还原按钮
		// 把窗口精确送回原位. 只有从未保存过浮动几何时才退回 0x0.
		if (tl->has_restore_box && tl->restore_box.width > 0) {
			int x = tl->restore_box.x;
			int y = tl->restore_box.y;
			// 精确还原到保存的位置 (即使半出屏); 只有期间出现的状态栏可能移动它
			restore_box_position(server, &tl->restore_box, &x, &y);

			wlr_xdg_toplevel_set_size(tl->xdg_toplevel,
				tl->restore_box.width, tl->restore_box.height);
			// Windows 式缩放回浮动框 (animate.c):
			// 在客户端提交还原后的内容前保持最大化窗口在屏, 再缩小到位
			struct wlr_box from_box;
			toplevel_box(tl, &from_box);
			struct wlr_box to_box = { x, y, tl->restore_box.width,
				tl->restore_box.height };
			if (!animate_toplevel_geometry(server, tl, &from_box, &to_box)) {
				wlr_scene_node_set_position(&tl->scene_tree->node, x, y);
			}
		} else {
			// 0x0 让客户端重新自选尺寸
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel, 0, 0);
		}
	}
	wlr_xdg_toplevel_set_maximized(tl->xdg_toplevel, maximized);
	if (tl->fthandle != NULL) {
		wlr_foreign_toplevel_handle_v1_set_maximized(tl->fthandle, maximized);
	}
}

// 取消最大化回到之前保存的几何 (拖动最大化窗口的标题栏, 键盘/按钮切换).
// `animate` 为真时还原播放与最大化相同的 Windows 式缩放 (animate.c),
// 且调用方之后不得自行移动场景节点 - 缩放会把节点留在还原框.
// 拖动还原传 false: 指针抓住还原后的窗口并按 motion 定位, 缩放会与抓取冲突.
void restore_maximized_toplevel(struct toplevel *tl, bool animate) {
	if (tl->closing || tl->xdg_toplevel->base == NULL ||
			!tl->xdg_toplevel->current.maximized || tl->fullscreen) {
		return;
	}
	if (tl->morph_active) {
		// 正在进行的最大化/还原缩放拥有场景节点
		animate_toplevel_abort_geometry(tl);
	}
	tl->user_moved = true; // 拖动还原: 停止自动居中
	if (tl->has_restore_box && tl->restore_box.width > 0) {
		int x = tl->restore_box.x;
		int y = tl->restore_box.y;
		// 精确还原到保存的位置 (即使半出屏); 只有期间出现的状态栏可能移动它
		restore_box_position(tl->server, &tl->restore_box, &x, &y);
		wlr_xdg_toplevel_set_size(tl->xdg_toplevel, tl->restore_box.width,
			tl->restore_box.height);
		if (animate) {
			// Windows 式缩放回浮动框, 与最大化对称 (animate.c)
			struct wlr_box from_box;
			toplevel_box(tl, &from_box);
			struct wlr_box to_box = { x, y, tl->restore_box.width,
				tl->restore_box.height };
			if (animate_toplevel_geometry(tl->server, tl, &from_box,
					&to_box)) {
				wlr_xdg_toplevel_set_maximized(tl->xdg_toplevel, false);
				if (tl->fthandle != NULL) {
					wlr_foreign_toplevel_handle_v1_set_maximized(
						tl->fthandle, false);
				}
				return;
			}
		}
		wlr_scene_node_set_position(&tl->scene_tree->node, x, y);
	}
	wlr_xdg_toplevel_set_maximized(tl->xdg_toplevel, false);
	if (tl->fthandle != NULL) {
		wlr_foreign_toplevel_handle_v1_set_maximized(tl->fthandle, false);
	}
}

void set_minimized(struct server *server, struct toplevel *tl,
		bool minimized) {
	if (tl->closing) {
		// 关闭已在等待中: 绝不最小化/还原垂死的窗口
		return;
	}
	if (toplevel_is_dialog(tl) || toplevel_is_fixed_size(tl)) {
		// 对话框和固定尺寸窗口没有最小化按钮
		return;
	}
	if (tl->minimized == minimized) {
		return;
	}
	if (tl->morph_active) {
		// 正在进行的最大化/还原缩放拥有场景节点 (快速切换, 或客户端在首个
		// configure ack 前重复声明 set_maximized): 让新请求替换它
		animate_toplevel_abort_geometry(tl);
	}
	tl->minimized = minimized;
	if (tl->fthandle != NULL) {
		wlr_foreign_toplevel_handle_v1_set_minimized(tl->fthandle, minimized);
	}
	if (minimized && server->focused == tl) {
		// 把键盘/光标焦点交给上一个可见窗口. 窗口在下落期间保持场景节点可见
		// (animate.c), 但它已被标记为最小化, 所以没有东西会再聚焦它;
		// 还原它 (焦点循环、foreign-toplevel 激活) 会把它放回原位.
		struct toplevel *prev = neighbor_toplevel(server, tl, false, false);
		server->focused = NULL;
		if (tl->xdg_toplevel->base != NULL) {
			wlr_xdg_toplevel_set_activated(tl->xdg_toplevel, false);
		}
		if (server->layer_focused != NULL) {
			// 覆盖层持有键盘: 保留
			focus_toplevel_state_only(server, tl, prev);
		} else {
			wlr_seat_keyboard_clear_focus(server->seat);
			if (prev != NULL) {
				focus_toplevel(server, prev);
			} else {
				ipc_send_window_event(server, "window_focus", NULL);
				ime_set_focus(server, NULL);
			}
		}
	}
	if (minimized) {
		// 动画下落; 没有动画时直接隐藏节点
		// (窗口保持位置, 所以还原会把它精确放回原位)
		if (!animate_toplevel_minimize(server, tl)) {
			wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
		}
	} else if (!animate_toplevel_restore(server, tl)) {
		wlr_scene_node_set_enabled(&tl->scene_tree->node, true);
	}
	// 隐藏窗口可能让光标下露出不同的 surface
	update_cursor_style(server);
}

// 提升 toplevel 的场景节点, 以及声明它为 xdg 父窗口的所有对话框 (递归):
// 聚焦/点击主窗口绝不能把它打开的对话框埋到下面. 对话框在父窗口之后提升,
// 所以它们最终总是叠在父窗口之上.
static void toplevel_raise(struct server *server, struct toplevel *tl) {
	if (tl->scene_tree == NULL) {
		return;
	}
	wlr_scene_node_raise_to_top(&tl->scene_tree->node);
	struct toplevel *child;
	wl_list_for_each(child, &server->toplevels, link) {
		if (child != tl && child->xdg_toplevel != NULL &&
				child->xdg_toplevel->parent == tl->xdg_toplevel) {
			toplevel_raise(server, child);
		}
	}
}

void focus_toplevel(struct server *server, struct toplevel *tl) {
	if (tl->closing || tl->minimized || tl->xdg_toplevel->base == NULL ||
			!tl->xdg_toplevel->base->surface->mapped) {
		return;
	}
	bool layer_held = server->layer_focused != NULL;
	struct toplevel *prev = server->focused;
	if (prev == tl && !layer_held) {
		toplevel_raise(server, tl);
		return;
	}
	if (prev != NULL && prev->xdg_toplevel->base != NULL) {
		wlr_xdg_toplevel_set_activated(prev->xdg_toplevel, false);
		if (prev->fthandle != NULL) {
			wlr_foreign_toplevel_handle_v1_set_activated(prev->fthandle, false);
		}
	}
	server->focused = tl;
	// 边框颜色依赖焦点: 重绘新聚焦窗口和先前聚焦窗口,
	// 让它们的圆角 FBO 取到新的聚焦/未聚焦边框色 (见 border.c)
	border_focus_changed(tl, prev);
	wlr_xdg_toplevel_set_activated(tl->xdg_toplevel, true);
	if (tl->fthandle != NULL) {
		wlr_foreign_toplevel_handle_v1_set_activated(tl->fthandle, true);
	}
	// 把 seat 键盘移到这个 toplevel. 若启动器覆盖层 (rofi) 持有它,
	// layer_keyboard_clear() 释放该持有并在同一步把键盘移到这里;
	// 否则 toplevel 直接接管.
	if (layer_held) {
		layer_keyboard_clear(server, tl->xdg_toplevel->base->surface);
	} else {
		seat_keyboard_focus(server, tl->xdg_toplevel->base->surface);
	}
	toplevel_raise(server, tl);
	// 告诉输入法现在哪个 surface 拥有文本输入
	ime_set_focus(server, tl->xdg_toplevel->base->surface);
	// 通知状态栏焦点变化 (对话框归到其主窗口)
	ipc_send_focus(server, tl);
}

// xdg-activation-v1 request_activate: surface 的客户端用有效激活 token 请求焦点.
// 聚焦 (并还原) 匹配的 toplevel.
void xdg_activation_request_activate(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		activation_request_activate);
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		if (tl->xdg_toplevel->base == NULL ||
				tl->xdg_toplevel->base->surface != event->surface) {
			continue;
		}
		if (tl->minimized) {
			set_minimized(server, tl, false);
		}
		focus_toplevel(server, tl);
		return;
	}
}

void update_toplevel_output(struct server *server, struct toplevel *tl) {
	if (tl->fthandle == NULL || tl->xdg_toplevel->base == NULL ||
			!tl->xdg_toplevel->base->surface->mapped) {
		return;
	}
	struct wlr_output *output = toplevel_output(server, tl);
	if (output == tl->last_output) {
		return;
	}
	if (tl->last_output != NULL) {
		wlr_foreign_toplevel_handle_v1_output_leave(tl->fthandle,
			tl->last_output);
	}
	tl->last_output = output;
	if (output != NULL) {
		wlr_foreign_toplevel_handle_v1_output_enter(tl->fthandle, output);
	}
}

// ------------------------------------------------------------------
// xdg-shell toplevel
// ------------------------------------------------------------------

static void toplevel_unfocus(struct server *server, struct toplevel *tl) {
	if (server->focused == tl) {
		// 聚焦窗口正在消失: 把焦点交回启动它的窗口 (after_id; 同进程兄弟窗口
		// 优先于产生该进程的终端), 或上一个可见窗口
		struct toplevel *prev = focus_fallback(server, tl);
		server->focused = NULL;
		if (server->layer_focused != NULL) {
			// 覆盖层持有键盘: 保留
			focus_toplevel_state_only(server, tl, prev);
		} else {
			wlr_seat_keyboard_clear_focus(server->seat);
			if (prev != NULL) {
				focus_window(server, prev);
			} else {
				ipc_send_window_event(server, "window_focus", NULL);
				ime_set_focus(server, NULL);
			}
		}
	}
	if (server->zone_toplevel == tl || server->move_toplevel == tl) {
		end_move(server);
	}
	if (server->resize_toplevel == tl) {
		end_resize(server);
	}
}

// ------------------------------------------------------------------
// subsurface: 它们的 commit 把圆角 FBO 缓存标记为脏,
// 内容变化时重绘离屏副本
// ------------------------------------------------------------------

static void toplevel_subsurface_commit(struct wl_listener *listener,
		void *data) {
	struct toplevel_subsurface *ts = wl_container_of(listener, ts, commit);
	(void)data;
	// wlroots 在提交后会把 subsuface 的透明度重新应用为 1.0; 重新隐藏它
	// (仅当已经发布了有效的圆角 FBO), 并重绘 FBO 缓存.
	// 提交附带的 damage 会被收集用于局部重绘 (rounded.c).
	rounded_cache_subsurface_commit(ts->tl, ts);
}

static void toplevel_subsurface_add(struct toplevel *tl,
		struct wlr_subsurface *subsurface);

static void toplevel_subsurface_destroy(struct wl_listener *listener,
		void *data) {
	struct toplevel_subsurface *ts = wl_container_of(listener, ts, destroy);
	(void)data;
	wl_list_remove(&ts->commit.link);
	wl_list_remove(&ts->new_subsurface.link);
	wl_list_remove(&ts->destroy.link);
	wl_list_remove(&ts->link);
	// subsurface 的旧内容必须从缓存 FBO 中消失:
	// 一次整幅重绘 (伴随其 damage 记账已清除) 会覆盖
	if (ts->tl->rounded != NULL) {
		rounded_cache_dirty(ts->tl);
	}
	pixman_region32_fini(&ts->damage);
	free(ts);
}

static void toplevel_subsurface_new_subsurface(struct wl_listener *listener,
		void *data) {
	struct toplevel_subsurface *ts = wl_container_of(listener, ts,
		new_subsurface);
	// 嵌套 subsurface (subsurface 的 subsurface): 与直接 subsurface 一样跟踪,
	// 使其 commit 也失效 FBO
	toplevel_subsurface_add(ts->tl, data);
}

static void toplevel_subsurface_add(struct toplevel *tl,
		struct wlr_subsurface *subsurface) {
	struct toplevel_subsurface *ts = calloc(1, sizeof(*ts));
	if (ts == NULL) {
		return;
	}
	ts->tl = tl;
	ts->subsurface = subsurface;
	pixman_region32_init(&ts->damage);
	ts->prev_x = subsurface->current.x;
	ts->prev_y = subsurface->current.y;
	ts->prev_w = subsurface->surface->current.width;
	ts->prev_h = subsurface->surface->current.height;
	ts->commit.notify = toplevel_subsurface_commit;
	wl_signal_add(&subsurface->surface->events.commit, &ts->commit);
	ts->new_subsurface.notify = toplevel_subsurface_new_subsurface;
	wl_signal_add(&subsurface->surface->events.new_subsurface,
		&ts->new_subsurface);
	ts->destroy.notify = toplevel_subsurface_destroy;
	wl_signal_add(&subsurface->events.destroy, &ts->destroy);
	wl_list_insert(tl->subsurfaces.prev, &ts->link);
	// 新加入的 subsurface 必须从场景中隐藏, 并并入下一次 FBO 渲染
	rounded_cache_hide_content(tl);
	rounded_cache_dirty(tl);

	// 已存在于它下面的 subsurface 只能靠遍历场景图找到;
	// 上面的 new_subsurface 监听器只捕获将来的, 所以也要递归已有的子节点
	struct wlr_subsurface *child;
	wl_list_for_each(child, &subsurface->surface->current.subsurfaces_below,
			current.link) {
		toplevel_subsurface_add(tl, child);
	}
	wl_list_for_each(child, &subsurface->surface->current.subsurfaces_above,
			current.link) {
		toplevel_subsurface_add(tl, child);
	}
}

static void xdg_toplevel_new_subsurface(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, new_subsurface);
	toplevel_subsurface_add(tl, data);
}

// toplevel 释放前移除所有 subsurface 记账
static void toplevel_destroy_subsurfaces(struct toplevel *tl) {
	struct toplevel_subsurface *ts, *tmp;
	wl_list_for_each_safe(ts, tmp, &tl->subsurfaces, link) {
		wl_list_remove(&ts->commit.link);
		wl_list_remove(&ts->new_subsurface.link);
		wl_list_remove(&ts->destroy.link);
		wl_list_remove(&ts->link);
		pixman_region32_fini(&ts->damage);
		free(ts);
	}
}

// 不占独立任务栏条目的"弹窗":
//  - 通过 xdg_toplevel.set_parent 声明父窗口的对话框 (GTK/Qt 对话框);
//  - 没有 xdg parent 的固定尺寸小窗, 且同进程已有其它任务栏条目
//    (Electron 弹窗, 如 QQ "资料卡" 322x472, min == max). 若它是该进程
//    唯一的窗口则不隐藏, 这样 QQ 登录窗仍有任务栏条目.
static bool toplevel_hidden_from_taskbar(struct server *server,
		struct toplevel *tl) {
	if (toplevel_is_dialog(tl)) {
		return true;
	}
	if (!toplevel_is_fixed_size(tl)) {
		return false;
	}
	struct toplevel *other;
	wl_list_for_each(other, &server->toplevels, link) {
		if (other != tl && other->ipc_added && other->pid != 0 &&
				other->pid == tl->pid) {
			return true;
		}
	}
	return false;
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, map);
	struct server *server = tl->server;

	if (tl->closing) {
		// 客户端重新显示了被要求关闭的 surface (如用 unmap/remap 花样回应关闭请求):
		// 保持窗口隐藏并重新发送关闭 - 关闭请求在 surface 销毁前都是粘性的
		if (tl->scene_tree != NULL) {
			wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
		}
		wlr_xdg_toplevel_send_close(tl->xdg_toplevel);
		return;
	}

	if (!tl->positioned) {
		tl->positioned = true;
		// 尺寸保持客户端提交的精确值; 位置在输出上水平垂直居中 (place.c)
		place_toplevel(server, tl);
	}
	// 先通知状态栏, 再发状态/焦点事件.
	// 对话框/弹窗不占独立任务栏条目 (归到其主窗口), 因此不分配 IPC id:
	// 不发 window_added, 之后的 window_focus 会用 toplevel_ipc_owner 归到主窗口.
	if (!toplevel_hidden_from_taskbar(server, tl)) {
		tl->ipc_added = true;
		ipc_send_window_event(server, "window_added", tl);
	}
	// 处理首个提交前到达的全屏/最大化请求 (此时 surface 已初始化并映射)
	if (tl->xdg_toplevel->requested.fullscreen) {
		// 初始状态, 不是用户全屏动作: 瞬时应用
		tl->skip_geom = true;
		set_fullscreen(server, tl, true);
		tl->skip_geom = false;
	} else if (tl->xdg_toplevel->requested.maximized) {
		// 初始状态, 不是用户最大化动作: 瞬时应用
		tl->skip_geom = true;
		set_maximized(server, tl, true);
		tl->skip_geom = false;
	}
	focus_toplevel(server, tl);
	update_toplevel_output(server, tl);
	rounded_cache_hide_content(tl);
	rounded_cache_dirty(tl);
	// 刚映射的窗口淡入 (animate.c)
	if (!tl->minimized) {
		animate_toplevel_fade_in(server, tl);
	}
	// 在静止光标下映射的窗口必须立即显示正确光标 (标题区/resize 边缘),
	// 而不用等待 motion
	update_cursor_style(server);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, unmap);
	// 窗口在动画中途自行隐藏: 停止动画并让窗口处于干净状态
	// (等待中的最小化完成, 等待中的还原落定)
	animate_toplevel_cancel(tl);
	toplevel_unfocus(tl->server, tl);
	update_cursor_style(tl->server);
}

static void toplevel_client_cursor_gone(struct server *server,
		struct wl_client *client);

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, toplevel_destroy);
	struct server *server = tl->server;

	toplevel_unfocus(server, tl);
	// 窗口正在消失: 若其客户端拥有当前光标 (文本框上的 I-beam 等), 回到默认箭头.
	// 必须在这里做, 此时 xdg_toplevel 仍存活 - wlroots 在此信号后立即释放它,
	// 所以 xdg_surface_destroy 不能碰它.
	if (tl->xdg_toplevel->base != NULL) {
		toplevel_client_cursor_gone(server,
			tl->xdg_toplevel->base->surface->resource->client);
	}
	wl_list_remove(&tl->link);

	if (tl->fthandle != NULL) {
		wlr_foreign_toplevel_handle_v1_destroy(tl->fthandle);
	}

	// 摘掉挂在 toplevel 及其 surface 上的所有监听器; xdg surface 监听器
	// (tl->destroy、tl->new_popup) 保持挂接, 直到 xdg surface 自身销毁.
	// popup 无需在此清理: 它们的场景树是 tl->scene_tree 的子节点,
	// 各自在自己的树销毁时自行释放 (树随 popup 的 xdg surface 或 tl->scene_tree 销毁).
	toplevel_destroy_subsurfaces(tl);
	wl_list_remove(&tl->new_subsurface.link);
	if (tl->rounded != NULL) {
		rounded_cache_destroy(tl->rounded);
		tl->rounded = NULL;
	}
	wl_list_remove(&tl->toplevel_destroy.link);
	wl_list_remove(&tl->map.link);
	wl_list_remove(&tl->unmap.link);
	wl_list_remove(&tl->commit.link);
	wl_list_remove(&tl->request_maximize.link);
	wl_list_remove(&tl->request_minimize.link);
	wl_list_remove(&tl->request_fullscreen.link);
	wl_list_remove(&tl->request_move.link);
	wl_list_remove(&tl->request_resize.link);
	wl_list_remove(&tl->set_title.link);
	wl_list_remove(&tl->set_app_id.link);
	wl_list_remove(&tl->new_popup.link);
}

// 正在关闭窗口的客户端可能拥有当前指针光标 (cursor-shape 或 cursor surface,
// 如文本框上的 I-beam): 丢弃过期状态, 并在合成器没有覆盖光标
// (移动/缩放/标题栏) 时回到默认箭头
static void toplevel_client_cursor_gone(struct server *server,
		struct wl_client *client) {
	bool owned = false;
	if (server->client_cursor_shape != 0 &&
			server->client_cursor_shape_client != NULL &&
			server->client_cursor_shape_client->client == client) {
		server->client_cursor_shape = 0;
		// 连同指针一起摘掉 destroy 监听器, 否则这个孤儿监听器之后会被重新挂到
		// 另一个 seat client 上并破坏 destroy 监听器链表
		// (该客户端断开时 wlroots 会在 seat_client_destroy 中断言)
		wl_list_remove(&server->client_cursor_shape_client_destroy.link);
		server->client_cursor_shape_client = NULL;
		owned = true;
	}
	if (server->client_cursor_surface != NULL &&
			server->client_cursor_surface->resource->client == client) {
		wl_list_remove(&server->client_cursor_destroy.link);
		server->client_cursor_surface = NULL;
		owned = true;
	}
	if (owned && server->cursor_override == NULL) {
		wlr_log(WLR_DEBUG, "cursor: default (window closed)");
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
			"left_ptr");
	}
}

static void xdg_surface_destroy(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, destroy);
	wl_list_remove(&tl->destroy.link);
	// 窗口消失前通知状态栏
	if (tl->ipc_added) {
		ipc_send_window_event(tl->server, "window_removed", tl);
	}
	// xdg 场景树由 wlroots 自己注册的 xdg-surface 场景处理器销毁
	// (在 wlr_scene_xdg_surface_create 中)
	free(tl->app_id);
	free(tl);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, commit);
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;

	// 按 xdg-shell, 合成器必须用首个 configure 回应初始提交
	if (base != NULL && base->initial_commit) {
		wlr_xdg_toplevel_set_size(tl->xdg_toplevel, 0, 0);
	}

	update_toplevel_output(tl->server, tl);

	// 首个 configure 只能在 xdg surface 初始化后发送;
	// 在客户端首次提交时回应它的装饰模式 (或我们的默认值)
	if (tl->decoration != NULL && !tl->decoration_configured &&
			base != NULL && base->initialized) {
		tl->decoration_configured = true;
		if (tl->decoration_mode !=
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE) {
			wlr_xdg_toplevel_decoration_v1_set_mode(tl->decoration,
				tl->decoration_mode);
		}
	}

	// 上/左抓取在这里定位, 即客户端真正提交了新几何之后:
	// update_resize() 只发送尺寸 configure, 对这些抓取从不移动节点,
	// 所以固定的 (对侧) 边缘保持粘在抓取开始时的位置, 旧的、仍更大的 buffer
	// 也绝不会在移动后的位置显示 - 那正是从上边缩放时底边弹跳的原因.
	// 在下一帧绘制前重新声明位置, 能让框与已提交 buffer 保持同步.
	if (tl->server->resizing && tl->server->resize_toplevel == tl &&
			base != NULL) {
		int x = tl->scene_tree->node.x;
		int y = tl->scene_tree->node.y;
		if ((tl->server->resize_edges & WLR_EDGE_LEFT) != 0) {
			x = tl->server->resize_orig.x + tl->server->resize_orig.width
				- base->geometry.width;
		}
		if ((tl->server->resize_edges & WLR_EDGE_TOP) != 0) {
			y = tl->server->resize_orig.y
				+ tl->server->resize_orig.height - base->geometry.height;
		}
		wlr_scene_node_set_position(&tl->scene_tree->node, x, y);
	}

	// 轮廓模式 (CONFIG_RESIZE_DRAW_CONTENTS=0): 按钮释放只画了轮廓并发送最终尺寸;
	// 本次提交携带最终几何, 所以现在结束缩放抓取.
	// 上面的重新定位已经把上/左抓取锚定到已提交尺寸.
	if (tl->server->resizing && tl->server->resize_toplevel == tl &&
			tl->server->resize_final_pending) {
		resize_grab_clear(tl->server);
	}

	// 让新窗口保持居中直到用户与之交互:
	// Electron 窗口 (QQ) 常先映射一个小的占位 surface, 稍后才提交真实尺寸,
	// 所以 surface 尺寸变化时就重新居中 (place.c).
	// 用户交互 (移动/缩放/最大化/全屏) 会置 user_moved 并停止,
	// 之后窗口停在用户放的位置.
	if (!tl->user_moved && !tl->minimized &&
			!tl->xdg_toplevel->current.maximized && !tl->fullscreen &&
			base != NULL && base->surface != NULL) {
		struct wlr_surface *s = base->surface;
		if (!tl->placed || s->current.width != tl->placed_w ||
				s->current.height != tl->placed_h) {
			if (place_toplevel(tl->server, tl)) {
				tl->placed = true;
				tl->placed_w = s->current.width;
				tl->placed_h = s->current.height;
			}
		}
	}
	// 几何 (以及光标下的 resize/标题区) 可能变化; 圆角 FBO 缓存必须跟随任何
	// 内容/几何变化. 提交附带的 damage 会被收集用于局部重绘 (rounded.c).
	rounded_cache_content_commit(tl);
	update_cursor_style(tl->server);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, request_maximize);
	// 早期请求 (首个提交前) 在 map 时应用
	if (tl->xdg_toplevel->base == NULL ||
			!tl->xdg_toplevel->base->initialized) {
		return;
	}
	// 遵循客户端请求的值 (wlroots 存在 requested.maximized):
	// 客户端自己的最大化按钮用 set_maximized(true) 最大化, 用 set_maximized(false) 还原.
	// 过去忽略该值、总是最大化, 使第二次按下变成静默空操作 -
	// 窗口永远无法用自己的按钮还原.
	set_maximized(tl->server, tl, tl->xdg_toplevel->requested.maximized);
}

static void xdg_toplevel_request_minimize(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, request_minimize);
	set_minimized(tl->server, tl, true);
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, request_fullscreen);
	(void)data;
	// 早期请求 (首个提交前) 在 map 时应用
	if (tl->xdg_toplevel->base == NULL ||
			!tl->xdg_toplevel->base->initialized) {
		return;
	}
	set_fullscreen(tl->server, tl, tl->xdg_toplevel->requested.fullscreen);
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, request_move);
	struct server *server = tl->server;
	if (tl->minimized || server->moving || tl->xdg_toplevel->base == NULL ||
			!tl->xdg_toplevel->base->surface->mapped) {
		return;
	}
	// 这里不还原最大化窗口: QQ 这类客户端在普通标题栏点击时也会发
	// xdg_toplevel.move, 那会在没有任何拖动的情况下还原窗口.
	// move_toplevel_to() 改在第一次真正移动时还原, 所以只有实际拖动才会取消最大化
	// (Windows 行为), 单纯点击什么都不做.
	server->move_deferred_restore = true;
	begin_move(server, tl, server->cursor->x, server->cursor->y);
	focus_toplevel(server, tl);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, request_resize);
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct server *server = tl->server;
	if (tl->minimized || server->resizing || server->moving ||
			tl->xdg_toplevel->base == NULL ||
			!tl->xdg_toplevel->base->surface->mapped ||
			tl->xdg_toplevel->current.maximized || tl->fullscreen) {
		return;
	}
	// 客户端自绘装饰的客户端 (如 Chromium) 请求合成器缩放它:
	// 用客户端选择的边缘进入合成器缩放抓取; 指针 motion 驱动 update_resize(),
	// 按钮释放结束抓取, 与合成器自己的边缘手柄完全一样.
	begin_resize(server, tl, event->edges);
}

static void xdg_toplevel_set_title(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, set_title);
	if (tl->fthandle != NULL && tl->xdg_toplevel->title != NULL) {
		wlr_foreign_toplevel_handle_v1_set_title(tl->fthandle,
			tl->xdg_toplevel->title);
	}
}

static void xdg_toplevel_set_app_id(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, set_app_id);
	if (tl->xdg_toplevel->app_id != NULL) {
		free(tl->app_id);
		tl->app_id = strdup(tl->xdg_toplevel->app_id);
		if (tl->fthandle != NULL) {
			wlr_foreign_toplevel_handle_v1_set_app_id(tl->fthandle,
				tl->xdg_toplevel->app_id);
		}
	}
}

// 把 popup 限制进其输出的操作延迟到 popup 提交过一次之后.
// wlr_xdg_popup_unconstrain_from_box() 会调度 configure,
// 而 wlroots 会对尚未初始化 (首次提交) 的 surface 断言 -
// 从 new_popup 处理器里调用它会让 Qt 应用
// (fcitx5-config-qt 主题页、工具提示、下拉框) 打开 popup 时崩溃.
// 一次性 commit 监听器紧跟在角色提交设置 initialized 之后运行,
// 所以限制在 popup 显示之前完成.
struct popup_unconstrain {
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wlr_xdg_popup *popup;
	struct server *server;
};

static void popup_unconstrain_handle_commit(struct wl_listener *listener,
		void *data) {
	struct popup_unconstrain *pu = wl_container_of(listener, pu, commit);
	struct server *server = pu->server;
	// popup 跟随光标, 所以把它限制进光标所在的输出
	// (绝不碰拥有它的 toplevel - 它可能已经消失)
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (output == NULL) {
		output = wlr_output_layout_get_center_output(server->output_layout);
	}
	if (output != NULL) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, output, &box);
		wlr_xdg_popup_unconstrain_from_box(pu->popup, &box);
	}
	wl_list_remove(&pu->commit.link);
	wl_list_remove(&pu->destroy.link);
	free(pu);
}

static void popup_unconstrain_handle_destroy(struct wl_listener *listener,
		void *data) {
	struct popup_unconstrain *pu = wl_container_of(listener, pu, destroy);
	wl_list_remove(&pu->commit.link);
	wl_list_remove(&pu->destroy.link);
	free(pu);
}

// ------------------------------------------------------------------
// popup
// ------------------------------------------------------------------

static void xdg_popup_attach(struct toplevel *tl, struct wlr_xdg_popup *popup,
		struct wlr_scene_tree *parent_tree);
static void xdg_popup_new_popup(struct wl_listener *listener, void *data);

// wayland 的 wl_list_remove 会把 link 置空 (prev/next 变 NULL),
// 对同一监听器第二次 wl_list_remove 会解引用 NULL.
// 只在监听器仍挂在信号上时才移除它.
static void listener_remove_if_attached(struct wl_listener *listener) {
	if (listener->link.prev != NULL) {
		wl_list_remove(&listener->link);
	}
}

// wlr_xdg_popup 角色对象消失 (菜单关闭/抓取取消): 停止响应它的 new popup 和 destroy 信号.
// 结构体本身在场景树销毁时由 xdg_popup_tree_destroy 释放.
static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	struct toplevel_popup *pp = wl_container_of(listener, pp, destroy);
	(void)data;
	listener_remove_if_attached(&pp->new_popup);
	listener_remove_if_attached(&pp->destroy);
}

// popup 的场景树销毁 (popup 的 xdg surface 销毁时 wlroots 会销毁它,
// 或随 toplevel 的树一起): 释放所有监听器和结构体.
// 这是 popup 的唯一所有者 - 它绝不会比树活得久, 所以不会留下悬空监听器.
// 各信号的移除都带判空: popup 角色先死时, xdg_popup_destroy 可能已经摘掉
// new_popup/destroy.
static void xdg_popup_tree_destroy(struct wl_listener *listener, void *data) {
	struct toplevel_popup *pp = wl_container_of(listener, pp, tree_destroy);
	(void)data;
	listener_remove_if_attached(&pp->tree_destroy);
	listener_remove_if_attached(&pp->new_popup);
	listener_remove_if_attached(&pp->destroy);
	free(pp);
}

static void xdg_popup_attach(struct toplevel *tl, struct wlr_xdg_popup *popup,
		struct wlr_scene_tree *parent_tree) {
	struct toplevel_popup *pp = calloc(1, sizeof(*pp));
	if (pp == NULL) {
		return;
	}
	pp->tl = tl;
	pp->popup = popup;

	// 在 popup 首次提交时把它限制进其输出框 (并借此发送首个 configure).
	// 在这里注册, 嵌套 popup (Qt 子菜单) 也能得到, 而不只是 toplevel 级 popup.
	struct popup_unconstrain *pu = calloc(1, sizeof(*pu));
	if (pu != NULL) {
		pu->popup = popup;
		pu->server = tl->server;
		pu->commit.notify = popup_unconstrain_handle_commit;
		wl_signal_add(&popup->base->surface->events.commit, &pu->commit);
		pu->destroy.notify = popup_unconstrain_handle_destroy;
		wl_signal_add(&popup->base->events.destroy, &pu->destroy);
	}

	// wlr_scene_xdg_surface_create 处理 popup surface 及其 subsurface,
	// 并在每次提交时把树定位到 popup->current.geometry.
	// 它还会在 popup 的 xdg-surface destroy 上注册自己的监听器来销毁该树,
	// 所以我们绝不自行销毁 (那会在信号发射中途释放 wlroots 的监听器).
	pp->tree = wlr_scene_xdg_surface_create(parent_tree, popup->base);
	if (pp->tree == NULL) {
		free(pp);
		return;
	}
	// 打标签, 使命中测试在 popup 下仍能解析出所属窗口 (scene.c)
	xdg_surface_tag(pp->tree, TAG_POPUP, popup);

	// pp 的生命周期与树完全相同: 树销毁时 (popup xdg surface 消失,
	// 或 toplevel 的树消失) 下面的监听器释放一切.
	// 在 popup 监听器之前注册它, 使其在销毁时最后运行.
	pp->tree_destroy.notify = xdg_popup_tree_destroy;
	wl_signal_add(&pp->tree->node.events.destroy, &pp->tree_destroy);

	// 嵌套 popup (Qt 子菜单) 挂到该 popup 的树下面
	pp->new_popup.notify = xdg_popup_new_popup;
	wl_signal_add(&popup->base->events.new_popup, &pp->new_popup);
	pp->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&popup->events.destroy, &pp->destroy);
}

static void xdg_popup_new_popup(struct wl_listener *listener, void *data) {
	struct toplevel_popup *pp = wl_container_of(listener, pp, new_popup);
	// 嵌套 popup (Qt 子菜单): 其父场景节点是该 popup 自己的场景树
	xdg_popup_attach(pp->tl, data, pp->tree);
}

static void xdg_toplevel_new_popup(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, new_popup);
	struct wlr_xdg_popup *popup = data;

	// popup 的父是 toplevel surface, 所以其场景树位于 toplevel 的内容树里
	// (内容树原点就是窗口 geometry 左上角, 正是 popup geometry 的相对基准)
	xdg_popup_attach(tl, popup, tl->scene_tree);
}

// ------------------------------------------------------------------
// foreign-toplevel 管理
// ------------------------------------------------------------------

static void foreign_toplevel_request_maximize(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, ft_request_maximize);
	struct wlr_foreign_toplevel_handle_v1_maximized_event *event = data;
	set_maximized(tl->server, tl, event->maximized);
}

static void foreign_toplevel_request_minimize(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, ft_request_minimize);
	struct wlr_foreign_toplevel_handle_v1_minimized_event *event = data;
	set_minimized(tl->server, tl, event->minimized);
}

static void foreign_toplevel_request_activate(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, ft_request_activate);
	struct wlr_foreign_toplevel_handle_v1_activated_event *event = data;
	(void)event;
	if (tl->minimized) {
		set_minimized(tl->server, tl, false);
	}
	focus_toplevel(tl->server, tl);
}

static void foreign_toplevel_request_close(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, ft_request_close);
	close_toplevel(tl);
}

static void foreign_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, ft_destroy);
	wl_list_remove(&tl->ft_request_maximize.link);
	wl_list_remove(&tl->ft_request_minimize.link);
	wl_list_remove(&tl->ft_request_activate.link);
	wl_list_remove(&tl->ft_request_close.link);
	wl_list_remove(&tl->ft_destroy.link);
	tl->fthandle = NULL;
}

void server_new_toplevel(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;
	struct wlr_xdg_surface *base = xdg_toplevel->base;

	struct toplevel *tl = calloc(1, sizeof(*tl));
	if (tl == NULL) {
		return;
	}
	tl->server = server;
	tl->xdg_toplevel = xdg_toplevel;
	tl->decoration_mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE;
	base->data = tl;
	tl->id = ++server->next_window_id;
	tl->app_id = strdup(xdg_toplevel->app_id != NULL
		? xdg_toplevel->app_id : "");

	// 记住客户端 pid, 让状态栏能按 pid 把窗口匹配到托盘项,
	// 并在从任务栏激活窗口时清除消息通知
	tl->pid = 0;
	if (base->client != NULL && base->client->client != NULL) {
		wl_client_get_credentials(base->client->client, &tl->pid, NULL, NULL);
	}

	// xdg surface 树处理 surface、subsurface 及其定位
	tl->scene_tree = wlr_scene_xdg_surface_create(
		server->layers[LAYER_TOPLEVELS], base);
	if (tl->scene_tree == NULL) {
		free(tl->app_id);
		free(tl);
		return;
	}
	xdg_surface_tag(tl->scene_tree, TAG_TOPLEVEL, tl);
	wl_list_init(&tl->subsurfaces);
	tl->rounded = rounded_cache_create(server, tl);

	tl->fthandle = wlr_foreign_toplevel_handle_v1_create(
		server->foreign_toplevel_manager);
	if (tl->fthandle != NULL) {
		tl->fthandle->data = tl;
		if (xdg_toplevel->title != NULL) {
			wlr_foreign_toplevel_handle_v1_set_title(tl->fthandle,
				xdg_toplevel->title);
		}
		if (xdg_toplevel->app_id != NULL) {
			wlr_foreign_toplevel_handle_v1_set_app_id(tl->fthandle,
				xdg_toplevel->app_id);
		}

		tl->ft_request_maximize.notify = foreign_toplevel_request_maximize;
		wl_signal_add(&tl->fthandle->events.request_maximize,
			&tl->ft_request_maximize);
		tl->ft_request_minimize.notify = foreign_toplevel_request_minimize;
		wl_signal_add(&tl->fthandle->events.request_minimize,
			&tl->ft_request_minimize);
		tl->ft_request_activate.notify = foreign_toplevel_request_activate;
		wl_signal_add(&tl->fthandle->events.request_activate,
			&tl->ft_request_activate);
		tl->ft_request_close.notify = foreign_toplevel_request_close;
		wl_signal_add(&tl->fthandle->events.request_close,
			&tl->ft_request_close);
		tl->ft_destroy.notify = foreign_toplevel_destroy;
		wl_signal_add(&tl->fthandle->events.destroy, &tl->ft_destroy);
	}

	tl->map.notify = xdg_toplevel_map;
	wl_signal_add(&base->surface->events.map, &tl->map);
	tl->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&base->surface->events.unmap, &tl->unmap);
	tl->destroy.notify = xdg_surface_destroy;
	wl_signal_add(&base->events.destroy, &tl->destroy);
	tl->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&base->surface->events.commit, &tl->commit);
	tl->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &tl->request_maximize);
	tl->request_minimize.notify = xdg_toplevel_request_minimize;
	wl_signal_add(&xdg_toplevel->events.request_minimize, &tl->request_minimize);
	tl->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen,
		&tl->request_fullscreen);
	tl->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &tl->request_move);
	tl->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &tl->request_resize);
	tl->set_title.notify = xdg_toplevel_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &tl->set_title);
	tl->set_app_id.notify = xdg_toplevel_set_app_id;
	wl_signal_add(&xdg_toplevel->events.set_app_id, &tl->set_app_id);
	tl->new_popup.notify = xdg_toplevel_new_popup;
	wl_signal_add(&base->events.new_popup, &tl->new_popup);
	tl->new_subsurface.notify = xdg_toplevel_new_subsurface;
	wl_signal_add(&base->surface->events.new_subsurface, &tl->new_subsurface);
	// 该监听器加入之前就已存在的 subsurface
	struct wlr_subsurface *subsurface;
	wl_list_for_each(subsurface, &base->surface->current.subsurfaces_below,
			current.link) {
		toplevel_subsurface_add(tl, subsurface);
	}
	wl_list_for_each(subsurface, &base->surface->current.subsurfaces_above,
			current.link) {
		toplevel_subsurface_add(tl, subsurface);
	}

	// toplevel destroy 处理器 (释放 foreign toplevel 句柄并摘掉上面的监听器)
	// 必须在 wlroots 断言 toplevel 信号为空之前运行, 即在 xdg_toplevel->events.destroy 上
	tl->toplevel_destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &tl->toplevel_destroy);

	// 记住 (如果有) 哪个窗口启动了本窗口: 本窗口关闭时焦点回到那个启动窗口.
	// 遍历从本窗口自己的 pid 开始, 使同进程的兄弟窗口
	// (如 Qt 的无父 About 对话框 - 与主窗口同 pid) 胜过进程树祖先
	// (启动该进程的终端): 关闭对话框必须把焦点交回其主窗口, 而不是终端.
	struct toplevel *ancestor = window_ancestor(server, tl->pid);
	tl->after_id = ancestor != NULL ? ancestor->id : 0;

	wl_list_insert(server->toplevels.prev, &tl->link);
}

// ------------------------------------------------------------------
// xdg-decoration
// ------------------------------------------------------------------

// 自己画装饰但自身窗口缩放坏掉的客户端 (无边框 Electron 窗口, 如 QQ):
// 合成器像对无装饰窗口一样接管其边框 (resize 边缘、顶部条、resize 光标),
// 因为客户端自己的边缘缩放从不工作.
// QQ 明确请求客户端侧装饰但无法自行缩放, 所以下面把它的请求覆盖为 NONE.
static bool toplevel_force_undecorated(struct toplevel *tl) {
	if (tl->app_id == NULL) {
		return false;
	}
	// 配置驱动的列表 (见 config.h): 自己画 CSD 边框, 但边缘缩放依赖合成器的客户端.
	// 对 app_id 做大小写不敏感的前缀匹配, 一条前缀即可覆盖多个变体.
	for (size_t i = 0; config_force_undecorated[i] != NULL; i++) {
		size_t len = strlen(config_force_undecorated[i]);
		if (strncasecmp(tl->app_id, config_force_undecorated[i], len) == 0) {
			return true;
		}
	}
	return false;
}

static void decoration_destroy(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, deco_destroy);
	wl_list_remove(&tl->deco_request_mode.link);
	wl_list_remove(&tl->deco_destroy.link);
	tl->decoration = NULL;
}

static void decoration_request_mode(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
	struct toplevel *tl = decoration->toplevel->base->data;
	if (tl == NULL) {
		return;
	}
	tl->decoration_mode = toplevel_force_undecorated(tl)
		? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE
		: decoration->requested_mode;
	if (tl->xdg_toplevel->base != NULL && tl->xdg_toplevel->base->initialized &&
			tl->decoration_mode !=
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE) {
		wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
			tl->decoration_mode);
	}
}

void server_new_decoration(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_decoration);
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
	struct toplevel *tl = decoration->toplevel->base->data;
	if (tl == NULL) {
		return;
	}
	tl->decoration = decoration;
	decoration->data = tl;

	tl->deco_request_mode.notify = decoration_request_mode;
	wl_signal_add(&decoration->events.request_mode, &tl->deco_request_mode);
	tl->deco_destroy.notify = decoration_destroy;
	wl_signal_add(&decoration->events.destroy, &tl->deco_destroy);

	// 默认: 除非客户端明确要求服务端装饰, 否则让客户端自绘
	// (我们从画任何装饰, 顶部 10px 区接管).
	// 实际 configure 在首次提交时发送.
	// 已知自身缩放坏掉的客户端 (无边框 Electron, 如 QQ) 改为 NONE,
	// 让合成器接管其边框.
	tl->decoration_mode = toplevel_force_undecorated(tl)
		? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE
		: WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
}
