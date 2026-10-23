// ipc.c - 通过 Unix 域套接字与状态栏通信
//
// 合成器监听 $XDG_RUNTIME_DIR/monodywm.sock (回退 /tmp/monodywm.sock).
// 状态栏连接后接收以换行分隔的 JSON 消息; 每个窗口用稳定的 id 标识.
//
// 广播给所有客户端的事件: window_added, window_removed, window_focus
// (id 0 = 没有聚焦窗口), window_full, window_list.
// 客户端请求 (每行一个 JSON 对象): list_windows,
// focus_window {"id": N}, close_window {"id": N},
// maximize_window {"id": N} (切换), minimize_window {"id": N}.

#include "ipc.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <wlr/util/log.h>

// 一个已连接的状态栏客户端; JSON 消息以换行分隔
struct ipc_client {
	struct wl_list link; // server.ipc_clients
	struct server *server;
	int fd;
	struct wl_event_source *source;

	char *out;        // 待发送字节
	size_t out_len;   // 已排队的字节数
	size_t out_off;   // 已写出的字节数
	size_t out_cap;

	char in[4096];    // 尚未凑齐一行的输入
	size_t in_len;

	// 写失败的客户端立即关闭 fd 并标记 dead, 结构体则延迟到空闲回调再释放:
	// 释放可能发生在广播循环或该客户端自己的读处理中, 调用方仍持有指针.
	// dead 让后续访问 (queue/flush/handler) 全部变成空操作; link 仍留在
	// server->ipc_clients 里直到空闲回调真正释放, 这样 ipc_server_destroy()
	// 退出时还能遍历到它并直接 free (不会因为 idle 永远不执行而泄漏).
	bool dead;
	struct wl_event_source *destroy_idle;
};

static void ipc_send_window_list(struct server *server,
		struct ipc_client *target);

// 摘掉 fd/event source; 幂等 (destroy 与其后的强制释放都可能调用)
static void ipc_client_close(struct ipc_client *client) {
	if (client->source != NULL) {
		wl_event_source_remove(client->source);
		client->source = NULL;
	}
	if (client->fd >= 0) {
		close(client->fd);
		client->fd = -1;
	}
}

// 真正释放: 先摘掉可能还挂着的延迟释放源, 再从链表里摘除.
// 既用于空闲回调, 也用于 ipc_server_destroy() 的退出路径.
static void ipc_client_free(struct ipc_client *client) {
	if (client->destroy_idle != NULL) {
		wl_event_source_remove(client->destroy_idle);
		client->destroy_idle = NULL;
	}
	if (client->link.prev != NULL) {
		wl_list_remove(&client->link);
	}
	free(client->out);
	free(client);
}

static void ipc_client_destroy_idle(void *data) {
	struct ipc_client *client = data;
	client->destroy_idle = NULL; // idle 源在 dispatch 时已自动摘除
	ipc_client_free(client);
}

static void ipc_client_destroy(struct ipc_client *client) {
	if (client->dead) {
		return;
	}
	client->dead = true;
	ipc_client_close(client);
	// 延迟释放: 让广播循环/读处理中仍持有的指针在事件循环空闲前保持有效
	client->destroy_idle = wl_event_loop_add_idle(
		wl_display_get_event_loop(client->server->display),
		ipc_client_destroy_idle, client);
	if (client->destroy_idle == NULL) {
		// 没有空闲源: 先保留结构体 (其 link 仍在链表里), 退出时由
		// ipc_server_destroy() 的强制释放回收
		wlr_log(WLR_ERROR, "ipc: failed to schedule client destroy");
	}
}

// 写出所有排队输出; 成功后清空缓冲区.
// 写失败销毁客户端时返回 false (调用方之后不得再访问该客户端).
static bool ipc_client_flush(struct ipc_client *client) {
	while (client->out_off < client->out_len) {
		ssize_t n = write(client->fd, client->out + client->out_off,
			client->out_len - client->out_off);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return true; // 等待可写事件
			}
			ipc_client_destroy(client);
			return false; // 客户端已消失
		}
		if (n == 0) {
			// 非阻塞 socket 上 write() 返回 0 不应发生; 若按 0 继续
			// 循环, out_off 不前进, 事件循环会被卡死.
			ipc_client_destroy(client);
			return false;
		}
		client->out_off += n;
	}
	client->out_len = 0;
	client->out_off = 0;
	return true;
}

