/*
 * toplevel.c - xdg-shell toplevel windows
 *
 * Window lifecycle (map/unmap/commit/destroy), window state (maximize,
 * minimize, fullscreen, move, close), xdg-decoration mode negotiation and
 * the foreign-toplevel handle that mirrors each window for taskbars.
 *
 * The compositor draws no window decorations of its own: client-side
 * decorated windows keep their native controls, and undecorated windows
 * get only the compositor's invisible grab zones (title strip and resize
 * edges) handled in pointer.c.
 */

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

/* Parent process id of pid, read from /proc/<pid>/stat; 0 on failure. */
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
	/* comm is in parentheses and may contain spaces and ')': the last ')'
	 * is the delimiter, followed by " state ppid ..." */
	char *close = strrchr(buf, ')');
	if (close == NULL)
		return 0;
	char state = 0;
	pid_t ppid = 0;
	if (sscanf(close + 2, "%c %d", &state, &ppid) != 2)
		return 0;
	return ppid;
}

/* Nearest ancestor window of pid by walking up the process tree (a window
 * launched from a terminal is a descendant of the terminal).  Prefers the
 * focused window when several windows share the same pid.  Returns NULL when
 * no ancestor is a window (launched by the compositor, the bar, or after the
 * launching process has daemonized/reparented to init). */
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

/* effective window geometry box in layout coordinates: the xdg window
 * geometry (the window bounds per xdg-shell, excluding CSD margins/drop
 * shadows).  The xdg scene tree is anchored at this box's top-left corner. */
void toplevel_box(struct toplevel *tl, struct wlr_box *box) {
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	box->x = tl->scene_tree->node.x;
	box->y = tl->scene_tree->node.y;
	box->width = base != NULL ? base->geometry.width : 0;
	box->height = base != NULL ? base->geometry.height : 0;
}

/* a dialog / transient window: one that declared a parent toplevel via
 * xdg_toplevel.set_parent (GTK/Qt dialogs do this).  wlroots keeps
 * toplevel->parent updated (including clearing it when the parent
 * unmaps), so a live check is always current. */
bool toplevel_is_dialog(struct toplevel *tl) {
	return tl->xdg_toplevel != NULL && tl->xdg_toplevel->parent != NULL;
}

/* a window pinned to a fixed size via min == max constraints (e.g. QQ's
 * login window, splash screens): it cannot meaningfully be resized,
 * maximized or minimized.  A dimension is fixed only when min > 0 and
 * equals max (min=0/max=0 means "unconstrained"). */
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

/* output under the toplevel (its center), or the center output */
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

/* clamp a position so the window never ends up underneath a layer-shell
 * bar's exclusive zone: the top-left corner goes into the work area (and
 * at least 40 px stay visible if the window is bigger than the area) */
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
	/* right edge: a bar there clamps the window flush to the work area
	 * edge (never underneath it); without a bar at least 40 px stay
	 * visible when the window is bigger than the area */
	if (area.x + area.width < out.x + out.width) { /* bar at the right */
		if (*x + width > area.x + area.width) {
			*x = area.x + area.width - width;
		}
	} else if (*x + 40 > area.x + area.width) {
		*x = area.x + area.width - 40;
	}
	/* bottom edge: same rule - a bar there clamps the window's bottom
	 * to the work area edge, so a restored/mapped window can never slide
	 * underneath the bar */
	if (area.y + area.height < out.y + out.height) { /* bar at the bottom */
		if (*y + height > area.y + area.height) {
			*y = area.y + area.height - height;
		}
	} else if (*y + 40 > area.y + area.height) {
		*y = area.y + area.height - 40;
	}
}

/* restore a maximized/fullscreen window to its saved spot, Windows style:
 * the position is restored exactly - including when it hangs partly off
 * the screen, which the user placed there deliberately.  Only two safety
 * nets apply, and neither moves a merely half-off-screen window:
 *   - a spot completely outside every output is unreachable, so the
 *     window is brought back into the work area;
 *   - a layer-shell bar that appeared while the window was zoomed (its
 *     exclusive zone shrank the work area) must not occlude the window:
 *     it is nudged out of the bar strip on that side only. */
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

	/* fully outside every output: pull the top-left into the work area */
	struct wlr_box b = { *x, *y, box->width, box->height };
	struct wlr_box inter;
	if (!wlr_box_intersection(&inter, &b, &out)) {
		*x = area.x;
		*y = area.y;
		return;
	}

	/* nudge out from under a bar that now exists on that side (a window
	 * drag already refuses to slide a window under the bars) */
	if (area.y > out.y && *y < area.y) { /* top bar */
		*y = area.y;
	}
	if (area.x > out.x && *x < area.x) { /* left bar */
		*x = area.x;
	}
	if (area.x + area.width < out.x + out.width &&
			*x + box->width > area.x + area.width) { /* right bar */
		*x = area.x + area.width - box->width;
	}
	if (area.y + area.height < out.y + out.height &&
			*y + box->height > area.y + area.height) { /* bottom bar */
		*y = area.y + area.height - box->height;
	}
}

