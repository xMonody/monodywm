/*
 * pointer.c - compositor cursor interaction
 *
 * Undecorated windows get a frame owned by the compositor:
 *   - top strip (the 3-colored title border) : long press / drag moves
 *     the window; double-click a segment minimizes (left), toggles
 *     maximize/restore (middle) or closes (right); hold right + wheel
 *     up/down toggles maximize-restore / minimize-restore
 *   - edges / corners    : resize
 * A press in the frame is swallowed by the compositor and never reaches
 * the client; client-side decorated windows keep their native controls.
 */

#include "server.h"

#include <limits.h>
#include <math.h>
#include <time.h>

#include <linux/input-event-codes.h>

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

/* is the cursor over the visible title strip of an undecorated window?  The
 * strip spans the window width: the top CONFIG_TITLEBAR_HEIGHT px of the
 * content, split into three gesture segments (minimize / maximize / close). */
static bool is_in_titlebar_zone(struct server *server, struct toplevel *tl) {
	/* a popup (menu / dropdown) floating over the strip wins the pointer:
	 * no move / minimize / maximize / close grab while the cursor is on it */
	if (pointer_over_popup(server) || pointer_over_layer_surface(server)) {
		return false;
	}
	if (tl->decoration_mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE &&
			!toplevel_is_dialog(tl) && !toplevel_is_fixed_size(tl)) {
		return false; /* client draws its own title bar, use it natively */
	}
	/* dialogs and fixed-size windows: the top border band is the
	 * compositor's close button even when the client draws its own (CSD)
	 * decorations */
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	if (base == NULL || !base->surface->mapped) {
		return false;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);
	if (box.width <= 0) {
		return false;
	}
	double lx = server->cursor->x;
	double ly = server->cursor->y;
	return lx >= box.x && lx < box.x + box.width &&
		ly >= box.y &&
		ly < box.y + CONFIG_TITLEBAR_HEIGHT;
}

/* which third of the title strip the press sits over: left third =
 * minimize, middle third = toggle maximize/restore, right third = close */
static enum zone_action title_strip_action(struct server *server,
		struct toplevel *tl) {
	/* a window that cannot maximize/minimize (dialog, or fixed-size like
	 * QQ's login) has a top border that is one single close button: a
	 * double-click anywhere on it closes the window */
	if (toplevel_is_dialog(tl) || toplevel_is_fixed_size(tl)) {
		return ZONE_CLOSE;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);
	if (box.width <= 0) {
		return ZONE_CLOSE;
	}
	double x = server->press_x;
	double third = box.width / 3.0;
	if (x < box.x + third) {
		return ZONE_MINIMIZE;
	}
	if (x < box.x + 2.0 * third) {
		return ZONE_MAXIMIZE;
	}
	return ZONE_CLOSE;
}

static void disarm_zone_timer(struct server *server) {
	if (server->zone_timer != NULL) {
		wl_event_source_timer_update(server->zone_timer, 0);
	}
}

/* ------------------------------------------------------------------ */
/* window move                                                        */
/* ------------------------------------------------------------------ */