static void ipc_client_queue(struct ipc_client *client, const char *json) {
	if (client->dead) {
		return;
	}
	size_t len = strlen(json);
	if (client->out_len + len + 1 > client->out_cap) {
		size_t cap = client->out_cap ? client->out_cap * 2 : 256;
		while (cap < client->out_len + len + 1) {
			cap *= 2;
		}
		char *buf = realloc(client->out, cap);
		if (buf == NULL) {
			return;
		}
		client->out = buf;
		client->out_cap = cap;
	}
	memcpy(client->out + client->out_len, json, len);
	client->out_len += len;
	client->out[client->out_len++] = '\n';
	// flush 可能销毁客户端 (向已断开的栏写失败): 那种情况下不要再访问它
	if (!ipc_client_flush(client)) {
		return;
	}
	// 还有待发送输出时保持可写事件开启
	uint32_t mask = WL_EVENT_READABLE;
	if (client->out_off < client->out_len) {
		mask |= WL_EVENT_WRITABLE;
	}
	wl_event_source_fd_update(client->source, mask);
}

// 处理客户端发来的一条完整 JSON 消息
static void ipc_handle_line(struct server *server, struct ipc_client *client,
		const char *line) {
	cJSON *root = cJSON_Parse(line);
	if (root == NULL) {
		return;
	}
	cJSON *action = cJSON_GetObjectItem(root, "action");
	if (action != NULL && cJSON_IsString(action)) {
		if (strcmp(action->valuestring, "list_windows") == 0) {
			ipc_send_window_list(server, client);
		} else if (strcmp(action->valuestring, "focus_window") == 0) {
			// 任务栏点击了窗口图标: 切换焦点过去
			cJSON *id = cJSON_GetObjectItem(root, "id");
			if (id != NULL && cJSON_IsNumber(id)) {
				struct toplevel *tl =
					toplevel_by_id(server, (int)id->valuedouble);
				if (tl != NULL) {
					focus_window(server, tl);
				}
			}
		} else if (strcmp(action->valuestring, "close_window") == 0) {
			cJSON *id = cJSON_GetObjectItem(root, "id");
			if (id != NULL && cJSON_IsNumber(id)) {
				struct toplevel *tl =
					toplevel_by_id(server, (int)id->valuedouble);
				if (tl != NULL) {
					close_toplevel(tl);
				}
			}
		} else if (strcmp(action->valuestring, "maximize_window") == 0) {
			// 切换: 未最大化则最大化, 已最大化则还原
			cJSON *id = cJSON_GetObjectItem(root, "id");
			if (id != NULL && cJSON_IsNumber(id)) {
				struct toplevel *tl =
					toplevel_by_id(server, (int)id->valuedouble);
				if (tl != NULL) {
					set_maximized(server, tl,
						!tl->xdg_toplevel->current.maximized);
				}
			}
		} else if (strcmp(action->valuestring, "minimize_window") == 0) {
			cJSON *id = cJSON_GetObjectItem(root, "id");
			if (id != NULL && cJSON_IsNumber(id)) {
				struct toplevel *tl =
					toplevel_by_id(server, (int)id->valuedouble);
				if (tl != NULL) {
					set_minimized(server, tl, true);
				}
			}
		}
	}
	cJSON_Delete(root);
}

static void ipc_client_handle_input(struct server *server,
		struct ipc_client *client, const char *data, size_t len) {
	size_t i = 0;
	while (i < len) {
		while (i < len && data[i] != '\n' &&
				client->in_len < sizeof(client->in) - 1) {
			client->in[client->in_len++] = data[i++];
		}
		if (i < len && data[i] == '\n') {
			client->in[client->in_len] = '\0';
			if (client->in_len > 0) {
				ipc_handle_line(server, client, client->in);
				// 处理器里的广播可能已摘掉本客户端; 结构体仍有效, 但停止解析其输入
				if (client->dead) {
					return;
				}
			}
			client->in_len = 0;
			i++;
		} else if (i < len) {
			// 内层循环因缓冲区满而退出 (行超过 sizeof(client->in)):
			// 丢弃该行剩余部分直到换行符. 没有这一步 i 不再前进,
			// 外层 while 会永远自旋 (一条超长无换行的输入即可卡死合成器).
			while (i < len && data[i] != '\n') {
				i++;
			}
			client->in_len = 0;
		}
	}
}

