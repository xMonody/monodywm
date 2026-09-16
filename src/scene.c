// scene.c - 窗口/图层模块共用的场景图辅助
//
// 每个有意义的场景节点都在 node.data 里放一个 struct scene_tag,
// 这样从任意命中测试到的节点向上遍历父节点, 就能找到所属对象
// (toplevel / layer surface).

#include "server.h"

#include <stdlib.h>

static void scene_tag_destroy(struct wl_listener *listener, void *data) {
	struct scene_tag *tag = wl_container_of(listener, tag, destroy);
	wl_list_remove(&tag->destroy.link);
	free(tag);
}

void xdg_surface_tag(struct wlr_scene_tree *tree,
		enum scene_tag_type type, void *ptr) {
	struct scene_tag *tag = calloc(1, sizeof(*tag));
	if (tag == NULL) {
		return;
	}
	tag->type = type;
	tag->ptr = ptr;
	tag->destroy.notify = scene_tag_destroy;
	wl_signal_add(&tree->node.events.destroy, &tag->destroy);
	tree->node.data = tag;
}

// 找到给定布局坐标下带标签的对象
void *scene_tag_at(struct server *server, enum scene_tag_type type,
		double lx, double ly) {
	double sx, sy;
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, &sx, &sy);
	if (node == NULL) {
		return NULL;
	}
	struct wlr_scene_node *n = node;
	while (n != NULL) {
		if (n->data != NULL) {
			struct scene_tag *tag = n->data;
			if (tag->type == type) {
				return tag->ptr;
			}
			if (tag->type != TAG_POPUP) {
				return NULL; // 更近的带标签对象赢得命中测试
			}
			// popup 属于某个 toplevel: 继续向上找, 这样菜单打开时
			// 仍能找到所属窗口
		}
		n = n->parent != NULL ? &n->parent->node : NULL;
	}
	return NULL;
}

struct toplevel *toplevel_at(struct server *server) {
	return toplevel_morph_at(server, server->cursor->x, server->cursor->y);
}

// toplevel a 的场景树是否叠在 b 之上
// (图层的子节点自下而上排列)
static bool toplevel_tree_above(struct server *server, struct toplevel *a,
		struct toplevel *b) {
	struct wlr_scene_tree *layer = server->layers[LAYER_TOPLEVELS];
	struct wlr_scene_node *na = a->scene_tree != NULL ?
		&a->scene_tree->node : NULL;
	struct wlr_scene_node *nb = b->scene_tree != NULL ?
		&b->scene_tree->node : NULL;
	if (na == NULL || nb == NULL || na->parent != layer ||
			nb->parent != layer) {
		return false;
	}
	struct wlr_scene_node *child;
	wl_list_for_each_reverse(child, &layer->children, link) {
		if (child == na) {
			return true;  // a 在 b 之上
		}
		if (child == nb) {
			return false; // b 在 a 之上
		}
	}
	return false;
}

