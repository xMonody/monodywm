// ipc.h - 状态栏 IPC 套接字 (Unix 域套接字上的 JSON)

#ifndef XMONODYWM_IPC_H
#define XMONODYWM_IPC_H

#include "server.h"

// 创建 Unix 套接字; 失败返回 false (合成器仍会运行, 只是没有状态栏)
bool ipc_server_init(struct server *server, const char *path);

void ipc_server_destroy(struct server *server);

// 序列化并向所有已连接客户端广播一条窗口事件
void ipc_send_window_event(struct server *server, const char *event,
	struct toplevel *tl);

#endif // XMONODYWM_IPC_H