/* geometry of a maximized window: the work area exactly (the window fills
 * the area, flush against any layer-shell bars' exclusive zones) */
void maximized_box(struct server *server, struct wlr_output *output,
		struct wlr_box *box) {
	get_work_area(server, output, box);
}

/* geometry of a fullscreen window: the whole output box (fullscreen covers
 * layer-shell bars as well) */
static void fullscreen_box(struct server *server, struct wlr_output *output,
		struct wlr_box *box) {
	wlr_output_layout_get_box(server->output_layout, output, box);
}

/* after a layer-shell exclusive-zone change, existing windows that now sit
 * under the bar are moved back into the work area (the bar must never
 * cover them), and maximized windows are re-fitted to the new area */
void arrange_toplevels_work_area(struct server *server,
		struct wlr_output *output) {
	struct wlr_box area;
	get_work_area(server, output, &area);
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
		if (base == NULL || !base->surface->mapped || tl->minimized ||
				tl->closing || tl->fullscreen) {
			/* fullscreen windows deliberately cover the whole output,
			 * bars included */
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
			/* re-fit the maximized window to the (possibly shrunk) area */
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

/* the next/previous mapped toplevel relative to tl, wrapping around the
 * list. include_minimized controls whether hidden windows are eligible.
 * Returns NULL when there is no such toplevel. */
struct toplevel *neighbor_toplevel(struct server *server,
		struct toplevel *tl, bool next, bool include_minimized) {
	struct wl_list *head = &server->toplevels;
	struct wl_list *cur = &tl->link;
	struct wl_list *iter = next ? cur->next : cur->prev;
	while (iter != cur) {
		if (iter == head) {
			/* wrap around the circular list */
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

/* find a live toplevel by its IPC id (shared with ipc.c) */
struct toplevel *toplevel_by_id(struct server *server, int id) {
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		if (tl->id == id) {
			return tl;
		}
	}
	return NULL;
}

/* the window that should receive focus when the focused toplevel `tl` goes
 * away: a dialog hands focus back to its xdg parent (the main window it
 * belongs to), otherwise prefer the window that launched it (after_id,
 * which prefers a same-process sibling window over a terminal), then fall
 * back to the previous visible window in creation order. */
static struct toplevel *focus_fallback(struct server *server,
		struct toplevel *tl) {
	/* a transient/dialog window declared its parent via
	 * xdg_toplevel.set_parent: when it closes, focus must return to that
	 * parent main window, not to the window that launched the dialog (e.g.
	 * fcitx5-config run from a terminal - the terminal is the launcher of
	 * the main window, but closing the dialog should keep focus on the
	 * main window). */
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

/* ------------------------------------------------------------------ */
/* window state                                                       */
/* ------------------------------------------------------------------ */

void close_toplevel(struct toplevel *tl) {
	if (tl->xdg_toplevel->base == NULL) {
		return;
	}
	if (tl->closing) {
		return; /* a close is already pending; the fade owns the request */
	}
	if (animate_toplevel_close(tl)) {
		/* the fade-out is running and sends the close request once the
		 * window is invisible (animate.c).  The close is now sticky: the
		 * window turns inert until it actually goes away - it cannot be
		 * re-focused, minimized, maximized or restored, so no later user
		 * action can silently cancel the close - and once the fade
		 * finishes, animate.c disables its scene node, so an uncooperative
		 * client can never leave an invisible, input-blocking window. */
		struct server *server = tl->server;
		tl->closing = true;
		if (server->focused == tl) {
			/* hand keyboard focus to the window that would receive it when
			 * this one dies, right away (like a minimize does): input must
			 * never keep targeting a window that is fading away */
			struct toplevel *prev = focus_fallback(server, tl);
			server->focused = NULL;
			if (tl->xdg_toplevel->base != NULL) {
				wlr_xdg_toplevel_set_activated(tl->xdg_toplevel, false);
			}
			wlr_seat_keyboard_clear_focus(server->seat);
			if (prev != NULL) {
				focus_toplevel(server, prev);
			} else {
				ipc_send_window_event(server, "window_focus", NULL);
				ime_set_focus(server, NULL);
			}
			/* the fade must stay visible above the newly focused window
			 * (mirrors the minimize drop, which raises the falling window
			 * after focus moved on) */
			if (tl->scene_tree != NULL) {
				wlr_scene_node_raise_to_top(&tl->scene_tree->node);
			}
		}
		return;
	}
	/* no fade ran (animations disabled, or the window is hidden): close
	 * right away */
	wlr_xdg_toplevel_send_close(tl->xdg_toplevel);
}

void set_fullscreen(struct server *server, struct toplevel *tl,
		bool fullscreen) {
	if (tl->closing || tl->xdg_toplevel->base == NULL ||
			tl->xdg_toplevel->current.fullscreen == fullscreen) {
		return;
	}
	if (tl->morph_active) {
		/* a maximize/restore zoom is running: let fullscreen take over
		 * cleanly instead of fighting the running zoom */
		animate_toplevel_abort_geometry(tl);
	}
	tl->user_moved = true; /* fullscreen state: stop auto-centering */
	tl->fullscreen = fullscreen;
	/* border width/color depend on the fullscreen state */
	rounded_cache_dirty(tl);
	if (fullscreen) {
		/* remember the pre-fullscreen geometry so it can be restored
		 * later.  Keep this separate from restore_box: entering fullscreen
		 * from a maximized window must not overwrite the floating geometry
		 * that maximize saved. */
		toplevel_box(tl, &tl->fullscreen_restore_box);
		tl->has_fullscreen_restore_box = true;

		struct wlr_output *output = toplevel_output(server, tl);
		if (output != NULL) {
			struct wlr_box fbox;
			fullscreen_box(server, output, &fbox);
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel, fbox.width,
				fbox.height);
			/* Windows-style zoom into the fullscreen box (animate.c):
			 * keep showing the floating window until the client commits the
			 * fullscreen content, then scale it up.  Falls back to an
			 * instant jump when the zoom cannot run. */
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
			/* back exactly where it was; a bar that appeared meanwhile is
			 * the only thing that may move it (restore_box_position) */
			restore_box_position(server, &tl->fullscreen_restore_box,
				&x, &y);
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel,
				tl->fullscreen_restore_box.width,
				tl->fullscreen_restore_box.height);
			/* Windows-style zoom back to the floating box (animate.c),
			 * same as maximize restore: keep the fullscreen window on
			 * screen until the client commits the restored content, then
			 * scale it down into place */
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
	/* notify status bars */
	ipc_send_window_event(server, "window_full", tl);
}

void set_maximized(struct server *server, struct toplevel *tl,
		bool maximized) {
	if (tl->closing || tl->xdg_toplevel->base == NULL) {
		return;
	}
	if (tl->morph_active) {
		/* an in-flight maximize/restore zoom owns the scene node: let a
		 * new maximize/restore request replace it cleanly instead of
		 * fighting the running zoom (rapid toggling, or clients that
		 * re-assert set_maximized before the first configure is acked) */
		animate_toplevel_abort_geometry(tl);
	}
	if (tl->fullscreen) {
		/* while fullscreen, maximize/restore is not available; only
		 * leaving fullscreen returns the window to its previous state */
		return;
	}
	if (toplevel_is_dialog(tl) || toplevel_is_fixed_size(tl)) {
		/* dialogs and fixed-size windows (e.g. QQ's login) are never
		 * maximized (their top border is a close button, not a title bar) */
		return;
	}
	if (maximized && tl->xdg_toplevel->current.maximized) {
		/* already maximized: re-fit the window to the maximized geometry.
		 * A maximized window can drift off it (e.g. a client-drag of a
		 * maximized window that never got restored), and the client's own
		 * maximize button then sends a set_maximized that would otherwise
		 * be a silent no-op - the window stays stuck.  Re-asserting the
		 * box makes a maximize request always respond. */
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
	tl->user_moved = true; /* maximize/restore state: stop auto-centering */
	if (maximized) {
		/* remember the floating geometry so restoring returns the window
		 * exactly to where it was before this maximize (Windows behavior).
		 * Only capture while the client still reports the window as
		 * floating (current.maximized false): clients like QQ re-assert
		 * set_maximized before the first request is acked, and re-saving
		 * then would overwrite the floating geometry with the
		 * already-maximized box.  Re-capture whenever the floating box
		 * changed since the last capture, so a move or resize between two
		 * maximize/restore cycles never makes the next restore return to
		 * an old spot or an old size. */
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
			/* Windows-style zoom into the maximized box (animate.c): keep
			 * showing the floating window until the client commits the
			 * maximized content, then scale it up.  Falls back to an
			 * instant jump when the zoom cannot run. */
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
		/* restore to the geometry saved when the window was maximized
		 * (position + size), so the client's own restore button brings
		 * the window back exactly where it was.  Falling back to 0x0
		 * only when no floating geometry was ever saved. */
		if (tl->has_restore_box && tl->restore_box.width > 0) {
			int x = tl->restore_box.x;
			int y = tl->restore_box.y;
			/* restore to the saved spot exactly (even half off-screen);
			 * only a bar that appeared meanwhile may move it */
restore_box_position(server, &tl->restore_box, &x, &y);

			wlr_xdg_toplevel_set_size(tl->xdg_toplevel,
				tl->restore_box.width, tl->restore_box.height);
			/* Windows-style zoom back to the floating box (animate.c):
			 * keep the maximized window on screen until the client commits
			 * the restored content, then scale it down into place */
			struct wlr_box from_box;
			toplevel_box(tl, &from_box);
			struct wlr_box to_box = { x, y, tl->restore_box.width,
				tl->restore_box.height };
			if (!animate_toplevel_geometry(server, tl, &from_box, &to_box)) {
				wlr_scene_node_set_position(&tl->scene_tree->node, x, y);
			}
		} else {
			/* 0x0 lets the client pick its own size again */
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel, 0, 0);
		}
	}
	wlr_xdg_toplevel_set_maximized(tl->xdg_toplevel, maximized);
	if (tl->fthandle != NULL) {
		wlr_foreign_toplevel_handle_v1_set_maximized(tl->fthandle, maximized);
	}
	}

/* un-maximize back to the previously saved geometry (drag of a maximized
 * window's title bar) */
/* un-maximize back to the previously saved geometry (drag of a maximized
 * window's title bar, keyboard/button toggle).  With `animate` the restore
 * plays the same Windows-style zoom as maximizing (animate.c) and the
 * caller must not move the scene node itself afterwards - the zoom leaves
 * the node at the restored box.  Drag restores pass false: the pointer
 * grabs the restored window and positions it per motion, so a zoom would
 * fight the grab. */
void restore_maximized_toplevel(struct toplevel *tl, bool animate) {
	if (tl->closing || tl->xdg_toplevel->base == NULL ||
			!tl->xdg_toplevel->current.maximized || tl->fullscreen) {
		return;
	}
	if (tl->morph_active) {
		/* an in-flight maximize/restore zoom owns the scene node */
		animate_toplevel_abort_geometry(tl);
	}
	tl->user_moved = true; /* drag-restore: stop auto-centering */
	if (tl->has_restore_box && tl->restore_box.width > 0) {
		int x = tl->restore_box.x;
		int y = tl->restore_box.y;
		/* restore to the saved spot exactly (even half off-screen); only a
		 * bar that appeared meanwhile may move it */
		restore_box_position(tl->server, &tl->restore_box, &x, &y);
		wlr_xdg_toplevel_set_size(tl->xdg_toplevel, tl->restore_box.width,
			tl->restore_box.height);
		if (animate) {
			/* Windows-style zoom back to the floating box, symmetric with
			 * maximizing (animate.c) */
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
		/* a close is pending: never minimize/restore a dying window */
		return;
	}
	if (toplevel_is_dialog(tl) || toplevel_is_fixed_size(tl)) {
		/* dialogs and fixed-size windows have no minimize button */
		return;
	}
	if (tl->minimized == minimized) {
		return;
	}
	if (tl->morph_active) {
		/* an in-flight maximize/restore zoom owns the scene node (rapid
		 * toggles, or clients re-asserting set_maximized before the first
		 * configure is acked): let the new request replace it */
		animate_toplevel_abort_geometry(tl);
	}
	tl->minimized = minimized;
	if (tl->fthandle != NULL) {
		wlr_foreign_toplevel_handle_v1_set_minimized(tl->fthandle, minimized);
	}
	if (minimized && server->focused == tl) {
		/* hand keyboard/cursor focus to the previous visible window.  The
		 * window keeps its scene node visible while it falls (animate.c),
		 * but it is already flagged minimized, so nothing focuses it again
		 * and restoring it (focus cycle, foreign-toplevel activate) puts
		 * it back exactly where it was */
		struct toplevel *prev = neighbor_toplevel(server, tl, false, false);
		server->focused = NULL;
		if (tl->xdg_toplevel->base != NULL) {
			wlr_xdg_toplevel_set_activated(tl->xdg_toplevel, false);
		}
		wlr_seat_keyboard_clear_focus(server->seat);
		if (prev != NULL) {
			focus_toplevel(server, prev);
		} else {
			ipc_send_window_event(server, "window_focus", NULL);
			ime_set_focus(server, NULL);
		}
	}
	if (minimized) {
		/* animate the fall; without an animation the node is simply hidden
		 * (the window keeps its position, so restoring it puts it back
		 * exactly where it was) */
		if (!animate_toplevel_minimize(server, tl)) {
			wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
		}
	} else if (!animate_toplevel_restore(server, tl)) {
		wlr_scene_node_set_enabled(&tl->scene_tree->node, true);
	}
	/* hiding the window may expose a different surface under the cursor */
	update_cursor_style(server);
}

/* raise a toplevel's scene node and, above it, every dialog that declared
 * it as its xdg parent (recursively): focusing / clicking the main window
 * must never bury its open dialogs underneath it.  Dialogs are raised
 * after their parent, so they always end up stacked above it. */
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
	/* the border color depends on focus: re-render both the newly focused
	 * window and the previously focused one so their rounded FBOs pick up
	 * the new focused/unfocused border color (see border.c) */
	border_focus_changed(tl, prev);
	wlr_xdg_toplevel_set_activated(tl->xdg_toplevel, true);
	if (tl->fthandle != NULL) {
		wlr_foreign_toplevel_handle_v1_set_activated(tl->fthandle, true);
	}
	/* move the seat keyboard to this toplevel.  If a launcher overlay held
	 * it (rofi), layer_keyboard_clear() releases that hold and moves the
	 * keyboard here in one step; otherwise the toplevel simply takes it. */
	if (layer_held) {
		layer_keyboard_clear(server, tl->xdg_toplevel->base->surface);
	} else {
		seat_keyboard_focus(server, tl->xdg_toplevel->base->surface);
	}
	toplevel_raise(server, tl);
	/* tell the input method which surface now owns text input */
	ime_set_focus(server, tl->xdg_toplevel->base->surface);
	/* notify status bars that the focus changed */
	ipc_send_window_event(server, "window_focus", tl);
}

/* xdg-activation-v1 request_activate: the surface's client asked for focus
 * with a valid activation token.  Focus (and restore) the matching toplevel. */
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

/* ------------------------------------------------------------------ */
/* xdg-shell toplevels                                                */
/* ------------------------------------------------------------------ */

static void toplevel_unfocus(struct server *server, struct toplevel *tl) {
	if (server->focused == tl) {
		/* the focused window is going away: hand focus back to the window
		 * that launched it (after_id; a same-process sibling window beats
		 * the terminal that spawned the process), or the previous visible
		 * window */
		struct toplevel *prev = focus_fallback(server, tl);
		server->focused = NULL;
		wlr_seat_keyboard_clear_focus(server->seat);
		if (prev != NULL) {
			focus_window(server, prev);
		} else {
			ipc_send_window_event(server, "window_focus", NULL);
			ime_set_focus(server, NULL);
		}
	}
	if (server->zone_toplevel == tl || server->move_toplevel == tl) {
		end_move(server);
	}
	if (server->resize_toplevel == tl) {
		end_resize(server);
	}
}

/* ------------------------------------------------------------------ */
/* subsurfaces: their commits mark the rounded FBO cache dirty so the
 * offscreen copy is re-rendered when any content changes */
/* ------------------------------------------------------------------ */

static void toplevel_subsurface_commit(struct wl_listener *listener,
		void *data) {
	struct toplevel_subsurface *ts = wl_container_of(listener, ts, commit);
	(void)data;
	/* wlroots re-applies opacity 1.0 to the committed subsurface; re-hide it
	 * (only if a valid rounded FBO is already published) and re-render the
	 * FBO cache.  The damage the commit attached is collected for a partial
	 * re-render (rounded.c). */
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
	/* the subsurface's old content must vanish from the cached FBO: a full
	 * re-render (with its damage bookkeeping gone) covers that */
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
	/* a nested subsurface (subsurface of a subsurface): track it exactly
	 * like a direct subsurface so its commits also invalidate the FBO */
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
	/* a newly added subsurface must be hidden from the scene and folded
	 * into the next FBO render */
	rounded_cache_hide_content(tl);
	rounded_cache_dirty(tl);

	/* subsurfaces that already exist below this one are only reachable by
	 * walking the scene graph; the new_subsurface listener above catches
	 * only future ones, so recurse into the existing children too */
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

/* remove all subsurface bookkeeping before the toplevel is freed */
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

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, map);
	struct server *server = tl->server;

	if (tl->closing) {
		/* the client re-showed a surface it was asked to close (e.g. it
		 * answered the close request with an unmap/remap dance): keep the
		 * window hidden and re-send the close - a close request is sticky
		 * until the surface is destroyed */
		if (tl->scene_tree != NULL) {
			wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
		}
		wlr_xdg_toplevel_send_close(tl->xdg_toplevel);
		return;
	}

	if (!tl->positioned) {
		tl->positioned = true;
		/* size stays exactly what the client committed; the position is
		 * centered on the output both horizontally and vertically
		 * (place.c) */
		place_toplevel(server, tl);
	}
	/* notify status bars first, then the state/focus events */
	tl->ipc_added = true;
	ipc_send_window_event(server, "window_added", tl);
	/* honor fullscreen/maximize requests that arrived before the first
	 * commit (the surface is initialized and mapped by now) */
	if (tl->xdg_toplevel->requested.fullscreen) {
		/* initial state, not a user fullscreen action: apply instantly */
		tl->skip_geom = true;
		set_fullscreen(server, tl, true);
		tl->skip_geom = false;
	} else if (tl->xdg_toplevel->requested.maximized) {
		/* initial state, not a user maximize action: apply instantly */
		tl->skip_geom = true;
		set_maximized(server, tl, true);
		tl->skip_geom = false;
	}
	focus_toplevel(server, tl);
	update_toplevel_output(server, tl);
	rounded_cache_hide_content(tl);
	rounded_cache_dirty(tl);
	/* a freshly mapped window fades in (animate.c) */
	if (!tl->minimized) {
		animate_toplevel_fade_in(server, tl);
	}
	/* a window mapped under a stationary cursor must immediately show the
	 * right cursor (title zone / resize edge), without waiting for motion */
	update_cursor_style(server);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, unmap);
	/* the window hid itself mid-animation: stop it and leave the window
	 * in a clean state (a pending minimize completes, a pending restore
	 * lands) */
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
	/* the window is going away: if its client owned the cursor (I-beam
	 * over a text field, etc.), revert to the default arrow.  This must run
	 * here, while xdg_toplevel is still alive - wlroots frees it right
	 * after this signal, so xdg_surface_destroy cannot touch it. */
	if (tl->xdg_toplevel->base != NULL) {
		toplevel_client_cursor_gone(server,
			tl->xdg_toplevel->base->surface->resource->client);
	}
	wl_list_remove(&tl->link);

	if (tl->fthandle != NULL) {
		wlr_foreign_toplevel_handle_v1_destroy(tl->fthandle);
	}

	/* remove all listeners attached to the toplevel and its surface; the
	 * xdg surface listeners (tl->destroy, tl->new_popup) stay linked until
	 * the xdg surface itself is destroyed.  Popups need no cleanup here:
	 * their scene trees are children of tl->scene_tree and each popup frees
	 * itself when its own tree is destroyed (which happens when the popup's
	 * xdg surface goes away or with tl->scene_tree). */
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

/* the closing window's client may own the current pointer cursor (a
 * cursor-shape or a cursor surface, e.g. the I-beam over a text field):
 * drop the stale state and revert to the default arrow when the
 * compositor isn't overriding the cursor (move / resize / title strip) */
static void toplevel_client_cursor_gone(struct server *server,
		struct wl_client *client) {
	bool owned = false;
	if (server->client_cursor_shape != 0 &&
			server->client_cursor_shape_client != NULL &&
			server->client_cursor_shape_client->client == client) {
		server->client_cursor_shape = 0;
		/* drop the destroy listener together with the pointer, or the
		 * orphaned listener is re-added to another seat client later and
		 * corrupts the destroy-listener list (wlroots asserts on it in
		 * seat_client_destroy when that client disconnects) */
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
	/* notify status bars before the window disappears */
	if (tl->ipc_added) {
		ipc_send_window_event(tl->server, "window_removed", tl);
	}
	/* the xdg scene tree is destroyed by wlroots' own xdg-surface scene
	 * handler registered by wlr_scene_xdg_surface_create */
	free(tl->app_id);
	free(tl);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, commit);
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;

	/* per xdg-shell the compositor must reply to the initial commit with
	 * the first configure */
	if (base != NULL && base->initial_commit) {
		wlr_xdg_toplevel_set_size(tl->xdg_toplevel, 0, 0);
	}

	update_toplevel_output(tl->server, tl);

	/* the first configure must only be sent once the xdg surface is
	 * initialized; respond to the client's decoration mode (or our
	 * default) on its first commit */
	if (tl->decoration != NULL && !tl->decoration_configured &&
			base != NULL && base->initialized) {
		tl->decoration_configured = true;
		if (tl->decoration_mode !=
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE) {
			wlr_xdg_toplevel_decoration_v1_set_mode(tl->decoration,
				tl->decoration_mode);
		}
	}

	/* a top/left grab is positioned here, once the client has actually
	 * committed the new geometry: update_resize() only sends the size
	 * configure and never moves the node for these grabs, so the fixed
	 * (opposite) edge stays glued to where it was when the grab started
	 * and the old, still-larger buffer is never shown at a moved
	 * position - which is what made the bottom edge bounce while
	 * resizing from the top.  Re-asserting the position before the next
	 * frame is drawn keeps the box in sync with the committed buffer. */
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

	/* outline mode (CONFIG_RESIZE_DRAW_CONTENTS=0): the button release only
	 * drew the outline and sent the final size; this commit carries the
	 * final geometry, so end the resize grab now.  The reposition above
	 * already anchored a top/left grab to the committed size. */
	if (tl->server->resizing && tl->server->resize_toplevel == tl &&
			tl->server->resize_final_pending) {
		resize_grab_clear(tl->server);
	}

	/* keep a fresh window centered until the user interacts with it:
	 * Electron windows (QQ) often map with a small placeholder surface
	 * and only commit their real size on a later frame, so re-center
	 * whenever the surface size changes (place.c).  User interactions
	 * (move / resize / maximize / fullscreen) set user_moved and stop
	 * this, and the window then stays where the user puts it. */
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
	/* geometry (and thus the resize/title zones under the cursor) may have
	 * changed; the rounded FBO cache must follow any content/geometry
	 * change.  Damage attached to the commit is collected for a partial
	 * re-render (rounded.c). */
	rounded_cache_content_commit(tl);
	update_cursor_style(tl->server);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, request_maximize);
	/* early requests (before the first commit) are applied at map time */
	if (tl->xdg_toplevel->base == NULL ||
			!tl->xdg_toplevel->base->initialized) {
		return;
	}
	/* honor the client's requested value (wlroots stores it in
	 * requested.maximized): a client's own maximize button sends
	 * set_maximized(true) to maximize and set_maximized(false) to restore.
	 * Ignoring the value and always maximizing made the second press a
	 * silent no-op - the window could never be restored from its own
	 * button. */
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
	/* early requests (before the first commit) are applied at map time */
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
	/* a maximized window is NOT restored here: clients like QQ send
	 * xdg_toplevel.move on a plain title-bar click, which would restore
	 * the window without any drag.  move_toplevel_to() restores it on the
	 * first real motion instead, so only an actual drag un-maximizes
	 * (Windows behavior) while a mere click does nothing. */
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
			tl->xdg_toplevel->current.maximized) {
		return;
	}
	/* a client-side-decorated client (e.g. Chromium) asks the compositor to
	 * resize it: enter the compositor's resize grab with the edges the
	 * client chose; pointer motion drives update_resize() and the button
	 * release ends it, exactly like the compositor's own edge handles. */
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

/* defer clamping a popup into its output until the popup has committed
 * once.  wlr_xdg_popup_unconstrain_from_box() schedules a configure, and
 * wlroots asserts on surfaces that are not yet initialized (first commit)
 * — calling it from the new_popup handler crashed the compositor when a
 * Qt app (fcitx5-config-qt theme page, tooltips, combo boxes) opened a
 * popup.  A one-shot commit listener runs right after the role commit set
 * initialized, so the clamp happens before the popup is shown. */
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
	/* the popup follows the cursor, so constrain it into the output under
	 * the cursor (never touch the owning toplevel - it may be gone) */
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

/* ------------------------------------------------------------------ */
/* popups                                                             */
/* ------------------------------------------------------------------ */

static void xdg_popup_attach(struct toplevel *tl, struct wlr_xdg_popup *popup,
		struct wlr_scene_tree *parent_tree);
static void xdg_popup_new_popup(struct wl_listener *listener, void *data);

/* wayland's wl_list_remove nulls the link (prev/next become NULL), so a
 * second wl_list_remove on the same listener dereferences NULL.  Remove a
 * listener only while it is still attached to a signal. */
static void listener_remove_if_attached(struct wl_listener *listener) {
	if (listener->link.prev != NULL) {
		wl_list_remove(&listener->link);
	}
}

/* the wlr_xdg_popup role object is gone (menu closed / grab dismissed):
 * stop reacting to its new popups and destroy signal.  The struct itself is
 * freed by xdg_popup_tree_destroy once the scene tree goes away. */
static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	struct toplevel_popup *pp = wl_container_of(listener, pp, destroy);
	(void)data;
	listener_remove_if_attached(&pp->new_popup);
	listener_remove_if_attached(&pp->destroy);
}

/* the popup's scene tree is destroyed (wlroots destroys it when the popup's
 * xdg surface is destroyed, or together with the toplevel's tree): release
 * every listener and the struct.  This is the popup's only owner - it never
 * outlives the tree, so no dangling listeners can be left behind.  The
 * per-signal removals are guarded: xdg_popup_destroy may already have
 * detached new_popup/destroy when the popup role died first. */
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

	/* unconstrain the popup into its output box (and thereby send its
	 * first configure) on its first commit.  Registered here so nested
	 * popups (Qt submenus) get it too, not just toplevel-level popups. */
	struct popup_unconstrain *pu = calloc(1, sizeof(*pu));
	if (pu != NULL) {
		pu->popup = popup;
		pu->server = tl->server;
		pu->commit.notify = popup_unconstrain_handle_commit;
		wl_signal_add(&popup->base->surface->events.commit, &pu->commit);
		pu->destroy.notify = popup_unconstrain_handle_destroy;
		wl_signal_add(&popup->base->events.destroy, &pu->destroy);
	}

	/* wlr_scene_xdg_surface_create handles the popup surface and its
	 * subsurfaces, and positions the tree at popup->current.geometry on
	 * every commit.  It also registers its own listener on the popup's
	 * xdg-surface destroy that destroys this tree, so we never destroy it
	 * ourselves (that would free wlroots' listener mid-signal-emit). */
	pp->tree = wlr_scene_xdg_surface_create(parent_tree, popup->base);
	if (pp->tree == NULL) {
		free(pp);
		return;
	}
	/* tagged so hit-testing can still resolve the owning window under a
	 * popup (scene.c) */
	xdg_surface_tag(pp->tree, TAG_POPUP, popup);

	/* pp lives exactly as long as the tree: when the tree is destroyed
	 * (popup xdg surface gone, or the toplevel's tree gone) the listener
	 * below releases everything.  Register it before the popup listeners
	 * so it is the last one to run on destroy. */
	pp->tree_destroy.notify = xdg_popup_tree_destroy;
	wl_signal_add(&pp->tree->node.events.destroy, &pp->tree_destroy);

	/* nested popups (Qt submenus) attach under this popup's tree */
	pp->new_popup.notify = xdg_popup_new_popup;
	wl_signal_add(&popup->base->events.new_popup, &pp->new_popup);
	pp->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&popup->events.destroy, &pp->destroy);
}

