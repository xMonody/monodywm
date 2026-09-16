// ipc.c - 通过 Unix 域套接字与状态栏通信
//
// 合成器监听 $XDG_RUNTIME_DIR/xmonodywm.sock (回退 /tmp/xmonodywm.sock).
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

	// 写失败的客户端立即摘链, 结构体则延迟到空闲回调再释放:
	// 释放可能发生在广播循环或该客户端自己的读处理中, 调用方仍持有指针.
	// dead 让后续访问 (queue/flush/handler) 全部变成空操作.
	bool dead;
	struct wl_event_source *destroy_idle;
};

static void ipc_send_window_list(struct server *server,
		struct ipc_client *target);

static void ipc_client_destroy_idle(void *data) {
	struct ipc_client *client = data;
	free(client->out);
	free(client);
}

static void ipc_client_destroy(struct ipc_client *client) {
	if (client->dead) {
		return;
	}
	client->dead = true;
	if (client->source != NULL) {
		wl_event_source_remove(client->source);
		client->source = NULL;
	}
	if (client->fd >= 0) {
		close(client->fd);
		client->fd = -1;
	}
	wl_list_remove(&client->link);
	// 延迟释放: 让广播循环/读处理中仍持有的指针在事件循环空闲前保持有效
	client->destroy_idle = wl_event_loop_add_idle(
		wl_display_get_event_loop(client->server->display),
		ipc_client_destroy_idle, client);
	if (client->destroy_idle == NULL) {
		// 没有空闲源时宁可泄漏, 也不释放调用方可能仍会解引用的内存
		wlr_log(WLR_ERROR, "ipc: failed to schedule client destroy, leaking");
	}
}

// 写出所有排队输出; 成功后清空缓冲区.
// 写失败销毁客户端时返回 false (调用方之后不得再访问该客户端).
static bool ipc_client_flush(struct ipc_client *client) {
	while (client->out_off < client->out_len) {
		ssize_t n = write(client->fd, client->out + client->out_off,
			client->out_len - client->out_off);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return true; // 等待可写事件
			}
			ipc_client_destroy(client);
			return false; // 客户端已消失
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
	if (fcntl(cfd, F_SETFL, O_NONBLOCK | O_CLOEXEC) < 0) {
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
		focused = toplevel_ipc_owner(server, focused);
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
	if (listen(fd, 8) < 0) {
		close(fd);
		return false;
	}
	server->ipc_fd = fd;
	server->ipc_source = wl_event_loop_add_fd(
		wl_display_get_event_loop(server->display), fd, WL_EVENT_READABLE,
		ipc_handle_accept, server);
	if (server->ipc_source == NULL) {
		close(fd);
		server->ipc_fd = -1;
		return false;
	}
	wl_list_init(&server->ipc_clients);
	wlr_log(WLR_INFO, "IPC socket listening on %s", path);
	return true;
}

void ipc_server_destroy(struct server *server) {
	struct ipc_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &server->ipc_clients, link) {
		ipc_client_destroy(client);
	}
	if (server->ipc_source != NULL) {
		wl_event_source_remove(server->ipc_source);
		server->ipc_source = NULL;
	}
	if (server->ipc_fd >= 0) {
		close(server->ipc_fd);
		server->ipc_fd = -1;
	}
}