static bool is_double_click(struct server *server, uint32_t button) {
	if (!server->last_was_click || button != server->last_click_button) {
		return false;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t dt = (now.tv_sec - server->last_release_time.tv_sec) * 1000000000L
		+ (now.tv_nsec - server->last_release_time.tv_nsec);
	return dt >= 0 && dt <= CONFIG_DOUBLE_CLICK_NS;
}

void begin_move(struct server *server, struct toplevel *tl,
		double ref_x, double ref_y) {
	if (server->moving) {
		return;
	}
	server->moving = true;
	server->input_mode = INPUT_MODE_MOVE;
	server->move_toplevel = tl;
	if (tl != NULL) {
		tl->user_moved = true; /* user move: stop auto-centering */
	}
	server->move_ref_x = ref_x;
	server->move_ref_y = ref_y;
	server->grab_x = ref_x - tl->scene_tree->node.x;
	server->grab_y = ref_y - tl->scene_tree->node.y;
	/* remember the maximized box: when the drag restores the window, the
	 * press point's window-internal offset is mapped proportionally into
	 * the restored box (see move_toplevel_to) */
	server->move_max_w = 0;
	server->move_max_h = 0;
	if (tl->xdg_toplevel->current.maximized) {
		struct wlr_box box;
		toplevel_box(tl, &box);
		server->move_max_w = box.width;
		server->move_max_h = box.height;
	}
}

void end_move(struct server *server) {
	if (server->moving) {
	}
	server->moving = false;
	server->input_mode = INPUT_MODE_PASSTHROUGH;
	server->move_deferred_restore = false;
	server->move_toplevel = NULL;
	server->zone_toplevel = NULL;
	server->zone_press = false;
	server->dragged = false;
	server->zone_action = ZONE_NONE;
	disarm_zone_timer(server);
}

/* clamp a dragged window's position so its top can never slide above a
 * top layer-shell bar; left/right bars clamp their sides too.  The bottom
 * is deliberately unclamped: the cursor itself is kept above the bar
 * (clamp_move_cursor), and the window follows it, so it may slide past a
 * bottom bar and off the bottom of the screen - Windows-style. */
static void clamp_drag_position(struct server *server, struct toplevel *tl,
		double *x, double *y) {
	struct wlr_box box;
	toplevel_box(tl, &box);
	if (box.width <= 0 || box.height <= 0) {
		return;
	}
	struct wlr_output *output = toplevel_output(server, tl);
	if (output == NULL) {
		return;
	}
	struct wlr_box out;
	wlr_output_layout_get_box(server->output_layout, output, &out);
	struct wlr_box area;
	get_work_area(server, output, &area);

	if (area.x > out.x) {           /* bar at the left */
		if (*x < area.x) {
			*x = area.x;
		}
	}
	if (area.y > out.y) {           /* bar at the top */
		if (*y < area.y) {
			*y = area.y;
		}
	}
	if (area.x + area.width < out.x + out.width) { /* bar at the right */
		if (*x + box.width > area.x + area.width) {
			*x = area.x + area.width - box.width;
		}
	}
}

/* while a window is being dragged, the cursor may never enter a layer-shell
 * bar's exclusive zone: clamp it to the work area of the output under it,
 * so it stays visible above the bar.  The window follows the cursor with
 * no bottom limit, so it can slide past the bar and off the screen. */
static void clamp_move_cursor(struct server *server) {
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (output == NULL) {
		output = wlr_output_layout_get_center_output(server->output_layout);
	}
	if (output == NULL) {
		return;
	}
	struct wlr_box area;
	get_work_area(server, output, &area);
	double cx = server->cursor->x;
	double cy = server->cursor->y;
	if (cx < area.x) {
		cx = area.x;
	} else if (cx > area.x + area.width) {
		cx = area.x + area.width;
	}
	if (cy < area.y) {
		cy = area.y;
	} else if (cy > area.y + area.height) {
		cy = area.y + area.height;
	}
	if (cx != server->cursor->x || cy != server->cursor->y) {
		wlr_cursor_warp(server->cursor, NULL, cx, cy);
	}
}

static void move_toplevel_to(struct server *server, double lx, double ly) {
	struct toplevel *tl = server->move_toplevel;
	if (tl == NULL) {
		return;
	}
	/* a maximized window whose move came from the client (xdg_toplevel.
	 * move - e.g. QQ's own title bar sends it on a plain click, not just
	 * a drag): defer the restore until the drag really moves, so a mere
	 * click - or the first press of a double-click, with its few px of
	 * hand jitter - never un-maximizes the window and yanks it across
	 * the screen.  Only here (not in request_move) can a click be told
	 * apart from a drag, and only once the cursor passed
	 * CONFIG_DRAG_THRESHOLD (the zone strip gates its move on the same
	 * threshold; without it the client title bar restored on the first
	 * pixel of jitter and scrambled the double-click). */
	if (tl->xdg_toplevel->current.maximized) {
		double ddx = server->cursor->x - server->move_ref_x;
		double ddy = server->cursor->y - server->move_ref_y;
		if (ddx * ddx + ddy * ddy <
				CONFIG_DRAG_THRESHOLD * CONFIG_DRAG_THRESHOLD) {
			/* a click (or a double-click's first press) that never crossed
			 * the drag threshold: the window stays maximized, nothing moves */
			return;
		}
		restore_maximized_toplevel(tl, false); /* drag: grab owns the geometry */
		/* re-anchor the grab by mapping the press point's offset inside
		 * the maximized box proportionally into the restored box: the
		 * cursor keeps gripping the same window-internal spot it pressed.
		 * A plain absolute offset would keep it on the same pixel, which
		 * drifts right on narrower restored windows (e.g. a press on the
		 * centered title text of a maximized gvim would float past the
		 * text once the window shrinks); proportional mapping lands the
		 * cursor on the same relative spot - exactly where centered
		 * content (title text) sits.  Do NOT re-anchor against the
		 * restored origin: that makes the grab negative (the press point
		 * usually lies above/left of it) and shoves the window away. */
		if (server->move_deferred_restore && tl->has_restore_box &&
				tl->restore_box.width > 0 && server->move_max_w > 0) {
			server->grab_x = server->grab_x * tl->restore_box.width /
				server->move_max_w;
			server->grab_y = server->grab_y * tl->restore_box.height /
				server->move_max_h;
			/* map only once: current.maximized stays true until the client
			 * commits the un-maximize configure, so every motion would
			 * re-enter this branch and shrink the grab exponentially,
			 * drifting the cursor up-left on every event */
			server->move_max_w = 0;
			server->move_max_h = 0;
		}
	}
	/* the cursor must stay above the status bar while dragging; the
	 * window follows it with no bottom limit (it may slide past the bar
	 * and off the screen, Windows-style) */
	clamp_move_cursor(server);
	lx = server->cursor->x;
	ly = server->cursor->y;
	double nx = lx - server->grab_x;
	double ny = ly - server->grab_y;
	clamp_drag_position(server, tl, &nx, &ny);
	wlr_scene_node_set_position(&tl->scene_tree->node, nx, ny);
}

/* turn a press on the title strip into a move grab: the grab anchors at the
 * original press position; dragging a maximized window restores it first
 * (Windows behavior) so the drag grips its restored geometry */
static void begin_zone_drag(struct server *server) {
	struct toplevel *tl = server->zone_toplevel;
	if (tl == NULL) {
		return;
	}
	server->dragged = true;
	server->zone_action = ZONE_NONE; /* the drag cancels any armed double click */
	double ref_x = server->press_x;
	double ref_y = server->press_y;
	if (tl->xdg_toplevel->current.maximized) {
		/* drag of a maximized window's title bar: restore it to its
		 * previous geometry and clamp the grab point into the restored
		 * window, so the cursor grips its title bar and the window
		 * follows (Windows behavior) */
		restore_maximized_toplevel(tl, false); /* drag: grab owns the geometry */
		struct wlr_box rb = tl->restore_box;
		/* restore_maximized_toplevel clamps the restored position into
		 * the work area, so grip the cursor on the window's actual box,
		 * not the stale saved one */
		rb.x = tl->scene_tree->node.x;
		rb.y = tl->scene_tree->node.y;
		if (tl->has_restore_box && rb.width > 0) {
			if (ref_x < rb.x) {
				ref_x = rb.x;
			}
			if (ref_x > rb.x + rb.width - 1) {
				ref_x = rb.x + rb.width - 1;
			}
			if (ref_y < rb.y) {
				ref_y = rb.y;
			}
			if (ref_y > rb.y + rb.height - 1) {
				ref_y = rb.y + rb.height - 1;
			}
		}
	}
	begin_move(server, tl, ref_x, ref_y);
	move_toplevel_to(server, server->cursor->x, server->cursor->y);
	server->last_was_click = false;
	disarm_zone_timer(server);
}

/* the strip was held without moving for CONFIG_LONG_PRESS_NS: grab the
 * window so it follows the cursor (a long press anywhere on the border
 * moves the window) */
static int zone_timer_cb(void *data) {
	struct server *server = data;
	if (server->zone_press && server->zone_toplevel != NULL &&
			!server->dragged && !server->moving && !server->resizing &&
			!server->chord_active) {
		bool held = (server->zone_button == BTN_LEFT &&
			server->left_button_held) ||
			(server->zone_button == BTN_RIGHT &&
			server->right_button_held);
		if (held) {
			begin_zone_drag(server);
		}
	}
	return 0; /* leave the source armed for the next press */
}

static void arm_zone_timer(struct server *server) {
	if (server->zone_timer == NULL) {
		struct wl_event_loop *loop =
			wl_display_get_event_loop(server->display);
		server->zone_timer =
			wl_event_loop_add_timer(loop, zone_timer_cb, server);
		if (server->zone_timer == NULL) {
			return; /* no timer: drag still moves, long press can't grab */
		}
	}
	wl_event_source_timer_update(server->zone_timer,
		CONFIG_LONG_PRESS_NS / 1000000);
}

/* ------------------------------------------------------------------ */
/* edge resize                                                        */
/* ------------------------------------------------------------------ */

/* which edges of the toplevel the cursor is over. Only compositor-owned
 * windows (no client-side decoration) resize here; CSD windows handle
 * their own edges.  The top strip itself is the title bar, but its two
 * corner zones (top-left / top-right) are diagonal resize handles.
 * Each grab zone straddles its edge: half of CONFIG_EDGE_THICKNESS lies
 * outside the window box and half inside, so the handles are reachable
 * both from the desktop and from just inside the window.  A maximized
 * window is never resized here. */
static uint32_t toplevel_resize_edges(struct server *server,
		struct toplevel *tl) {
	/* a popup covering the window's border wins the pointer: no resize
	 * handle while the cursor is over it (the popup is the focused
	 * surface and its clicks must reach the menu, not a resize grab) */
	if (pointer_over_popup(server) || pointer_over_layer_surface(server)) {
		return 0;
	}
	if (tl->minimized || tl->xdg_toplevel->base == NULL ||
			tl->decoration_mode ==
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE ||
			tl->xdg_toplevel->current.maximized ||
			toplevel_is_dialog(tl) || toplevel_is_fixed_size(tl)) {
		/* dialogs and fixed-size windows (e.g. QQ's login) are never
		 * resized, so their edges never get a resize cursor either */
		return 0;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);
	if (box.width <= 0 || box.height <= 0) {
		return 0;
	}
	double lx = server->cursor->x;
	double ly = server->cursor->y;
	/* per-axis fixed dimensions (min == max) disable only that axis's
	 * resize handles */
	bool fixed_w = tl->xdg_toplevel->current.min_width > 0 &&
		tl->xdg_toplevel->current.min_width ==
			tl->xdg_toplevel->current.max_width;
	bool fixed_h = tl->xdg_toplevel->current.min_height > 0 &&
		tl->xdg_toplevel->current.min_height ==
			tl->xdg_toplevel->current.max_height;
	if (fixed_w && fixed_h) {
		return 0;
	}
	uint32_t edges = 0;
	double zone = CONFIG_EDGE_THICKNESS / 2.0; /* half out, half in */
	double x0 = box.x - zone;              /* left handle outer edge */
	double x1 = box.x + box.width + zone;  /* right handle outer edge */
	double y0 = box.y - zone;              /* top handle outer edge */
	double y1 = box.y + box.height + zone; /* bottom handle outer edge */
	bool in_x = lx >= x0 && lx <= x1;
	bool in_y = ly >= y0 && ly <= y1;
	bool on_left = lx >= x0 && lx < box.x + zone;
	bool on_right = lx > box.x + box.width - zone && lx <= x1;
	bool on_top = ly >= y0 && ly < box.y + zone;
	bool on_bottom = ly > box.y + box.height - zone && ly <= y1;
	/* left/right handles run the full height (both corner zones included) */
	if (on_left && in_y && !fixed_w) {
		edges |= WLR_EDGE_LEFT;
	}
	if (on_right && in_y && !fixed_w) {
		edges |= WLR_EDGE_RIGHT;
	}
	/* the bottom handle runs the full width */
	if (on_bottom && in_x && !fixed_h) {
		edges |= WLR_EDGE_BOTTOM;
	}
	/* the top edge itself is the title strip; only its two corner zones
	 * are diagonal resize handles */
	if (on_top && (on_left || on_right) && !fixed_h) {
		edges |= WLR_EDGE_TOP;
	}
	return edges;
}

static const char *resize_cursor_name(uint32_t edges) {
	bool left = (edges & WLR_EDGE_LEFT) != 0;
	bool right = (edges & WLR_EDGE_RIGHT) != 0;
	bool top = (edges & WLR_EDGE_TOP) != 0;
	bool bottom = (edges & WLR_EDGE_BOTTOM) != 0;
	if ((left && top) || (right && bottom)) {
		return "nwse-resize"; /* top-left / bottom-right corner */
	}
	if ((right && top) || (left && bottom)) {
		return "nesw-resize"; /* top-right / bottom-left corner */
	}
	if (left || right) {
		return "ew-resize";
	}
	return "ns-resize";
}

/* ------------------------------------------------------------------ */
/* bound buttons (like labwc): presses the compositor swallowed        */
/* ------------------------------------------------------------------ */

static bool bound_button_contains(struct bound_buttons *bb, uint32_t value) {
	for (int i = 0; i < bb->size; i++) {
		if (bb->values[i] == value) {
			return true;
		}
	}
	return false;
}

static void bound_button_add(struct bound_buttons *bb, uint32_t value) {
	if (bound_button_contains(bb, value)) {
		return;
	}
	if (bb->size >= BOUND_BUTTONS_MAX) {
		return;
	}
	bb->values[bb->size++] = value;
}

static void bound_button_remove(struct bound_buttons *bb, uint32_t value) {
	for (int i = 0; i < bb->size; i++) {
		if (bb->values[i] == value) {
			bb->values[i] = bb->values[--bb->size];
			return;
		}
	}
}

static void set_cursor_override(struct server *server, const char *name) {
	if (server->cursor_override == name) {
		return;
	}
	server->cursor_override = name;
	wlr_log(WLR_DEBUG, "cursor: %s (buttons=%zu)", name,
		server->seat->pointer_state.button_count);
	wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, name);
}