static int ipc_client_handle_data(int fd, uint32_t mask, void *data) {
	struct ipc_client *client = data;
	struct server *server = client->server;
	if ((mask & WL_EVENT_WRITABLE) != 0) {
		if (!ipc_client_flush(client)) {
			return 0; // 写失败已销毁客户端
		}
		uint32_t new_mask = WL_EVENT_READABLE;
		if (client->out_off < client->out_len) {
			new_mask |= WL_EVENT_WRITABLE;
		}
		wl_event_source_fd_update(client->source, new_mask);
	}
	if ((mask & WL_EVENT_READABLE) != 0) {
		char buf[4096];
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n < 0 && errno == EINTR) {
			return 0; // 被信号打断: 下次可读事件再试
		}
		if (n <= 0) {
			ipc_client_destroy(client);
			return 0;
		}
		ipc_client_handle_input(server, client, buf, (size_t)n);
	}
	return 0;
}

static int ipc_handle_accept(int fd, uint32_t mask, void *data) {
	struct server *server = data;
	if ((mask & WL_EVENT_READABLE) == 0) {
		return 0;
	}
	int cfd = accept(fd, NULL, NULL);
	if (cfd < 0) {
		return 0;
	}
	// F_SETFL 只处理文件状态标志; O_CLOEXEC 是 fd 标志, 必须用 F_SETFD.
	// 否则 fork/exec 出来的应用会继承状态栏 IPC 连接, 状态栏退出后合成器
	// 也检测不到断开.
	if (fcntl(cfd, F_SETFL, O_NONBLOCK) < 0 ||
			fcntl(cfd, F_SETFD, FD_CLOEXEC) < 0) {
		close(cfd);
		return 0;
	}
	struct ipc_client *client = calloc(1, sizeof(*client));
	if (client == NULL) {
		close(cfd);
		return 0;
	}
	client->fd = cfd;
	client->server = server;
	client->source = wl_event_loop_add_fd(
		wl_display_get_event_loop(server->display), cfd, WL_EVENT_READABLE,
		ipc_client_handle_data, client);
	if (client->source == NULL) {
		close(cfd);
		free(client);
		return 0;
	}
	wl_list_insert(server->ipc_clients.prev, &client->link);
	// 新客户端需要当前窗口列表来绘制状态栏
	ipc_send_window_list(server, client);
	return 0;
}

// 序列化并向所有已连接客户端广播一条窗口事件
void ipc_send_window_event(struct server *server, const char *event,
		struct toplevel *tl) {
	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return;
	}
	cJSON_AddStringToObject(root, "event", event);
	if (tl != NULL) {
		cJSON_AddNumberToObject(root, "id", tl->id);
		cJSON_AddStringToObject(root, "app_id",
			tl->app_id != NULL ? tl->app_id : "");
		cJSON_AddNumberToObject(root, "pid", (double)tl->pid);
	} else {
		// 焦点清空: id 0, 无窗口
		cJSON_AddNumberToObject(root, "id", 0);
		cJSON_AddStringToObject(root, "app_id", "");
		cJSON_AddNumberToObject(root, "pid", 0);
	}
	char *json = cJSON_PrintUnformatted(root);
	if (json != NULL) {
		struct ipc_client *client, *tmp;
		wl_list_for_each_safe(client, tmp, &server->ipc_clients, link) {
			ipc_client_queue(client, json);
		}
		free(json);
	}
	cJSON_Delete(root);
}