// 给定坐标下"实际绘制"的 toplevel, 即其可见内容占据指针的窗口.
// 当窗口正在形变 (animate.c: 最大化/还原缩放, 或打开/关闭淡入淡出的缩放部分) 时,
// 它位置上唯一可见的是按 tl->morph_* 缩放后的圆角 FBO;
// 场景的原始命中测试只认识自然尺寸下的客户端 surface.
// 因此光标落在形变框内时就属于该形变窗口, 即使原始场景命中落在
// 其下方堆叠的窗口上 (还原/缩小缩放会把窗口画得比已提交内容更大);
// 形变窗口只输给堆叠在其上、且自身内容覆盖该点的窗口 (那个窗口确实画在上面).
// 这里不查 layer-shell 和 popup: 它们位于 toplevel 图层之上, 调用方
// (pointer.c) 已经让它们优先于任何窗口.
struct toplevel *toplevel_morph_at(struct server *server, double lx,
		double ly) {
	// 快速路径: 没有窗口形变, 直接用原始场景命中
	bool any_morph = false;
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		if (tl->morph_active) {
			any_morph = true;
			break;
		}
	}
	if (!any_morph) {
		return scene_tag_at(server, TAG_TOPLEVEL, lx, ly);
	}
	// layer-shell surface (状态栏/菜单覆盖层) 或 popup 覆盖该点:
	// 它们浮在窗口之上, 不会被形变窗口挤走
	{
		double sx, sy;
		struct wlr_scene_node *node = wlr_scene_node_at(
			&server->scene->tree.node, lx, ly, &sx, &sy);
		while (node != NULL && node->data == NULL) {
			node = node->parent != NULL ? &node->parent->node : NULL;
		}
		if (node != NULL) {
			struct scene_tag *tag = node->data;
			if (tag->type == TAG_LAYER || tag->type == TAG_POPUP) {
				return NULL;
			}
		}
	}
	struct toplevel *raw = scene_tag_at(server, TAG_TOPLEVEL, lx, ly);
	struct toplevel *best = NULL;
	wl_list_for_each(tl, &server->toplevels, link) {
		if (!tl->morph_active || tl->morph_w <= 0 || tl->morph_h <= 0 ||
				tl->scene_tree == NULL ||
				!tl->scene_tree->node.enabled || tl->minimized ||
				tl->xdg_toplevel == NULL ||
				tl->xdg_toplevel->base == NULL ||
				!tl->xdg_toplevel->base->surface->mapped) {
			continue;
		}
		if (lx < tl->morph_x || lx >= tl->morph_x + tl->morph_w ||
				ly < tl->morph_y || ly >= tl->morph_y + tl->morph_h) {
			continue; // 只有可见的 (形变) 框才占据指针
		}
		// 叠在形变窗口之上的窗口用自身 (原始) 内容盖住该点:
		// 它确实画在上面, 保留命中
		if (raw != NULL && raw != tl && toplevel_tree_above(server, raw,
				tl)) {
			continue;
		}
		if (best == NULL || toplevel_tree_above(server, tl, best)) {
			best = tl;
		}
	}
	return best;
}

// 光标是否位于 popup (菜单/下拉框/工具提示) surface 上?
// popup 浮在其父 toplevel 之上, 盖住窗口边框的 popup 赢得指针:
// 此时合成器的边框抓取 (resize 边缘、标题栏) 被禁用, 点击到达 popup
// 而不是触发缩放. scene_tag_at() 会特意越过 TAG_POPUP 去找所属窗口,
// 所以这里用显式的命中测试来检测 popup 本身.
bool pointer_over_popup(struct server *server) {
	double sx, sy;
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, server->cursor->x, server->cursor->y,
		&sx, &sy);
	if (node == NULL) {
		return false;
	}
	struct wlr_scene_node *n = node;
	while (n != NULL) {
		if (n->data != NULL) {
			struct scene_tag *tag = n->data;
			return tag->type == TAG_POPUP;
		}
		n = n->parent != NULL ? &n->parent->node : NULL;
	}
	return false;
}

// 光标是否位于 layer-shell surface (状态栏、菜单覆盖层等) 上?
// 它们叠在窗口之上, 所以光标在其上时合成器不能在下方窗口上开始
// 移动/缩放抓取 - 点击必须到达 layer surface (任务栏按钮、菜单项).
bool pointer_over_layer_surface(struct server *server) {
	double sx, sy;
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, server->cursor->x, server->cursor->y,
		&sx, &sy);
	if (node == NULL) {
		return false;
	}
	struct wlr_scene_node *n = node;
	while (n != NULL) {
		if (n->data != NULL) {
			struct scene_tag *tag = n->data;
			if (tag->type == TAG_LAYER) {
				return true;
			}
		}
		n = n->parent != NULL ? &n->parent->node : NULL;
	}
	return false;
}