/* show the cursor the focused client currently wants: its compositor-
 * rendered cursor shape (cursor-shape-v1), its own cursor surface
 * (wl_pointer.set_cursor), or the default arrow.  A shape wins over a
 * surface: the compositor always renders it at the correct output scale,
 * while a client-drawn surface may be sized by a client that does not
 * use wp_fractional_scale_v1.  If a compositor
 * cursor override (title strip / resize edge) is active it wins over
 * everything else. */
void reapply_client_cursor(struct server *server) {
	if (server->cursor_override != NULL) {
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
			server->cursor_override);
		return;
	}
	struct wlr_surface *focused =
		server->seat->pointer_state.focused_surface;
	/* the stored client shape is only valid while the pointer is still over
	 * that client's surface; once the pointer moved out to empty desktop or
	 * onto another surface, restoring it would show a stale cursor (e.g. a
	 * resize shape the client set at the edge).  Fall through to the default
	 * arrow below in that case. */
	bool over_shape_client = focused != NULL &&
		server->client_cursor_shape_client != NULL &&
		focused->resource->client ==
			server->client_cursor_shape_client->client;
	if (server->client_cursor_shape != 0 &&
			server->client_cursor_shape_client ==
				server->seat->pointer_state.focused_client &&
			over_shape_client) {
		wlr_log(WLR_DEBUG, "cursor: restore-shape");
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
			wlr_cursor_shape_v1_name(server->client_cursor_shape));
		return;
	}
	/* only hand the pointer back to the client when the pointer is still
	 * over that client's surface (e.g. leaving the resize strip back into
	 * the window). If the pointer moved out to empty desktop, the stored
	 * client cursor (e.g. the terminal's text caret) would be stale, so
	 * fall back to the default arrow. The client draws its cursor on a
	 * separate surface, so compare the owning clients rather than the
	 * surfaces themselves. */
	bool over_client_surface = focused != NULL &&
		server->client_cursor_surface != NULL &&
		focused->resource->client ==
			server->client_cursor_surface->resource->client;
	wlr_log(WLR_DEBUG, "cursor: %s", over_client_surface ?
		"restore-client" : "left_ptr");
	if (over_client_surface) {
		wlr_cursor_set_surface(server->cursor, server->client_cursor_surface,
			server->client_cursor_hotspot_x,
			server->client_cursor_hotspot_y);
	} else {
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
			"left_ptr");
	}
}

static void clear_cursor_override(struct server *server) {
	if (server->cursor_override == NULL) {
		return;
	}
	server->cursor_override = NULL;
	reapply_client_cursor(server);
}

/* the toplevel the cursor interacts with: the window under the cursor, or
 * - when the cursor is outside every window - the one whose resize grab
 * zone (CONFIG_EDGE_THICKNESS px straddling the box: half in, half out)
 * contains the cursor, so the resize handles stay reachable even though
 * half of them live outside the box.
 * The title strip's 2 px overhang above the box (the ring's top edge) is
 * reachable the same way, so the whole colored strip is draggable. */
static struct toplevel *toplevel_nearby(struct server *server) {
	struct toplevel *tl = toplevel_at(server);
	if (tl != NULL && !tl->minimized && !tl->closing) {
		return tl;
	}
	struct toplevel *candidate;
	wl_list_for_each(candidate, &server->toplevels, link) {
		if (toplevel_resize_edges(server, candidate) != 0 ||
				(!candidate->minimized && !candidate->closing &&
				 is_in_titlebar_zone(server, candidate))) {
			return candidate;
		}
	}
	return NULL;
}

/* the band around a window where the compositor suppresses client cursor
 * requests.  The actual grab zone is CONFIG_EDGE_THICKNESS/2 (half inside /
 * half outside the box), but clients detect their own edges over the full
 * CONFIG_EDGE_THICKNESS inside the window (winit, GTK, ...).  Without the
 * wider band a client would store its own resize shape (e.g. clash-verge's
 * ew_resize) just outside the grab zone and get it restored stale once the
 * cursor leaves the edge. */
static bool cursor_in_cursor_band(struct server *server,
		struct toplevel *tl) {
	if (tl->closing || tl->minimized || tl->xdg_toplevel->base == NULL ||
			tl->decoration_mode ==
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE ||
			tl->xdg_toplevel->current.maximized ||
			toplevel_is_dialog(tl) || toplevel_is_fixed_size(tl)) {
		return false;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);
	if (box.width <= 0 || box.height <= 0) {
		return false;
	}
	double lx = server->cursor->x;
	double ly = server->cursor->y;
	double in = CONFIG_EDGE_THICKNESS;        /* inside the window */
	double out = CONFIG_EDGE_THICKNESS / 2.0; /* outside the window */
	bool x_band = lx >= box.x - out && lx <= box.x + box.width + out;
	bool y_band = ly >= box.y - out && ly <= box.y + box.height + out;
	bool on_left = lx >= box.x - out && lx < box.x + in;
	bool on_right = lx > box.x + box.width - in &&
		lx <= box.x + box.width + out;
	bool on_top = ly >= box.y - out && ly < box.y + in;
	bool on_bottom = ly > box.y + box.height - in &&
		ly <= box.y + box.height + out;
	return (on_left && y_band) || (on_right && y_band) ||
		(on_top && x_band) || (on_bottom && x_band);
}

/* is the cursor over the compositor's own frame zone (title strip / resize
 * edge) of any window?  There the compositor owns the cursor, so client
 * cursor requests are ignored (input.c) - the client still keeps pointer
 * focus and receives motion, so its hover feedback keeps working. */