static void xdg_popup_new_popup(struct wl_listener *listener, void *data) {
	struct toplevel_popup *pp = wl_container_of(listener, pp, new_popup);
	/* a nested popup (Qt submenu): its parent scene node is this popup's
	 * own scene tree */
	xdg_popup_attach(pp->tl, data, pp->tree);
}

static void xdg_toplevel_new_popup(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, new_popup);
	struct wlr_xdg_popup *popup = data;

	/* the popup's parent is the toplevel surface, so its scene tree lives
	 * inside the toplevel's content tree (whose origin is the window
	 * geometry top-left, exactly what the popup geometry is relative to) */
	xdg_popup_attach(tl, popup, tl->scene_tree);
}

/* ------------------------------------------------------------------ */
/* foreign-toplevel management                                        */
/* ------------------------------------------------------------------ */

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

	/* remember the client's pid so the bar can match this window to its
	 * tray item (by pid) and clear message notifications when the window
	 * is activated from the taskbar */
	tl->pid = 0;
	if (base->client != NULL && base->client->client != NULL) {
		wl_client_get_credentials(base->client->client, &tl->pid, NULL, NULL);
	}

	/* the xdg surface tree handles the surface, subsurfaces and their
	 * positioning */
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
	/* subsurfaces that already exist before this listener was added */
	struct wlr_subsurface *subsurface;
	wl_list_for_each(subsurface, &base->surface->current.subsurfaces_below,
			current.link) {
		toplevel_subsurface_add(tl, subsurface);
	}
	wl_list_for_each(subsurface, &base->surface->current.subsurfaces_above,
			current.link) {
		toplevel_subsurface_add(tl, subsurface);
	}

	/* the toplevel destroy handler (frees the foreign toplevel handle and
	 * unlinks the listeners above) must run before wlroots asserts that the
	 * toplevel signals are empty, i.e. on xdg_toplevel->events.destroy */
	tl->toplevel_destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &tl->toplevel_destroy);

	/* Remember which window (if any) launched this one: when this window
	 * closes, focus returns to that launcher window.  The walk starts at
	 * this window's own pid so a sibling window of the same process (e.g.
	 * Qt's parentless About dialogs - same pid as the main window) wins
	 * over the process-tree ancestor (the terminal that launched the
	 * process): closing the dialog must hand focus back to its main
	 * window, not to the terminal. */
	struct toplevel *ancestor = window_ancestor(server, tl->pid);
	tl->after_id = ancestor != NULL ? ancestor->id : 0;

	wl_list_insert(server->toplevels.prev, &tl->link);
}

/* ------------------------------------------------------------------ */
/* xdg-decoration                                                     */
/* ------------------------------------------------------------------ */

/* clients that draw their own decorations but whose own window resize is
 * broken (frameless Electron windows, e.g. QQ): the compositor owns their
 * frame (resize edges, top strip, resize cursor) exactly like an
 * undecorated window, because the client's own edge resize never works.
 * QQ explicitly requests client-side decorations but cannot resize
 * itself, so its request is overridden to NONE below. */
static bool toplevel_force_undecorated(struct toplevel *tl) {
	if (tl->app_id == NULL) {
		return false;
	}
	/* config-driven list (see config.h): clients that draw their own CSD
	 * frame but whose edge resize relies on the compositor.  Case-
	 * insensitive prefix match against the app_id, so a single entry
	 * covers variants. */
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

	/* default: let clients draw their own decorations unless they explicitly
	 * ask for server-side (we never draw any, the top-10px zone takes over
	 * instead). The actual configure is sent on the first commit.  Clients
	 * whose own resize is known to be broken (frameless Electron, e.g.
	 * QQ) get NONE instead so the compositor owns their frame. */
	tl->decoration_mode = toplevel_force_undecorated(tl)
		? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE
		: WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
}