// 发送当前已映射窗口; target 为 NULL 时广播给所有人
static void ipc_send_window_list(struct server *server,
		struct ipc_client *target) {	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return;
	}
	cJSON_AddStringToObject(root, "event", "window_list");
	cJSON *arr = cJSON_CreateArray();
	cJSON_AddItemToObject(root, "windows", arr);

	// 按创建顺序: 新窗口总是追加到任务栏末尾, 与启动方式无关
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		// 对话框/弹窗没有 IPC id: 重连时也不能让它们重新冒出来
		if (!tl->ipc_added || tl->xdg_toplevel->base == NULL ||
				!tl->xdg_toplevel->base->surface->mapped) {
			continue;
		}
		cJSON *w = cJSON_CreateObject();
		cJSON_AddNumberToObject(w, "id", tl->id);
		cJSON_AddStringToObject(w, "app_id",
			tl->app_id != NULL ? tl->app_id : "");
		cJSON_AddNumberToObject(w, "pid", (double)tl->pid);
		cJSON_AddItemToArray(arr, w);
	}
	// 告诉 (新的) 状态栏当前聚焦窗口, 让高亮立即显示;
	// 0 = 没有聚焦窗口 (或聚焦窗口已隐藏/未映射, 例如最小化)
	int focused_id = 0;
	struct toplevel *focused = server->focused;
	if (focused != NULL) {
		// 对话框不占任务栏条目: 高亮其主窗口, 与 window_focus 保持一致
		focused = toplevel_ipc_owner(focused);
	}
	if (focused != NULL && focused->xdg_toplevel->base != NULL &&
			focused->xdg_toplevel->base->surface->mapped) {
		focused_id = focused->id;
	}
	cJSON_AddNumberToObject(root, "focused_id", focused_id);
	char *json = cJSON_PrintUnformatted(root);
	if (json != NULL) {
		if (target != NULL) {
			ipc_client_queue(target, json);
		} else {
			struct ipc_client *client, *tmp;
			wl_list_for_each_safe(client, tmp, &server->ipc_clients, link) {
				ipc_client_queue(client, json);
			}
		}
		free(json);
	}
	cJSON_Delete(root);
}

// 创建 Unix 套接字; 失败返回 false (合成器仍会运行, 只是没有状态栏)
bool ipc_server_init(struct server *server, const char *path) {
	// 先初始化客户端链表: 之后任何提前 return (套接字/绑定失败) 都会让
	// ipc_send_window_event()/ipc_server_destroy() 仍能安全遍历它,
	// 而不是在未初始化的头结点上解引用 NULL.
	wl_list_init(&server->ipc_clients);

	unlink(path);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		return false;
	}
	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(addr.sun_path)) {
		close(fd);
		return false;
	}
	strcpy(addr.sun_path, path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return false;
	}
	// bind 成功后套接字文件已经存在: 之后任何失败都要 unlink, 否则留下
	// 一个无人监听的 socket 文件 (下次 connect 会拿到 ECONNREFUSED)
	if (listen(fd, 8) < 0) {
		close(fd);
		unlink(path);
		return false;
	}
	server->ipc_fd = fd;
	snprintf(server->ipc_path, sizeof(server->ipc_path), "%s", path);
	server->ipc_source = wl_event_loop_add_fd(
		wl_display_get_event_loop(server->display), fd, WL_EVENT_READABLE,
		ipc_handle_accept, server);
	if (server->ipc_source == NULL) {
		close(fd);
		server->ipc_fd = -1;
		unlink(path);
		server->ipc_path[0] = '\0';
		return false;
	}
	wlr_log(WLR_INFO, "IPC socket listening on %s", path);
	return true;
}

void ipc_server_destroy(struct server *server) {
	// 退出时直接释放, 不走延迟释放的 idle: 调用方是 main() 的拆除路径,
	// 之后马上就会销毁事件循环, idle 永远不会被 dispatch (否则就漏了).
	// 广播/读处理中的临时指针问题在正常运行时由 dead 标记 + idle 保证,
	// 这里已经没有任何 handler 在跑.
	struct ipc_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &server->ipc_clients, link) {
		client->dead = true;
		ipc_client_close(client);
		ipc_client_free(client);
	}
	if (server->ipc_source != NULL) {
		wl_event_source_remove(server->ipc_source);
		server->ipc_source = NULL;
	}
	if (server->ipc_fd >= 0) {
		close(server->ipc_fd);
		server->ipc_fd = -1;
	}
	if (server->ipc_path[0] != '\0') {
		unlink(server->ipc_path);
		server->ipc_path[0] = '\0';
	}
}