bool pointer_over_frame_zone(struct server *server) {
	if (pointer_over_popup(server) || pointer_over_layer_surface(server)) {
		return false;
	}
	struct toplevel *tl = toplevel_at(server);
	if (tl != NULL && !tl->minimized && !tl->closing) {
		return cursor_in_cursor_band(server, tl) ||
			is_in_titlebar_zone(server, tl);
	}
	/* cursor outside every window: the outer half of an edge zone is still
	 * reachable over the desktop */
	struct toplevel *candidate;
	wl_list_for_each(candidate, &server->toplevels, link) {
		if (cursor_in_cursor_band(server, candidate)) {
			return true;
		}
	}
	return false;
}

/* decide which compositor-owned cursor to show at the current position:
 * hovering the colored top strip (CONFIG_TITLEBAR_HEIGHT px) shows
 * CONFIG_TITLEBAR_CURSOR (all-scroll); moving out of the strip restores
 * the client's cursor through clear_cursor_override() */
void update_cursor_style(struct server *server) {
	const char *name = NULL;

	if (server->moving && server->move_toplevel != NULL) {
		name = CONFIG_MOVE_CURSOR;
	} else if (server->resizing && server->resize_toplevel != NULL) {
		name = resize_cursor_name(server->resize_edges);
	} else if (server->seat->pointer_state.button_count > 0) {
		/* implicit grab: a client holds a pointer button (text selection,
		 * right-button drag, ...) and the pointer is focused on its
		 * surface.  Never switch to the compositor's hover cursors (edge
		 * resize / title strip) while that button is held, no matter which
		 * path reaches here (motion, button release, focus change); the
		 * client's cursor stays until the last button is released. */
		return;
	} else {
		struct toplevel *tl = toplevel_nearby(server);
		if (tl != NULL) {
			if (toplevel_is_dialog(tl)) {
				/* dialogs: the top close band gets the all-scroll hint
				 * (CONFIG_EDGE_THICKNESS / title-strip height); the edges
				 * keep the client's own cursor - no resize cursors */
				if (is_in_titlebar_zone(server, tl)) {
					name = CONFIG_TITLEBAR_CURSOR;
				}
			} else {
				uint32_t edges = toplevel_resize_edges(server, tl);
				/* the corner resize zones win over the title strip;
				 * everywhere else the top strip keeps its all-scroll
				 * cursor */
				bool corner = (edges & WLR_EDGE_TOP) != 0;
				if (corner || !is_in_titlebar_zone(server, tl)) {
					if (edges != 0) {
						name = resize_cursor_name(edges);
					}
				} else {
					name = CONFIG_TITLEBAR_CURSOR;
				}
			}
		}
	}

	if (name != NULL) {
		set_cursor_override(server, name);
	} else if (server->cursor_override != NULL) {
		/* leave the pointer over the client's own cursor (or the default
		 * one, which the motion handler resets when the surface changes) */
		clear_cursor_override(server);
	}
}

/* ------------------------------------------------------------------ */
/* resize outline (labwc-style)                                       */
/* ------------------------------------------------------------------ */

static void resize_outline_ensure(struct server *server) {
	if (server->resize_outline != NULL) {
		return;
	}
	server->resize_outline =
		wlr_scene_tree_create(server->layers[LAYER_OVERLAY]);
	/* focused border color 0x9F6680, premultiplied */
	float color[4] = { 0.62f, 0.40f, 0.50f, 1.0f };
	for (int i = 0; i < 4; i++) {
		server->resize_outline_edges[i] =
			wlr_scene_rect_create(server->resize_outline, 0, 0, color);
	}
}

static void resize_outline_show(struct server *server, struct wlr_box *box) {
	resize_outline_ensure(server);
	int t = 2;
	struct wlr_scene_rect **e = server->resize_outline_edges;
	wlr_scene_rect_set_size(e[0], box->width, t);
	wlr_scene_node_set_position(&e[0]->node, box->x, box->y);
	wlr_scene_rect_set_size(e[1], box->width, t);
	wlr_scene_node_set_position(&e[1]->node, box->x,
		box->y + box->height - t);
	wlr_scene_rect_set_size(e[2], t, box->height);
	wlr_scene_node_set_position(&e[2]->node, box->x, box->y);
	wlr_scene_rect_set_size(e[3], t, box->height);
	wlr_scene_node_set_position(&e[3]->node, box->x + box->width - t,
		box->y);
	wlr_scene_node_set_enabled(&server->resize_outline->node, true);
}

static void resize_outline_hide(struct server *server) {
	if (server->resize_outline != NULL) {
		wlr_scene_node_set_enabled(&server->resize_outline->node, false);
	}
}

/* outline mode: after the button release the final size is sent and the
 * grab stays until the client commits it (so the top/left reposition and
 * the resize cursor stay put).  A client that never commits - hung, or one
 * ignoring the configure - would leave the grab and the resize cursor
 * stuck forever, so a watchdog force-ends the grab. */
static int resize_final_timeout_cb(void *data) {
	struct server *server = data;
	if (server->resizing && server->resize_final_pending) {
		resize_grab_clear(server);
	}
	return 0;
}

static void arm_resize_final_timer(struct server *server) {
	if (server->resize_final_timer == NULL) {
		struct wl_event_loop *loop =
			wl_display_get_event_loop(server->display);
		server->resize_final_timer =
			wl_event_loop_add_timer(loop, resize_final_timeout_cb, server);
		if (server->resize_final_timer == NULL) {
			return; /* no watchdog: the grab still ends on the commit */
		}
	}
	wl_event_source_timer_update(server->resize_final_timer,
		CONFIG_RESIZE_FINAL_TIMEOUT_MS);
}

static void disarm_resize_final_timer(struct server *server) {
	if (server->resize_final_timer != NULL) {
		wl_event_source_timer_update(server->resize_final_timer, 0);
	}
}

void begin_resize(struct server *server, struct toplevel *tl,
		uint32_t edges) {
	/* never resize a maximized window */
	if (server->resizing || tl->xdg_toplevel->current.maximized) {
		return;
	}
	server->resizing = true;
	server->input_mode = INPUT_MODE_RESIZE;
	server->resize_toplevel = tl;
	tl->user_moved = true; /* user resize: stop auto-centering */
	server->resize_edges = edges;
	server->resize_final_pending = false;
	server->press_x = server->cursor->x;
	server->press_y = server->cursor->y;
	toplevel_box(tl, &server->resize_orig);
	/* a grab that has not moved yet must not re-request the current size */
	server->resize_last_w = server->resize_orig.width;
	server->resize_last_h = server->resize_orig.height;
	if (CONFIG_RESIZE_DRAW_CONTENTS) {
		wlr_xdg_toplevel_set_resizing(tl->xdg_toplevel, true);
	} else {
		/* outline mode: show the starting box, apply the real size later */
		server->resize_target = server->resize_orig;
		resize_outline_show(server, &server->resize_target);
	}
	focus_toplevel(server, tl);
}

