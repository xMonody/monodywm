/*
 * scene.c - scene-graph helpers shared by the window / layer modules
 *
 * Every interesting scene node carries a struct scene_tag in node.data so
 * the compositor can find the owning object (toplevel, layer surface) from
 * an arbitrary hit-tested node by walking up the parent chain.
 */

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

/* find the tagged object under the given layout coordinates */
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
				return NULL; /* a closer tagged object won the hit test */
			}
			/* a popup belongs to a toplevel: keep walking up so the
			 * owning window is still found under an open menu */
		}
		n = n->parent != NULL ? &n->parent->node : NULL;
	}
	return NULL;
}

struct toplevel *toplevel_at(struct server *server) {
	return toplevel_morph_at(server, server->cursor->x, server->cursor->y);
}

/* is toplevel a's scene tree stacked above toplevel b's in the toplevel
 * layer (the layer's children are listed bottom to top)? */
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
			return true;  /* a is above b */
		}
		if (child == nb) {
			return false; /* b is above a */
		}
	}
	return false;
}

/* The toplevel *drawn* under (lx, ly), i.e. the window whose visible
 * content claims the pointer.  While a window morphs (animate.c:
 * maximize/restore zoom, or the scale part of an open/close fade) the only
 * thing on screen at its position is its rounded FBO scaled into
 * tl->morph_*; the scene's raw hit test only knows the client surface at
 * its natural geometry.  A cursor inside the morph box therefore belongs
 * to the morphing window even when the raw scene hit landed on a window
 * stacked below it (restore / shrink zoom draws the window larger than its
 * committed content), and a morphing window loses only to a window stacked
 * above it whose own content covers the point (that window really is drawn
 * on top).  Layer-shell surfaces and popups are not consulted here: they
 * sit above the toplevel layer and callers (pointer.c) already prefer
 * them over any window. */
struct toplevel *toplevel_morph_at(struct server *server, double lx,
		double ly) {
	/* fast path: no window morphs - the raw scene hit stands */
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
	/* a layer-shell surface (bar / menu overlay) or a popup covers the
	 * point: it floats above the windows and is never displaced by a
	 * morphing window */
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
			continue; /* only the visible (morph) box claims the pointer */
		}
		/* a window stacked above the morphing one covers the point with
		 * its own (raw) content: it is drawn above it and keeps the hit */
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

/* is the cursor over a popup (menu / dropdown / tooltip) surface?  A popup
 * floats above its parent toplevel, so a popup covering the window's border
 * wins the pointer: the compositor's frame grabs (resize edges, title
 * strip) are disabled there and the click reaches the popup instead of
 * starting a resize.  scene_tag_at() deliberately walks up past TAG_POPUP
 * to find the owning window, so the popup itself is detected with an
 * explicit hit test instead. */
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

/* is the cursor over a layer-shell surface (bar, menu overlay, ...)?  These
 * sit above the windows, so while the cursor is on one the compositor must
 * not start a move/resize grab on the window below - the click has to reach
 * the layer surface (taskbar buttons, menu items) instead. */
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