static void update_resize(struct server *server) {
	struct toplevel *tl = server->resize_toplevel;
	if (tl == NULL || tl->xdg_toplevel->base == NULL ||
			tl->xdg_toplevel->current.maximized) {
		return; /* never adjust a maximized window */
	}
	double dx = server->cursor->x - server->press_x;
	double dy = server->cursor->y - server->press_y;
	struct wlr_box orig = server->resize_orig;

	/* compute the target size with rounding instead of (int) truncation:
	 * truncating kept the dragged edge up to 1 px behind the cursor and
	 * quantized sub-pixel mouse motion asymmetrically, so the edge wobbled
	 * under the cursor while a move (which uses doubles end to end) was
	 * smooth. */
	int nw = orig.width;
	int nh = orig.height;
	if ((server->resize_edges & WLR_EDGE_RIGHT) != 0) {
		nw = orig.width + (int)llround(dx);
	}
	if ((server->resize_edges & WLR_EDGE_LEFT) != 0) {
		nw = orig.width - (int)llround(dx);
	}
	if ((server->resize_edges & WLR_EDGE_BOTTOM) != 0) {
		nh = orig.height + (int)llround(dy);
	}
	if ((server->resize_edges & WLR_EDGE_TOP) != 0) {
		nh = orig.height - (int)llround(dy);
	}

	/* clamp to the client's constraints (or a sane fallback) */
	int min_w = tl->xdg_toplevel->current.min_width > 0
		? tl->xdg_toplevel->current.min_width : 40;
	int min_h = tl->xdg_toplevel->current.min_height > 0
		? tl->xdg_toplevel->current.min_height : 30;
	int max_w = tl->xdg_toplevel->current.max_width > 0
		? tl->xdg_toplevel->current.max_width : INT_MAX;
	int max_h = tl->xdg_toplevel->current.max_height > 0
		? tl->xdg_toplevel->current.max_height : INT_MAX;
	if (nw < min_w) {
		nw = min_w;
	}
	if (nw > max_w) {
		nw = max_w;
	}
	if (nh < min_h) {
		nh = min_h;
	}
	if (nh > max_h) {
		nh = max_h;
	}

	/* the target box: a top/left grab moves the window origin so the
	 * opposite edge stays anchored where it was when the grab started */
	struct wlr_box box = {
		.x = (server->resize_edges & WLR_EDGE_LEFT) != 0
			? orig.x + orig.width - nw : orig.x,
		.y = (server->resize_edges & WLR_EDGE_TOP) != 0
			? orig.y + orig.height - nh : orig.y,
		.width = nw,
		.height = nh,
	};

	if (!CONFIG_RESIZE_DRAW_CONTENTS) {
		/* outline mode (labwc-style): only follow the cursor with the
		 * outline; apply the real size once on release, so the dragged
		 * edge never waits for the client's configure -> commit round trip
		 * and the drag feels as tight as a window move. */
		if (box.x == server->resize_target.x &&
				box.y == server->resize_target.y &&
				box.width == server->resize_target.width &&
				box.height == server->resize_target.height) {
			return;
		}
		server->resize_target = box;
		resize_outline_show(server, &server->resize_target);
		return;
	}

	/* live mode: only send a configure when the target size actually
	 * changed; wlr_xdg_toplevel_set_size always schedules one, so repeating
	 * the same size on every motion event (sub-pixel deltas, or a client
	 * that has not committed yet) would push a configure -> commit -> mask/
	 * border GPU re-render cycle through the client on every event.  That
	 * synchronous per-commit GL work is what makes a resize drag stutter
	 * (and a software cursor with it) while a move - no round trip, no
	 * GL - stays smooth. */
	if (nw == server->resize_last_w && nh == server->resize_last_h) {
		return;
	}
	server->resize_last_w = nw;
	server->resize_last_h = nh;
	wlr_xdg_toplevel_set_size(tl->xdg_toplevel, nw, nh);
}

void resize_grab_clear(struct server *server) {
	server->resizing = false;
	server->input_mode = INPUT_MODE_PASSTHROUGH;
	server->resize_toplevel = NULL;
	server->resize_edges = 0;
	server->resize_final_pending = false;
	/* a late motion between the release and the final commit can re-show
	 * the outline; hide it here so no ghost outline survives the grab */
	resize_outline_hide(server);
	disarm_resize_final_timer(server);
}

void end_resize(struct server *server) {
	if (!server->resizing) {
		return;
	}
	struct toplevel *tl = server->resize_toplevel;

	if (tl != NULL && tl->xdg_toplevel->base != NULL) {
		if (CONFIG_RESIZE_DRAW_CONTENTS) {
			wlr_xdg_toplevel_set_resizing(tl->xdg_toplevel, false);
			resize_grab_clear(server);
			return;
		}
		/* outline mode: hide the outline and apply the final box.  Keep the
		 * grab (resizing stays true) until the client commits the new
		 * geometry, so the cursor stays resize-shaped and the top/left
		 * reposition happens in xdg_toplevel_commit() against the committed
		 * size (no one-frame bounce, no transient default cursor). */
		resize_outline_hide(server);
		struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
		if (!base->surface->mapped) {
			resize_grab_clear(server);
			return;
		}
		if (server->resize_target.width != base->geometry.width ||
				server->resize_target.height != base->geometry.height) {
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel,
				server->resize_target.width,
				server->resize_target.height);
			server->resize_final_pending = true; /* finish on commit */
			arm_resize_final_timer(server); /* watchdog */
		} else {
			resize_grab_clear(server); /* size unchanged: nothing will commit */
		}
		return;
	}
	resize_grab_clear(server);
}

/* ------------------------------------------------------------------ */
/* chord gestures                                                      */
/* ------------------------------------------------------------------ */

/* Hold one mouse button, then press the other:
 *   right held + double-click left -> toggle maximize / restore
 *   left held + double-click right -> close the window
 *   hold the other button          -> move the window under the cursor
 *                                    (cursor turns to CONFIG_MOVE_CURSOR; releasing
 *                                    restores the previous cursor style) */

/* start moving the chord's window with the cursor; the grab anchors at the
 * trigger button's press point (server->press_x/press_y), so the whole drag
 * distance is honored; a maximized window is restored first so the drag
 * grips its restored geometry (Windows behavior, same as the title-strip
 * drag) */
static void begin_chord_move(struct server *server) {
	struct toplevel *tl = server->chord_toplevel;
	if (tl == NULL || server->moving || server->resizing) {
		return;
	}
	double ref_x = server->press_x;
	double ref_y = server->press_y;
	if (tl->xdg_toplevel->current.maximized) {
		restore_maximized_toplevel(tl, false); /* drag: grab owns the geometry */
		struct wlr_box rb = tl->restore_box;
		/* restore_maximized_toplevel clamps the restored position into the
		 * work area, so grip the cursor on the window's actual box */
		rb.x = tl->scene_tree->node.x;
		rb.y = tl->scene_tree->node.y;
		if (tl->has_restore_box && rb.width > 0) {
			if (ref_x < rb.x) {
				ref_x = rb.x;
			}
			if (ref_x > rb.x + rb.width - 1) {
				ref_x = rb.x + rb.width - 1;
			}
			if (ref_y < rb.y) {
				ref_y = rb.y;
			}
			if (ref_y > rb.y + rb.height - 1) {
				ref_y = rb.y + rb.height - 1;
			}
		}
	}
	focus_toplevel(server, tl);
	begin_move(server, tl, ref_x, ref_y);
	move_toplevel_to(server, server->cursor->x, server->cursor->y);
	server->chord_moving = true;
	update_cursor_style(server); /* CONFIG_MOVE_CURSOR */
}

static void disarm_chord_timer(struct server *server) {
	if (server->chord_timer != NULL) {
		wl_event_source_timer_update(server->chord_timer, 0);
	}
}

/* the disambiguation timer fired while the trigger button is still held:
 * it is a hold, not the first click of a double-click -> move the window */
static int chord_timer_cb(void *data) {
	struct server *server = data;
	if (server->chord_active && server->chord_pending &&
			!server->chord_moving && server->chord_toplevel != NULL &&
			!server->moving && !server->resizing) {
		bool held = (server->chord_button == BTN_LEFT &&
			server->left_button_held) ||
			(server->chord_button == BTN_RIGHT &&
			server->right_button_held);
		if (held) {
			server->chord_pending = false;
			begin_chord_move(server);
		}
	}
	return 0; /* leave the source armed for the next chord */
}

static void arm_chord_timer(struct server *server) {
	if (server->chord_timer == NULL) {
		struct wl_event_loop *loop =
			wl_display_get_event_loop(server->display);
		server->chord_timer =
			wl_event_loop_add_timer(loop, chord_timer_cb, server);
		if (server->chord_timer == NULL) {
			return; /* no timer: double-click still works, hold can't move */
		}
	}
	wl_event_source_timer_update(server->chord_timer,
		CONFIG_DOUBLE_CLICK_NS / 1000000);
}

/* fully end a chord gesture and every grab it started */
static void end_chord(struct server *server) {
	disarm_chord_timer(server);
	disarm_zone_timer(server);
	server->moving = false;
	server->input_mode = INPUT_MODE_PASSTHROUGH;
	server->move_deferred_restore = false;
	server->move_toplevel = NULL;
	server->zone_toplevel = NULL;
	server->zone_press = false;
	server->dragged = false;
	server->zone_action = ZONE_NONE;
	server->chord_active = false;
	server->chord_pending = false;
	server->chord_moving = false;
}

/* the double-click action depends on which button is held:
 * right held + double-click left -> toggle maximize / restore
 * left held + double-click right -> close the window */
static void chord_double_click(struct server *server, uint32_t chord_button) {
	struct toplevel *tl = server->chord_toplevel;
	if (tl == NULL) {
		return;
	}
	if (chord_button == BTN_LEFT) {
		/* the held button is the right one */
		focus_toplevel(server, tl);
		if (tl->xdg_toplevel->current.maximized) {
			restore_maximized_toplevel(tl, true);
		} else {
			set_maximized(server, tl, true);
		}
	} else {
		/* the held button is the left one */
		close_toplevel(tl);
	}
}

/* the other button was pressed while one is held: remember which presses
 * the compositor consumed (so their releases are swallowed too) and tell a
 * double-click (toggle maximize) from a hold (move) */
static void begin_chord(struct server *server, uint32_t button) {
	struct toplevel *tl = toplevel_at(server);
	server->chord_active = true;
	server->chord_button = button;
	server->chord_toplevel = tl;
	server->chord_pending = false;
	server->chord_moving = false;

	/* the trigger button's press is swallowed by the compositor */
	server->press_x = server->cursor->x;
	server->press_y = server->cursor->y;
	if (button == BTN_LEFT) {
		server->chord_swallow_left = true;
	} else {
		server->chord_swallow_right = true;
	}
	/* NOTE: the held button's release needs no extra swallow flag here -
	 * if its press was swallowed as a zone press it is already in
	 * bound_buttons, and its release is swallowed either by
	 * process_chord_button (zone_press) or by the generic release path
	 * (was_bound). */

	if (is_double_click(server, button)) {
		/* double-clicked the other button: the action depends on which
		 * button is held (right held -> maximize toggle, left held ->
		 * close) */
		server->last_was_click = false; /* consumed by the double click */
		server->chord_active = false;
		chord_double_click(server, button);
		return;
	}

	/* first press: it may become a double-click (maximize) or a hold
	 * (move); the timer disambiguates */
	server->chord_pending = true;
	arm_chord_timer(server);
}

/* button events while a chord is active: the trigger button decides
 * double-click vs hold; releasing either button ends the gesture */
static void process_chord_button(struct server *server, uint32_t time_msec,
		uint32_t button, enum wl_pointer_button_state state) {
	if (button != BTN_LEFT && button != BTN_RIGHT) {
		return; /* other buttons don't participate in the chord */
	}

	if (button == server->chord_button) {
		if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
			/* this press is consumed by the chord: swallow its release
			 * too (even when the chord ends right here) */
			if (button == BTN_LEFT) {
				server->chord_swallow_left = true;
			} else {
				server->chord_swallow_right = true;
			}
			/* second press within the double-click window: the user
			 * double-clicked the other button -> maximize (right held)
			 * or close (left held) */
			if (is_double_click(server, button)) {
				server->last_was_click = false; /* consumed */
				end_chord(server);
				chord_double_click(server, button);
			}
			return;
		}
		/* release of the trigger button */
		if (server->chord_pending) {
			/* it was a click, not a hold: remember it so a quick second
			 * press is recognized as a double-click */
			disarm_chord_timer(server);
			clock_gettime(CLOCK_MONOTONIC, &server->last_release_time);
			server->last_was_click = true;
			server->last_click_button = button;
			server->chord_pending = false;
		} else if (server->chord_moving) {
			/* release ends the move and restores the cursor style */
			end_chord(server);
			update_cursor_style(server);
		} else {
			end_chord(server);
		}
		if (button == BTN_LEFT) {
			server->chord_swallow_left = false;
		} else {
			server->chord_swallow_right = false;
		}
		return;
	}

	/* the held button was released: end the chord; forward its release
	 * only if its press reached the client (otherwise it was a swallowed
	 * zone press and the release must stay swallowed) */
	if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
		bool zone = server->zone_press;
		/* a swallowed zone press was recorded in bound_buttons; clear it
		 * here so a stale entry can never swallow a later release */
		bound_button_remove(&server->bound_buttons, button);
		end_chord(server);
		if (!zone) {
			wlr_seat_pointer_notify_button(server->seat, time_msec,
				button, state);
			wlr_seat_pointer_notify_frame(server->seat);
		}
		if (button == BTN_LEFT) {
			server->chord_swallow_left = false;
		} else {
			server->chord_swallow_right = false;
		}
	}
}

/* ------------------------------------------------------------------ */
/* cursor event processing                                            */
/* ------------------------------------------------------------------ */

/* surface-local coordinates of a layout point for a toplevel's surface,
 * even when the point lies outside the surface (used to keep forwarding
 * motion to the grabbed surface during an implicit grab).  Returns false
 * when the surface is not a toplevel surface. */
static bool toplevel_surface_coords(struct server *server,
		struct wlr_surface *surface, double lx, double ly,
		double *sx, double *sy) {
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
		if (base == NULL || base->surface != surface) {
			continue;
		}
		*sx = lx - tl->scene_tree->node.x + base->geometry.x;
		*sy = ly - tl->scene_tree->node.y + base->geometry.y;
		return true;
	}
	return false;
}

static void process_cursor_motion(struct server *server, uint32_t time_msec) {
	if (server->drag_tree != NULL) {
		wlr_scene_node_set_position(&server->drag_tree->node,
			server->cursor->x, server->cursor->y);
	}

	/* keep the input method's candidate window glued to the cursor */
	ime_update_popup(server);

	if (server->moving && server->move_toplevel != NULL) {
		move_toplevel_to(server, server->cursor->x, server->cursor->y);
		update_cursor_style(server);
		return;
	}

	if (server->resizing && server->resize_toplevel != NULL) {
		update_resize(server);
		update_cursor_style(server);
		return;
	}

	/* a chord's trigger is held and the cursor moves: start the move right
	 * away instead of waiting for the disambiguation timer (a quick release
	 * without motion stays a click, so double-clicks still work) */
	if (server->chord_active && server->chord_pending &&
			!server->chord_moving && !server->moving) {
		double dx = server->cursor->x - server->press_x;
		double dy = server->cursor->y - server->press_y;
		if (dx * dx + dy * dy > CONFIG_DRAG_THRESHOLD * CONFIG_DRAG_THRESHOLD) {
			server->chord_pending = false;
			begin_chord_move(server);
			return; /* this motion started the move; don't forward it */
		}
	}

	if (server->zone_press && server->zone_toplevel != NULL &&
			!server->chord_active) {
		if (!server->dragged) {
			double dx = server->cursor->x - server->press_x;
			double dy = server->cursor->y - server->press_y;
			if (dx * dx + dy * dy > CONFIG_DRAG_THRESHOLD * CONFIG_DRAG_THRESHOLD) {
				/* a press on the title strip that moves becomes a move grab */
				begin_zone_drag(server);
			}
		} else if (server->moving) {
			move_toplevel_to(server, server->cursor->x, server->cursor->y);
		}
		return;
	}

	/* normal path: forward pointer motion to the surface under the cursor */
	double sx, sy;
	struct wlr_surface *surface = NULL;
	/* While a window morphs (animate.c: maximize/restore zoom, or the
	 * scale part of an open/close fade) its visible content is the rounded
	 * FBO scaled into the morph box, but the scene only ever hit-tests the
	 * raw client surface at its natural geometry.  A cursor over the
	 * morphing window's visible area that the scene attributes to a window
	 * below (restore / shrink zoom) must reach the morphing window
	 * instead, with coordinates mapped through the morph scale.  Popups
	 * and layer-shell surfaces float above the windows and always win.
	 * The extra hit tests only run while some window actually morphs. */
	struct toplevel *morph_tl = NULL;
	bool any_morph = false;
	{
		struct toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {
			if (t->morph_active) {
				any_morph = true;
				break;
			}
		}
	}
	if (any_morph && !pointer_over_popup(server) &&
			!pointer_over_layer_surface(server)) {
		morph_tl = toplevel_morph_at(server, server->cursor->x,
			server->cursor->y);
	}
	if (morph_tl != NULL && morph_tl->xdg_toplevel != NULL &&
			morph_tl->xdg_toplevel->base != NULL) {
		struct wlr_box box;
		toplevel_box(morph_tl, &box);
		surface = morph_tl->xdg_toplevel->base->surface;
		if (morph_tl->morph_w > 0 && morph_tl->morph_h > 0 &&
				box.width > 0 && box.height > 0) {
			/* map the cursor (inside the morph box) through the morph
			 * scale onto the content's natural geometry */
			sx = (server->cursor->x - morph_tl->morph_x) *
				(double)box.width / (double)morph_tl->morph_w;
			sy = (server->cursor->y - morph_tl->morph_y) *
				(double)box.height / (double)morph_tl->morph_h;
		} else {
			sx = 0.0;
			sy = 0.0;
		}
	} else {
		struct wlr_scene_node *node = wlr_scene_node_at(
			&server->scene->tree.node, server->cursor->x, server->cursor->y,
			&sx, &sy);
		if (node != NULL && node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_buffer *buffer =
				wlr_scene_buffer_from_node(node);
			struct wlr_scene_surface *scene_surface =
				wlr_scene_surface_try_from_buffer(buffer);
			if (scene_surface != NULL) {
				surface = scene_surface->surface;
			} else {
				/* the hit buffer is the compositor's rounded-corner masked
				 * content re-render: resolve the xdg surface via the scene
				 * tag on its tree */
				struct wlr_scene_node *n = node;
				while (n != NULL) {
					if (n->data != NULL) {
						struct scene_tag *tag = n->data;
						if (tag->type == TAG_POPUP) {
							/* a rounded popup (Qt menu): the hit buffer is its
							 * masked re-render; resolve the popup surface */
							struct wlr_xdg_popup *popup = tag->ptr;
							if (popup != NULL && popup->base != NULL) {
								surface = popup->base->surface;
							}
						} else if (tag->type == TAG_TOPLEVEL) {
							struct toplevel *tl = tag->ptr;
							if (tl->xdg_toplevel->base != NULL) {
								surface = tl->xdg_toplevel->base->surface;
							}
						}
						break; /* a closer tagged object won the hit test */
					}
					n = n->parent != NULL ? &n->parent->node : NULL;
				}
			}
		}
	}

	/* implicit grab: a client holds a pointer button (e.g. text selection).
	 * Keep the pointer focused on the grabbed surface and keep forwarding
	 * motion to it, without switching focus or changing the cursor, until
	 * the button is released. */
	if (server->seat->pointer_state.button_count > 0) {
		struct wlr_surface *focused =
			server->seat->pointer_state.focused_surface;
		if (focused != NULL) {
			if (surface == focused) {
				wlr_seat_pointer_notify_motion(server->seat, time_msec,
					sx, sy);
			} else {
				double gx, gy;
				if (toplevel_surface_coords(server, focused,
						server->cursor->x, server->cursor->y,
						&gx, &gy)) {
					wlr_seat_pointer_notify_motion(server->seat, time_msec,
						gx, gy);
				}
			}
		}
		return;
	}

	if (server->seat->pointer_state.focused_surface != surface) {
		clear_cursor_override(server);
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
			"left_ptr");
	}
	update_cursor_style(server);
	if (surface != NULL) {
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(server->seat);
	}
}

static void process_cursor_button(struct server *server, uint32_t time_msec,
		uint32_t button, enum wl_pointer_button_state state) {
	/* A layer-shell overlay (e.g. the start menu) can be destroyed while the
	 * cursor is over it; wlroots then clears the pointer focus and it is
	 * only re-established by a motion event, so the first click after the
	 * overlay disappears would be dropped by the seat. Re-run the hit test
	 * on the first press so the surface under the cursor gets the click. */
	if (state == WL_POINTER_BUTTON_STATE_PRESSED
			&& server->seat->pointer_state.button_count == 0
			&& server->seat->pointer_state.focused_surface == NULL) {
		process_cursor_motion(server, time_msec);
	}

	/* remember whether each button is held: the wheel gestures (scroll up =
	 * maximize, scroll down = minimize) key off the right button, and the
	 * chord gestures key off both */
	if (button == BTN_LEFT) {
		server->left_button_held =
			state == WL_POINTER_BUTTON_STATE_PRESSED;
	}
	if (button == BTN_RIGHT) {
		server->right_button_held =
			state == WL_POINTER_BUTTON_STATE_PRESSED;
		if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
			/* a fresh press starts a fresh scroll burst */
			server->wheel_burst_start.tv_sec = 0;
			server->wheel_burst_start.tv_nsec = 0;
			server->wheel_last_tick.tv_sec = 0;
			server->wheel_last_tick.tv_nsec = 0;
		}
	}

	if (server->chord_active) {
		process_chord_button(server, time_msec, button, state);
		return;
	}

	/* start a chord: one button is held and the other one is pressed.
	 * Gated on CONFIG_WHEEL_DEBOUNCE_ENABLED: with the gestures master
	 * switch off, chords never start and both presses fall through to
	 * the client as ordinary clicks. */
	if (CONFIG_WHEEL_DEBOUNCE_ENABLED &&
			state == WL_POINTER_BUTTON_STATE_PRESSED &&
			!server->resizing && !server->moving &&
			((button == BTN_LEFT && server->right_button_held) ||
			 (button == BTN_RIGHT && server->left_button_held))) {
		begin_chord(server, button);
		return;
	}

	if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
		/* single decision point (labwc-style): every press the compositor
		 * swallowed was recorded in bound_buttons, so the release reaches
		 * the client only when it was NOT recorded. */
		bool was_bound = bound_button_contains(&server->bound_buttons,
			button);
		bound_button_remove(&server->bound_buttons, button);

		/* a chord consumed this button's press but ended earlier (e.g. the
		 * held button was released first): swallow the release so the client
		 * never sees an orphan release without a matching press */
		if ((button == BTN_LEFT && server->chord_swallow_left) ||
				(button == BTN_RIGHT && server->chord_swallow_right)) {
			if (button == BTN_LEFT) {
				server->chord_swallow_left = false;
			} else {
				server->chord_swallow_right = false;
			}
			return;
		}

		if (server->resizing) {
			end_resize(server);
			if (!was_bound) {
				/* client-initiated resize (xdg_toplevel.resize): its press
				 * was forwarded, so the release must be too */
				wlr_seat_pointer_notify_button(server->seat, time_msec,
					button, state);
				wlr_seat_pointer_notify_frame(server->seat);
			}
			update_cursor_style(server);
			return;
		}
		if (server->moving) {
			end_move(server);
			if (!was_bound) {
				/* client-initiated move (xdg_toplevel.move): its press was
				 * forwarded, so the release must be too */
				wlr_seat_pointer_notify_button(server->seat, time_msec,
					button, state);
				wlr_seat_pointer_notify_frame(server->seat);
			}
			update_cursor_style(server);
			return;
		}
		if (server->zone_press) {
			/* release of a swallowed zone press */
			struct toplevel *tl = server->zone_toplevel;
			server->zone_press = false;
			server->zone_toplevel = NULL;
			disarm_zone_timer(server);
			if (!server->dragged) {
				if (server->zone_action != ZONE_NONE && tl != NULL) {
					/* the second click of a double click on the title strip:
					 * the segment under the cursor decides the action */
					switch (server->zone_action) {
					case ZONE_MINIMIZE:
						set_minimized(server, tl, true);
						break;
					case ZONE_MAXIMIZE:
						focus_toplevel(server, tl);
						if (tl->xdg_toplevel->current.maximized) {
							restore_maximized_toplevel(tl, true);
						} else {
							set_maximized(server, tl, true);
						}
						break;
					case ZONE_CLOSE:
						close_toplevel(tl);
						break;
					default:
						break;
					}
				} else {
					clock_gettime(CLOCK_MONOTONIC,
						&server->last_release_time);
					server->last_was_click = true;
					server->last_click_button = button;
				}
			} else {
				server->last_was_click = false;
			}
			server->dragged = false;
			server->zone_action = ZONE_NONE;
			update_cursor_style(server);
			return;
		}
		/* normal path: forward the release only if its press reached the
		 * client.  A swallowed press whose grab ended early (e.g. its window
		 * was destroyed) has its release swallowed here, never orphaned. */
		if (!was_bound) {
			wlr_seat_pointer_notify_button(server->seat, time_msec,
				button, state);
			process_cursor_motion(server, time_msec);
		}
		return;
	}

	/* WLR_BUTTON_PRESSED */
	if (server->moving || server->zone_press || server->resizing) {
		bound_button_add(&server->bound_buttons, button);
		return; /* already grabbed: swallow, release must be swallowed too */
	}

	/* the cursor is on a layer-shell surface (taskbar / menu overlay): the
	 * click belongs to it, never start a window move/resize grab through it */
	if (pointer_over_layer_surface(server)) {
		wlr_seat_pointer_notify_button(server->seat, time_msec, button, state);
		return;
	}

	/* an implicit grab is active: a previous press was forwarded to the
	 * client and is still held (buttons > 0). Forward this press too
	 * instead of hijacking it for a resize/move grab, so the client sees
	 * the whole multi-button gesture and the cursor stays under the
	 * implicit-grab rules. */
	if (server->seat->pointer_state.button_count > 0) {
		struct toplevel *tl = toplevel_nearby(server);
		if (tl != NULL && !tl->minimized) {
			focus_toplevel(server, tl);
		}
		wlr_seat_pointer_notify_button(server->seat, time_msec, button,
			state);
		return;
	}

	struct toplevel *tl = toplevel_nearby(server);
	if (tl != NULL && !tl->minimized) {
		uint32_t edges = toplevel_resize_edges(server, tl);
		/* the corner resize zones win over the title strip; everywhere
		 * else the top strip stays the title bar */
		if (edges != 0 &&
				((edges & WLR_EDGE_TOP) != 0 ||
				 !is_in_titlebar_zone(server, tl))) {
			bound_button_add(&server->bound_buttons, button);
			begin_resize(server, tl, edges);
			return;
		}
	}
	if (tl != NULL && !tl->minimized && is_in_titlebar_zone(server, tl)) {
		/* take over: the colored top strip is our visible title bar */
		focus_toplevel(server, tl);
		bound_button_add(&server->bound_buttons, button);
		server->zone_press = true;
		server->zone_toplevel = tl;
		server->zone_button = button;
		server->press_x = server->cursor->x;
		server->press_y = server->cursor->y;
		server->dragged = false;
		if (is_double_click(server, button)) {
			/* second click of a double click: arm the action for the
			 * segment under the cursor (release triggers it) */
			server->last_was_click = false; /* consumed by the double click */
			server->zone_action = title_strip_action(server, tl);
		} else {
			server->zone_action = ZONE_NONE;
			/* a quick release stays a click (two clicks = double click);
			 * holding still past CONFIG_LONG_PRESS_NS grabs the window for
			 * moving - the timer disambiguates */
			arm_zone_timer(server);
		}
		return;
	}

	if (tl != NULL && !tl->minimized) {
		focus_toplevel(server, tl);
	}
	wlr_seat_pointer_notify_button(server->seat, time_msec, button, state);
}

/* right-hold + wheel gestures (wheel up toggles maximize/restore, wheel
 * down minimizes): a trackpad flick or a high-resolution wheel delivers
 * many ticks in one burst, which would toggle the state several times.
 * Two thresholds decide whether a tick is the same gesture or the next
 * action (only reached when CONFIG_WHEEL_DEBOUNCE_ENABLED is true - the
 * caller gates the whole gesture on the master switch):
 *   - CONFIG_WHEEL_BURST_NS: one continuous scroll (however fast) counts
 *     as one action for at most this long; ticks past the burst start +
 *     this are a new action.
 *   - CONFIG_WHEEL_TICK_GAP_NS: two ticks at least this far apart are the
 *     next action, even inside the burst window. */
static bool wheel_action_allowed(struct server *server) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t gap = (now.tv_sec - server->wheel_last_tick.tv_sec) * 1000000000L
		+ (now.tv_nsec - server->wheel_last_tick.tv_nsec);
	int64_t dur = (now.tv_sec - server->wheel_burst_start.tv_sec) * 1000000000L
		+ (now.tv_nsec - server->wheel_burst_start.tv_nsec);
	server->wheel_last_tick = now;
	if (gap >= 0 && gap < CONFIG_WHEEL_TICK_GAP_NS &&
			dur >= 0 && dur <= CONFIG_WHEEL_BURST_NS) {
		return false; /* same gesture: tick came fast and the burst is
		                * still within its max length */
	}
	/* a new gesture: either the previous tick was too long ago (gap) or
	 * the burst outlived its max length (duration) */
	server->wheel_burst_start = now;
	return true;
}

/* the toplevel the wheel gesture acts on: the window under the cursor, or
 * - when the cursor is over the spot of a minimized (hidden) window - that
 * window, so scrolling down can restore it */
static struct toplevel *toplevel_at_or_minimized(struct server *server) {
	struct toplevel *tl = toplevel_at(server);
	if (tl != NULL && !tl->minimized && !tl->closing) {
		return tl;
	}
	struct toplevel *candidate;
	wl_list_for_each(candidate, &server->toplevels, link) {
		if (candidate->minimized && candidate->xdg_toplevel->base != NULL &&
				candidate->xdg_toplevel->base->surface->mapped) {
			struct wlr_box box;
			toplevel_box(candidate, &box);
			if (server->cursor->x >= box.x &&
					server->cursor->x < box.x + box.width &&
					server->cursor->y >= box.y &&
					server->cursor->y < box.y + box.height) {
				return candidate;
			}
		}
	}
	return NULL;
}

static void process_cursor_axis(struct server *server, uint32_t time_msec,
		struct wlr_pointer_axis_event *event) {
	struct toplevel *tl = toplevel_at_or_minimized(server);
	/* gated on the gestures master switch: with CONFIG_WHEEL_DEBOUNCE_
	 * ENABLED false the wheel does not grab the scroll - it is forwarded
	 * to the client below like any ordinary scroll */
	if (CONFIG_WHEEL_DEBOUNCE_ENABLED && tl != NULL &&
			server->right_button_held &&
			event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL &&
			event->delta_discrete != 0) {
		/* while holding the right mouse button over a window:
		 * wheel up (negative axis value) toggles maximize / restore,
		 * wheel down (positive axis value) toggles minimize / restore.
		 * Two thresholds coalesce rapid ticks: one continuous scroll is
		 * one action for at most CONFIG_WHEEL_BURST_NS, and two ticks at
		 * least CONFIG_WHEEL_TICK_GAP_NS apart are the next action. */
		if (!wheel_action_allowed(server)) {
			return; /* swallowed: still inside the same gesture */
		}
		if (event->delta_discrete < 0) {
			if (tl->xdg_toplevel->current.maximized) {
				/* already maximized: restore the saved geometry */
				restore_maximized_toplevel(tl, true);
			} else {
				set_maximized(server, tl, true);
			}
		} else if (tl->minimized) {
			/* already minimized: restore (show) the window */
			set_minimized(server, tl, false);
		} else {
			set_minimized(server, tl, true);
		}
		return;
	}
	wlr_seat_pointer_notify_axis(server->seat, time_msec, event->orientation,
		event->delta, event->delta_discrete, event->source,
		event->relative_direction);
}

void cursor_motion(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

void cursor_motion_absolute(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
		event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

void cursor_button(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	process_cursor_button(server, event->time_msec, event->button,
		event->state);
}

void cursor_axis(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	process_cursor_axis(server, event->time_msec, event);
}

void cursor_frame(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}
