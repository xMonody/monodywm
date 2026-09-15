/*
 * animate.c - window animations
 *
 *   create (map)   -> fade in  (opacity 0 -> 1)
 *   close          -> fade out (opacity 1 -> 0, then the xdg close is
 *                     sent); the window turns inert the moment the close
 *                     is requested (closing flag) and its scene node is
 *                     disabled once it is fully transparent, so a client
 *                     that never honors the close cannot leave an
 *                     invisible, input-blocking window behind.  Closing a
 *                     window that is still falling (minimize) keeps the
 *                     drop while it fades; closing right after map fades
 *                     from the current opacity instead of flashing back.
 *   minimize       -> the window falls straight down from its current
 *                     position until it is fully below the bottom edge of
 *                     its output, then its scene node is hidden (it snaps
 *                     back to its resting origin while hidden, so geometry
 *                     bookkeeping stays untouched)
 *   restore        -> the hidden window drops back down from above its own
 *                     top edge and lands exactly where it was
 *
 * Motion is applied to the toplevel's scene-tree node (position), fades to
 * the visible window buffer(s) via rounded_window_set_opacity() - so the
 * rounded corner copy, the border and the shadow all move/fade together.
 *
 * Timing model: the animation state is NOT advanced by a per-window timer
 * whose phase drifts against the display refresh.  Instead each output's
 * frame handler (output.c) advances all running animations to its own
 * CLOCK_MONOTONIC instant right before the scene is rendered, so every
 * rendered frame shows exactly the eased state for its own vblank - no
 * beat between an update timer and the vblank, and high-refresh outputs
 * interpolate proportionally more states.  The scene damage each state
 * change creates keeps the output rendering every vblank while the
 * animation lasts.  A single server-wide pacing watchdog (armed only while
 * any animation runs, at a fixed 16 ms floor - see ANIM_WATCHDOG_MS)
 * covers the phases damage cannot
 * drive on its own: a window fully outside its output (drop-out tail /
 * drop-in head, whose damage may be clipped away) and the wall-clock
 * timeouts (a maximize zoom waiting for a client that never commits the
 * target size).
 *
 * All entry points return false (and change nothing) when animations are
 * disabled or cannot run, so the caller falls back to the instant,
 * animation-less behavior.
 */

#include "server.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>

#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

/* pacing watchdog floor: how often the watchdog may request an output
 * frame while a run is active.  This is NOT an animation frame rate - the
 * animation state advances once per rendered frame at the output's own
 * vblank, fully synced to the compositor (see the file comment).  The
 * watchdog only keeps the invisible phases of a run (window fully outside
 * its output, whose damage is clipped away) and the wall-clock timeouts
 * progressing; 16 ms is short enough that no run ever ends or re-enters
 * visibly late on any output. */
#define ANIM_WATCHDOG_MS 16

/* kinds of animation one window can run at a time */
enum anim_kind {
	ANIM_NONE = 0,
	ANIM_FADE_IN,   /* map: opacity 0 -> 1, no motion           */
	ANIM_FADE_OUT,  /* close: opacity 1 -> 0, then send close   */
	ANIM_FALL_OUT,  /* minimize: fall off the screen bottom     */
	ANIM_FALL_IN,   /* restore: drop in from above the window   */
	ANIM_GEOM,      /* maximize/restore: zoom between two boxes */
};

struct toplevel_anim {
	struct toplevel *tl;

	/* the window's scene tree died mid-animation (window closed): free the
	 * state so no code ever touches the freed toplevel again */
	struct wl_listener tree_destroy;

	enum anim_kind kind;

	/* resting origin: where the window belongs on screen.  Captured when a
	 * minimize starts (before the fall), so a restore that interrupts the
	 * fall still drops the window back to its real resting place. */
	bool has_rest;
	int rest_x, rest_y;

	/* current run: eased position / opacity interpolation endpoints */
	int from_x, from_y; /* position the animation starts from   */
	int to_x, to_y;     /* position the animation ends at       */
	float op_from, op_to; /* opacity range (fades only)        */
	float op_cur;         /* current applied opacity (fades): lets an
	                       * interrupting run start from where the previous
	                       * one actually left the window (close while the
	                       * map fade-in is still running) */
	int win_w, win_h;   /* window size (swept-area damage)      */

	/* minimize drop / restore drop-in scale (falls): while the window
	 * falls it shrinks around its own (moving) center down to
	 * CONFIG_ANIM_FALL_SCALE, the drop-in grows from there back to 1.0.
	 * Enabled per run only when the rounded FBO exists to scale. */
	bool scale_fall;
	float scale_from;   /* scale at p = 0: 1.0 (drop), CONFIG (drop-in) */

	/* ANIM_GEOM (Windows-style maximize/restore zoom): the window box
	 * interpolated between geom_from and geom_to.  The zoom starts with a
	 * wait phase - the final-size FBO only exists once the client commits
	 * the target size and the rounded cache is re-rendered at it - so it
	 * can never pop between the two content layouts. */
	struct wlr_box geom_from, geom_to;
	bool geom_zooming;  /* false = waiting for the target-size FBO */

	uint32_t start_ms;  /* CLOCK_MONOTONIC when the run started */
	uint32_t duration_ms;
};

static uint32_t mono_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000u);
}

/* one monotonic easing per kind: falling accelerates (gravity), landing
 * decelerates, fades are linear, the maximize/restore zoom uses a smooth
 * ease-in-out like Windows */
static float anim_ease(enum anim_kind kind, float t) {
	switch (kind) {
	case ANIM_FALL_OUT:
		return t * t; /* accelerating fall */
	case ANIM_FALL_IN:
		t = 1.0f - t; /* decelerating landing */
		return 1.0f - t * t * t;
	case ANIM_GEOM:
		return t * t * (3.0f - 2.0f * t); /* smoothstep */
	case ANIM_FADE_IN:
	case ANIM_FADE_OUT:
	default:
		return t;
	}
}

/* Duration of the maximize/restore zoom.  CONFIG_ANIM_MAXIMIZE_MS is the
 * base - its own independent knob, deliberately not tied to the minimize
 * fall time (CONFIG_ANIM_FALL_MS).  A very large zoom (a small window
 * filling the whole screen) gets a bounded extra time (span/12, at most
 * doubling the base) so its per-frame steps stay smooth at 60 Hz; the
 * boost is small enough that the knob keeps direct, predictable control
 * over how long the zoom feels. */
static uint32_t anim_geom_duration(const struct wlr_box *from,
		const struct wlr_box *to) {
	int ex = abs(to->x - from->x) + abs(to->width - from->width);
	int ey = abs(to->y - from->y) + abs(to->height - from->height);
	int span = ex > ey ? ex : ey;
	uint32_t dur = CONFIG_ANIM_MAXIMIZE_MS + (uint32_t)(span / 12u);
	uint32_t cap = CONFIG_ANIM_MAXIMIZE_MS * 2u;
	if (dur > cap) {
		dur = cap;
	}
	return dur;
}

/* force the outputs the swept strip touches to repaint */
static void anim_schedule_frames(struct server *server,
		const struct wlr_box *sweep) {
	struct wlr_output_layout_output *lo;
	wl_list_for_each(lo, &server->output_layout->outputs, link) {
		struct wlr_box obox;
		wlr_output_layout_get_box(server->output_layout, lo->output, &obox);
		struct wlr_box inter;
		if (wlr_box_intersection(&inter, &obox, sweep)) {
			wlr_output_schedule_frame(lo->output);
		}
	}
}

/* strip swept by the running animation (window box at from .. to, plus the
 * rounded-FBO/shadow padding), in layout coordinates */
static void anim_sweep_box(struct toplevel_anim *a, struct wlr_box *sweep) {
	int x0 = a->from_x < a->to_x ? a->from_x : a->to_x;
	int x1 = a->from_x > a->to_x ? a->from_x : a->to_x;
	int y0 = a->from_y < a->to_y ? a->from_y : a->to_y;
	int y1 = a->from_y > a->to_y ? a->from_y : a->to_y;
	int pad = shadow_padding() + 1;
	*sweep = (struct wlr_box){
		.x = x0 - pad,
		.y = y0 - pad,
		.width = (x1 - x0) + a->win_w + 2 * pad,
		.height = (y1 - y0) + a->win_h + 2 * pad,
	};
}

/* area touched by the maximize/restore zoom: the union of the from and to
 * boxes (padding included) */
static void geom_sweep_box(struct toplevel_anim *a, struct wlr_box *sweep) {
	int x0 = a->geom_from.x < a->geom_to.x ? a->geom_from.x : a->geom_to.x;
	int x1 = a->geom_from.x > a->geom_to.x ? a->geom_from.x : a->geom_to.x;
	int y0 = a->geom_from.y < a->geom_to.y ? a->geom_from.y : a->geom_to.y;
	int y1 = a->geom_from.y > a->geom_to.y ? a->geom_from.y : a->geom_to.y;
	int w0 = a->geom_from.width > a->geom_to.width ?
		a->geom_from.width : a->geom_to.width;
	int h0 = a->geom_from.height > a->geom_to.height ?
		a->geom_from.height : a->geom_to.height;
	int pad = shadow_padding() + 1;
	*sweep = (struct wlr_box){
		.x = x0 - pad,
		.y = y0 - pad,
		.width = (x1 - x0) + w0 + 2 * pad,
		.height = (y1 - y0) + h0 + 2 * pad,
	};
}

/* interpolate the window's morph box between geom_from (p=0) and geom_to
 * (p=1) into tl->morph_*; the caller (the maximize/restore zoom) then
 * anchors the scene tree at the box origin and scales the rounded FBO
 * into it (rounded_cache_morph_apply) */
static void morph_box_at(struct toplevel_anim *a, float p) {
	struct toplevel *tl = a->tl;
	if (p >= 1.0f) {
		tl->morph_x = a->geom_to.x;
		tl->morph_y = a->geom_to.y;
		tl->morph_w = a->geom_to.width;
		tl->morph_h = a->geom_to.height;
	} else {
		tl->morph_x = a->geom_from.x + (int)lroundf(
			(float)(a->geom_to.x - a->geom_from.x) * p);
		tl->morph_y = a->geom_from.y + (int)lroundf(
			(float)(a->geom_to.y - a->geom_from.y) * p);
		tl->morph_w = a->geom_from.width + (int)lroundf(
			(float)(a->geom_to.width - a->geom_from.width) * p);
		tl->morph_h = a->geom_from.height + (int)lroundf(
			(float)(a->geom_to.height - a->geom_from.height) * p);
	}
}

/* apply the interpolated state for progress p in [0,1] */
static void anim_apply(struct toplevel_anim *a, float p) {
	struct toplevel *tl = a->tl;
	switch (a->kind) {
	case ANIM_FALL_OUT:
	case ANIM_FALL_IN: {
		int nx = a->from_x + (int)lroundf(
			(float)(a->to_x - a->from_x) * p);
		int ny = a->from_y + (int)lroundf(
			(float)(a->to_y - a->from_y) * p);
		wlr_scene_node_set_position(&tl->scene_tree->node, nx, ny);
		/* the fall also shrinks/grows the rounded FBO around the window's
		 * own (moving) center: drop 1.0 -> CONFIG_ANIM_FALL_SCALE, drop-in
		 * CONFIG_ANIM_FALL_SCALE -> 1.0 */
		if (a->scale_fall) {
			float to = a->kind == ANIM_FALL_OUT ?
				CONFIG_ANIM_FALL_SCALE : 1.0f;
			float s = a->scale_from + (to - a->scale_from) * p;
			int mw = (int)lroundf(a->win_w * s);
			int mh = (int)lroundf(a->win_h * s);
			if (mw < 1) {
				mw = 1;
			}
			if (mh < 1) {
				mh = 1;
			}
			tl->morph_active = true;
			tl->morph_x = nx + (a->win_w - mw) / 2;
			tl->morph_y = ny + (a->win_h - mh) / 2;
			tl->morph_w = mw;
			tl->morph_h = mh;
			rounded_cache_morph_apply(tl);
		}
		break;
	}
	case ANIM_GEOM:
		/* the window (its rounded FBO, holding the content at the target
		 * size) is shown scaled into the interpolated box: move the scene
		 * tree to the box origin and let rounded.c scale the FBO into it */
		morph_box_at(a, p);
		wlr_scene_node_set_position(&tl->scene_tree->node,
			tl->morph_x, tl->morph_y);
		rounded_cache_morph_apply(tl);
		break;
	case ANIM_FADE_IN:
	case ANIM_FADE_OUT: {
		float op = a->op_from + (a->op_to - a->op_from) * p;
		rounded_window_set_opacity(tl, op);
		a->op_cur = op;
		/* a close that interrupted a minimize drop keeps the vertical
		 * motion of that drop (animate_toplevel_close) while it fades out,
		 * instead of freezing the window in mid-air: the run falls to the
		 * interrupted fall's off-screen target and vanishes there */
		if (a->from_x != a->to_x || a->from_y != a->to_y) {
			wlr_scene_node_set_position(&tl->scene_tree->node,
				a->from_x + (int)lroundf(
					(float)(a->to_x - a->from_x) * p),
				a->from_y + (int)lroundf(
					(float)(a->to_y - a->from_y) * p));
		}
		break;
	}
	default:
		break;
	}
}

static const char *anim_kind_name(enum anim_kind kind) {
	switch (kind) {
	case ANIM_FADE_IN:  return "fade-in";
	case ANIM_FADE_OUT: return "fade-out";
	case ANIM_FALL_OUT: return "minimize-drop";
	case ANIM_FALL_IN:  return "restore-drop";
	case ANIM_GEOM:     return "maximize-zoom";
	default:            return "none";
	}
}

/* tear down a running morph (maximize/restore zoom, or the shrink that a
 * minimize drop / restore drop-in applies): clear the morph state and
 * bring the FBO node back to a well-defined layout.
 *  - a zoom snaps the scene tree back to its target box (geometry) and
 *    lays the FBO out at its natural (target) size; if the rounded cache
 *    does not hold the target-size content yet, it is marked dirty so the
 *    natural layout is re-rendered as soon as the client commits it.
 *  - a fall/drop morph returns the FBO node to the natural layout of the
 *    window's current box, so whatever runs next (a fade-out that keeps
 *    the motion, a cancel) draws the window correctly from there. */
static void anim_stop(struct toplevel_anim *a); /* defined below */
static void morph_teardown(struct toplevel_anim *a) {
	struct toplevel *tl = a->tl;
	if (!tl->morph_active) {
		return;
	}
	struct wlr_box sweep;
	if (a->kind == ANIM_GEOM) {
		/* snap to the target box and lay the FBO back out at its natural
		 * (target) size before clearing morph_active: the last zoom tick
		 * left the node dest-sized to the mid-zoom morph box, and a ready
		 * cache is never re-published, so without this the window would
		 * stay drawn at the intermediate scale (e.g. a minimize or a
		 * close-fade starting mid-zoom) */
		wlr_scene_node_set_position(&tl->scene_tree->node,
			a->geom_to.x, a->geom_to.y);
		tl->morph_x = a->geom_to.x;
		tl->morph_y = a->geom_to.y;
		tl->morph_w = a->geom_to.width;
		tl->morph_h = a->geom_to.height;
		rounded_cache_morph_apply(tl); /* morph_active still set */
		tl->morph_active = false;
		if (!rounded_cache_size_ready(tl, a->geom_to.width,
				a->geom_to.height)) {
			rounded_cache_dirty(tl);
		}
		geom_sweep_box(a, &sweep);
	} else {
		/* fall/drop shrink: lay the FBO back out at the natural box of the
		 * window's current position, then stop scaling it */
		struct wlr_box box;
		toplevel_box(tl, &box);
		tl->morph_x = box.x;
		tl->morph_y = box.y;
		tl->morph_w = box.width;
		tl->morph_h = box.height;
		rounded_cache_morph_apply(tl);
		tl->morph_active = false;
		anim_sweep_box(a, &sweep);
	}
	anim_schedule_frames(tl->server, &sweep);
	wlr_log(WLR_DEBUG, "animate: %s cancelled for app_id \"%s\"",
		anim_kind_name(a->kind),
		tl->app_id != NULL ? tl->app_id : "?");
}

/* finish the current animation: land the exact final state (the pacing
 * watchdog disarms itself once no run is active anymore) */
static void anim_finish(struct toplevel_anim *a) {
	struct toplevel *tl = a->tl;
	enum anim_kind kind = a->kind;

	if (a->kind == ANIM_NONE) {
		/* another finisher (a frame tick and the pacing watchdog both
		 * watch the same wall-clock deadline) already landed this run:
		 * never finish - or re-send the close - twice */
		return;
	}

	wlr_log(WLR_DEBUG, "animate: %s finished for app_id \"%s\"",
		anim_kind_name(kind),
		tl->app_id != NULL ? tl->app_id : "?");
	anim_apply(a, 1.0f); /* exact endpoint (also covers p = 1 rounding) */
	switch (kind) {
	case ANIM_FADE_IN:
		rounded_window_set_opacity(tl, 1.0f);
		a->op_cur = 1.0f;
		break;
	case ANIM_FADE_OUT:
		/* fully transparent: now let the client actually go away */
		rounded_window_set_opacity(tl, 0.0f);
		a->op_cur = 0.0f;
		a->kind = ANIM_NONE;
		if (tl->xdg_toplevel != NULL && tl->xdg_toplevel->base != NULL) {
			wlr_xdg_toplevel_send_close(tl->xdg_toplevel);
		}
		if (tl->closing && tl->scene_tree != NULL) {
			/* the window is fully invisible but the client did not destroy
			 * the surface (yet).  Disable the node: wlroots' scene hit
			 * test ignores opacity, so a transparent-but-enabled node would
			 * keep blocking the pointer (and hover) underneath it while
			 * the close request stays unanswered */
			wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
		}
		return;
	case ANIM_FALL_OUT:
		/* the window is off the screen: hide it and snap it back to its
		 * resting origin so all geometry bookkeeping sees the original
		 * position of the (now invisible) window.  The shrink morph is
		 * done; the FBO node stays where the last tick left it, hidden
		 * with the whole tree until the window is restored. */
		wlr_scene_node_set_position(&tl->scene_tree->node,
			a->rest_x, a->rest_y);
		wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
		tl->morph_active = false;
		break;
	case ANIM_FALL_IN:
		/* landed: keep the resting origin.  The last tick (p = 1) already
		 * scaled the FBO back to the natural box, so only the morph state
		 * itself is cleared. */
		wlr_scene_node_set_position(&tl->scene_tree->node,
			a->rest_x, a->rest_y);
		tl->morph_active = false;
		break;
	case ANIM_GEOM:
		/* zoomed to the target box: hand the geometry back to the rounded
		 * cache (natural position/size equal what we just showed) */
		tl->morph_active = false;
		break;
	default:
		break;
	}
	a->kind = ANIM_NONE;
	struct wlr_box sweep;
	if (kind == ANIM_GEOM) {
		geom_sweep_box(a, &sweep);
	} else {
		anim_sweep_box(a, &sweep);
	}
	anim_schedule_frames(tl->server, &sweep);
}

/* advance one running animation to now_ms: interpolate and apply the eased
 * state, or land the final state when the run is over.  Called once per
 * rendered frame (anim_frame_tick) with that frame's own instant, so the
 * state the output actually shows always belongs to its own vblank. */
static void anim_advance(struct toplevel_anim *a, uint32_t now_ms) {
	struct toplevel *tl = a->tl;
	uint32_t elapsed = now_ms - a->start_ms;

	/* ANIM_GEOM wait phase: do nothing until the rounded cache holds the
	 * window content at the target size (the client committed it and the
	 * cache was re-rendered).  Only then start the actual zoom - that way
	 * the window never pops between the floating and the target layout.
	 * While waiting the picture is static, so nothing is applied here. */
	if (a->kind == ANIM_GEOM && !a->geom_zooming) {
		if (rounded_cache_size_ready(tl, a->geom_to.width,
				a->geom_to.height)) {
			a->geom_zooming = true;
			a->duration_ms = anim_geom_duration(&a->geom_from,
				&a->geom_to);
			a->start_ms = now_ms;
			wlr_log(WLR_DEBUG, "animate: maximize-zoom content ready, "
				"zooming for app_id \"%s\" (%u ms)",
				tl->app_id != NULL ? tl->app_id : "?",
				a->duration_ms);
			anim_apply(a, 0.0f);
			/* the p = 0 zoom state equals what the window already shows
			 * (its FBO scaled into the from box), so it damages nothing:
			 * request the first zoom frame explicitly */
			struct wlr_box sweep;
			geom_sweep_box(a, &sweep);
			anim_schedule_frames(tl->server, &sweep);
		} else if (elapsed >= a->duration_ms) {
			/* the client never committed the target size: drop the zoom
			 * and land on the target geometry instantly (morph_teardown
			 * schedules the frame that shows the landing) */
			morph_teardown(a);
			anim_stop(a);
		}
		return;
	}

	if (elapsed >= a->duration_ms) {
		anim_finish(a);
		return;
	}

	float t = anim_ease(a->kind,
		(float)elapsed / (float)a->duration_ms);
	anim_apply(a, t);
}

/* advance every running animation to the given CLOCK_MONOTONIC instant;
 * called by each output's frame handler right before the scene is rendered
 * (monitor_frame in output.c) */
void anim_frame_tick(struct server *server, uint32_t now_ms) {
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		struct toplevel_anim *a = tl->anim;
		if (a == NULL || a->kind == ANIM_NONE) {
			continue;
		}
		anim_advance(a, now_ms);
	}
}

/* server-wide pacing watchdog, armed only while at least one animation
 * runs.  During a visible animation every state change damages the scene,
 * which keeps the output rendering each vblank by itself; the watchdog
 * only fills the gaps damage cannot cover: a window that moved fully
 * outside its output (fall-out tail, drop-in head) or a state that stalls
 * at a rounded value damages nothing and would stop the frame flow.  It
 * also enforces the wall-clock timeouts (a maximize zoom waiting for a
 * client that never commits) when no frame ever renders. */
static int anim_watchdog(void *data) {
	struct server *server = data;
	bool active = false;
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		struct toplevel_anim *a = tl->anim;
		if (a == NULL || a->kind == ANIM_NONE) {
			continue;
		}
		active = true;
		if (a->kind == ANIM_GEOM && !a->geom_zooming) {
			/* pure wait: the scene is static, so no frame requests here;
			 * the readiness check runs in the frame ticks driven by the
			 * client's commits.  Only the timeout needs wall clock. */
			if (!rounded_cache_size_ready(tl, a->geom_to.width,
					a->geom_to.height) &&
					mono_ms() - a->start_ms >= a->duration_ms) {
				morph_teardown(a);
				anim_stop(a);
			}
			continue;
		}
		if (mono_ms() - a->start_ms >= a->duration_ms) {
			/* wall-clock fallback: the run is over even though no frame
			 * advanced it (e.g. its output went away mid-animation) */
			anim_finish(a);
			continue;
		}
		/* keep a frame flowing on the window's output (a disabled output
		 * would ignore the request anyway) */
		struct wlr_output *output = toplevel_output(tl->server, tl);
		if (output != NULL && output->enabled) {
			wlr_output_schedule_frame(output);
		}
	}
	if (active) {
		wl_event_source_timer_update(server->anim_timer,
			ANIM_WATCHDOG_MS);
	} else {
		wl_event_source_timer_update(server->anim_timer, 0); /* idle */
	}
	return 0;
}

/* arm the pacing watchdog (created lazily on first use); called whenever
 * a run starts.  If the allocation fails the visible animations still
 * advance through their own scene damage - only the offscreen phases and
 * the timeouts lose their safety net. */
static void anim_watchdog_arm(struct server *server) {
	if (server->anim_timer == NULL) {
		server->anim_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server->display),
			anim_watchdog, server);
	}
	if (server->anim_timer == NULL) {
		return;
	}
	wl_event_source_timer_update(server->anim_timer, ANIM_WATCHDOG_MS);
}

/* the window's scene tree is gone (the xdg surface was destroyed): stop the
 * animation and drop the per-window state.  Runs while the toplevel is
 * still alive - wlroots destroys the scene tree before the compositor's own
 * xdg-surface destroy handler frees it (see toplevel.c). */
static void anim_tree_destroy(struct wl_listener *listener, void *data) {
	struct toplevel_anim *a = wl_container_of(listener, a, tree_destroy);
	(void)data;
	a->tl->anim = NULL;
	wl_list_remove(&a->tree_destroy.link);
	free(a);
	/* the watchdog scans the toplevel list itself, so it disarms once no
	 * run is left; no explicit timer teardown is needed */
}

static struct toplevel_anim *anim_get(struct toplevel *tl) {
	if (tl->anim != NULL) {
		return tl->anim;
	}
	if (tl->scene_tree == NULL) {
		return NULL;
	}
	struct toplevel_anim *a = calloc(1, sizeof(*a));
	if (a == NULL) {
		return NULL;
	}
	a->tl = tl;
	a->kind = ANIM_NONE;
	a->tree_destroy.notify = anim_tree_destroy;
	wl_signal_add(&tl->scene_tree->node.events.destroy, &a->tree_destroy);
	tl->anim = a;
	return a;
}

/* stop the running animation without touching the scene state (the pacing
 * watchdog notices that no run is active and disarms itself) */
static void anim_stop(struct toplevel_anim *a) {
	a->kind = ANIM_NONE;
}

/* begin a run of `kind` lasting `duration_ms`; the initial state must be
 * applied by the caller right after (start position / start opacity) */
static void anim_begin(struct toplevel_anim *a, enum anim_kind kind,
		uint32_t duration_ms) {
	/* another animation owned the window; if that was the maximize/restore
	 * zoom, stop scaling and snap the window back to its target first */
	morph_teardown(a);
	anim_stop(a);
	a->kind = kind;
	a->duration_ms = duration_ms;
	a->start_ms = mono_ms();
	wlr_log(WLR_DEBUG, "animate: %s started for app_id \"%s\" (%u ms)",
		anim_kind_name(kind),
		a->tl->app_id != NULL ? a->tl->app_id : "?", duration_ms);
	anim_watchdog_arm(a->tl->server);
}

bool animate_toplevel_minimize(struct server *server, struct toplevel *tl) {
	if (!CONFIG_ANIM_ENABLE) {
		return false;
	}
	struct toplevel_anim *a = anim_get(tl);
	if (a == NULL) {
		return false;
	}
	if (tl->scene_tree == NULL ||
			!tl->scene_tree->node.enabled) {
		/* already hidden: nothing left to animate */
		return true;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);
	if (box.width <= 0 || box.height <= 0) {
		return false; /* no usable geometry: caller hides instantly */
	}
	struct wlr_output *output = toplevel_output(server, tl);
	if (output == NULL) {
		return false;
	}
	struct wlr_box obox;
	wlr_output_layout_get_box(server->output_layout, output, &obox);

	/* fall until the window is fully clear of the output's bottom edge:
	 * its FBO top edge sits shadow_padding() above the content top, so a
	 * content top at bottom + shadow (plus one pixel of safety) leaves no
	 * border/shadow sliver visible.  The drop ends exactly when the
	 * window leaves the screen - no invisible tail travel below it. */
	int fall_to = obox.y + obox.height + shadow_padding() + 1;
	if (fall_to <= box.y) {
		/* already below the bottom: hide right away */
		wlr_scene_node_set_position(&tl->scene_tree->node, box.x, box.y);
		wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
		return true;
	}

	/* remember the resting origin.  A minimize during an in-flight drop-in
	 * must keep that run's target: the restore consumed has_rest, and box
	 * is the mid-air position of the falling window, not where it belongs */
	if (a->kind == ANIM_FALL_IN) {
		a->has_rest = true;
		a->rest_x = a->to_x;
		a->rest_y = a->to_y;
	} else {
		a->has_rest = true;
		a->rest_x = box.x;
		a->rest_y = box.y;
	}

	anim_begin(a, ANIM_FALL_OUT, CONFIG_ANIM_FALL_MS);
	a->from_x = box.x;
	a->from_y = box.y;
	a->to_x = box.x;
	a->to_y = fall_to;
	a->win_w = box.width;
	a->win_h = box.height;

	/* shrink the window around its own (moving) center as it drops - the
	 * rounded FBO is scaled 1.0 -> CONFIG_ANIM_FALL_SCALE by the time the
	 * window leaves the screen.  Without a scalable FBO it stays a plain
	 * drop. */
	a->scale_fall = CONFIG_ANIM_FALL_SCALE < 1.0f &&
		rounded_morph_supported(tl);
	if (a->scale_fall) {
		a->scale_from = 1.0f;
		tl->morph_active = true;
		tl->morph_x = box.x;
		tl->morph_y = box.y;
		tl->morph_w = box.width;
		tl->morph_h = box.height;
	} else {
		tl->morph_active = false;
	}

	/* full opacity, node enabled, then let it fall.  Focus already moved to
	 * the next window and raised it above this one (see set_minimized), so
	 * put the falling window back on top of the toplevel layer: it must
	 * stay at its own place while dropping instead of sliding down behind
	 * the window that took focus. */
	rounded_window_set_opacity(tl, 1.0f);
	wlr_scene_node_set_enabled(&tl->scene_tree->node, true);
	wlr_scene_node_raise_to_top(&tl->scene_tree->node);
	wlr_scene_node_set_position(&tl->scene_tree->node, box.x, box.y);
	struct wlr_box sweep;
	anim_sweep_box(a, &sweep);
	anim_schedule_frames(server, &sweep);
	return true;
}

bool animate_toplevel_restore(struct server *server, struct toplevel *tl) {
	if (!CONFIG_ANIM_ENABLE) {
		return false;
	}
	struct toplevel_anim *a = anim_get(tl);
	if (a == NULL) {
		return false;
	}
	if (tl->scene_tree == NULL) {
		return true;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);
	if (box.width <= 0 || box.height <= 0) {
		return false; /* no usable geometry: caller shows instantly */
	}

	/* target: the resting origin captured when the window was minimized;
	 * a window that was never animated keeps its current position */
	int rest_x = a->has_rest ? a->rest_x : box.x;
	int rest_y = a->has_rest ? a->rest_y : box.y;
	a->has_rest = false;

	/* drop back in: start above the window's own top edge by roughly its
	 * own height (+ the gap) and fall straight down into its slot.  A
	 * restore that interrupts a minimize drop still in flight (the window
	 * is visibly falling out) instead reverses smoothly from wherever the
	 * window currently is - restoring from the taskbar mid-drop must not
	 * teleport the window back above its slot. */
	int from_y;
	if (a->kind == ANIM_FALL_OUT) {
		from_y = tl->scene_tree != NULL ?
			tl->scene_tree->node.y : rest_y;
	} else {
		int drop = box.height + CONFIG_ANIM_FALL_GAP;
		from_y = rest_y - drop;
		if (from_y >= rest_y) {
			from_y = rest_y - 1;
		}
	}

	anim_begin(a, ANIM_FALL_IN, CONFIG_ANIM_FALL_MS);
	a->from_x = rest_x;
	a->from_y = from_y;
	a->to_x = rest_x;
	a->to_y = rest_y;
	a->win_w = box.width;
	a->win_h = box.height;

	/* grow the window back to full size around its own (moving) center
	 * while it drops in: CONFIG_ANIM_FALL_SCALE -> 1.0 (rounded FBO
	 * only).  Without a scalable FBO it stays a plain drop-in. */
	a->scale_fall = CONFIG_ANIM_FALL_SCALE < 1.0f &&
		rounded_morph_supported(tl);
	if (a->scale_fall) {
		a->scale_from = CONFIG_ANIM_FALL_SCALE;
		int mw = (int)lroundf(box.width * CONFIG_ANIM_FALL_SCALE);
		int mh = (int)lroundf(box.height * CONFIG_ANIM_FALL_SCALE);
		if (mw < 1) {
			mw = 1;
		}
		if (mh < 1) {
			mh = 1;
		}
		tl->morph_active = true;
		tl->morph_x = rest_x + (box.width - mw) / 2;
		tl->morph_y = from_y + (box.height - mh) / 2;
		tl->morph_w = mw;
		tl->morph_h = mh;
		rounded_cache_morph_apply(tl);
	} else {
		tl->morph_active = false;
	}

	/* visible from the very first frame, already at the top of the drop.
	 * Raise it above the other windows so the drop-in is seen at its own
	 * place (a restored window is normally (re)focused anyway, which
	 * raises it); without this it would reappear from behind the window
	 * that currently has focus. */
	rounded_window_set_opacity(tl, 1.0f);
	wlr_scene_node_set_enabled(&tl->scene_tree->node, true);
	wlr_scene_node_raise_to_top(&tl->scene_tree->node);
	wlr_scene_node_set_position(&tl->scene_tree->node, rest_x, from_y);
	struct wlr_box sweep;
	anim_sweep_box(a, &sweep);
	anim_schedule_frames(server, &sweep);
	return true;
}

bool animate_toplevel_fade_in(struct server *server, struct toplevel *tl) {
	if (!CONFIG_ANIM_ENABLE) {
		return false;
	}
	struct toplevel_anim *a = anim_get(tl);
	if (a == NULL) {
		return false;
	}
	if (tl->scene_tree == NULL || tl->minimized) {
		/* hidden window: nothing to fade in */
		return true;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);

	anim_begin(a, ANIM_FADE_IN, CONFIG_ANIM_FADE_MS);
	a->from_x = a->to_x = box.x;
	a->from_y = a->to_y = box.y;
	a->win_w = box.width;
	a->win_h = box.height;
	a->op_from = 0.0f;
	a->op_to = 1.0f;
	a->op_cur = 0.0f; /* start transparent: a close that interrupts the
	                   * fade-in knows where the window actually is */

	/* start transparent right away so the first frame is not a flash;
	 * the window keeps its natural size, only the opacity fades in */
	rounded_window_set_opacity(tl, 0.0f);
	struct wlr_box sweep;
	anim_sweep_box(a, &sweep);
	anim_schedule_frames(server, &sweep);
	return true;
}

/* Windows-style maximize/restore zoom.  The caller already sent the target
 * size to the client and must not move the scene node itself.  The window
 * keeps showing its current (floating / maximized) content in the `from`
 * box until the rounded cache holds the content at the target size, then
 * it is scaled smoothly between the two boxes - maximize zooms up from the
 * floating rect, restore zooms back down.  The node ends at `to`'s origin. */
bool animate_toplevel_geometry(struct server *server, struct toplevel *tl,
		const struct wlr_box *from, const struct wlr_box *to) {
	if (!CONFIG_ANIM_ENABLE || CONFIG_ANIM_MAXIMIZE_MS <= 0) {
		return false;
	}
	if (tl->skip_geom) {
		/* the window just mapped and reports its initial state (e.g. an
		 * app that starts maximized): no user maximize action to animate */
		tl->skip_geom = false;
		return false;
	}
	if (tl->scene_tree == NULL || !tl->scene_tree->node.enabled ||
			tl->minimized) {
		return false;
	}
	if (from->width <= 0 || from->height <= 0 ||
			to->width <= 0 || to->height <= 0) {
		return false;
	}
	if (from->x == to->x && from->y == to->y &&
			from->width == to->width && from->height == to->height) {
		return false; /* no visible change to animate */
	}
	struct toplevel_anim *a = anim_get(tl);
	if (a == NULL) {
		return false;
	}
	if (a->kind != ANIM_NONE) {
		return false; /* another animation (drop/fade) is running */
	}
	if (!rounded_morph_supported(tl)) {
		return false; /* no scalable rounded FBO to zoom */
	}

	anim_begin(a, ANIM_GEOM, CONFIG_ANIM_MAXIMIZE_WAIT_MS);
	a->geom_from = *from;
	a->geom_to = *to;
	a->geom_zooming = false;

	/* stay visually where we are until the target-size content is ready:
	 * the FBO is published scaled into the current (from) box, which is
	 * exactly where the window already sits */
	tl->morph_active = true;
	tl->morph_x = from->x;
	tl->morph_y = from->y;
	tl->morph_w = from->width;
	tl->morph_h = from->height;
	wlr_scene_node_set_position(&tl->scene_tree->node, from->x, from->y);
	rounded_cache_morph_apply(tl);
	struct wlr_box sweep;
	geom_sweep_box(a, &sweep);
	anim_schedule_frames(server, &sweep);
	return true;
}

/* stop an in-flight maximize/restore zoom and land the window on its zoom
 * target (used by toplevel.c when a new maximize/restore request replaces
 * the running one) */
void animate_toplevel_abort_geometry(struct toplevel *tl) {
	if (tl->anim == NULL) {
		return;
	}
	morph_teardown(tl->anim);
	anim_stop(tl->anim);
}

bool animate_toplevel_close(struct toplevel *tl) {
	if (!CONFIG_ANIM_ENABLE) {
		return false;
	}
	if (tl->xdg_toplevel == NULL || tl->xdg_toplevel->base == NULL) {
		return false;
	}
	struct toplevel_anim *a = anim_get(tl);
	if (a == NULL) {
		return false;
	}
	if (tl->scene_tree == NULL ||
			!tl->scene_tree->node.enabled) {
		return false; /* hidden (minimized) window: no fade to show */
	}
	if (a->kind == ANIM_FADE_OUT) {
		return true; /* a fade-out close is already running */
	}

	bool interrupting_fall = a->kind == ANIM_FALL_OUT;
	bool interrupting_fade_in = a->kind == ANIM_FADE_IN;
	/* the minimize drop this close interrupts keeps falling: capture its
	 * off-screen target so the fade-out continues the motion (anim_apply)
	 * instead of freezing the window in mid-air */
	int fall_target_y = interrupting_fall ? a->to_y : 0;

	struct wlr_box box;
	toplevel_box(tl, &box);

	anim_begin(a, ANIM_FADE_OUT, CONFIG_ANIM_FADE_MS);
	a->from_x = a->to_x = box.x;
	a->from_y = box.y;
	a->to_y = interrupting_fall ? fall_target_y : box.y;
	a->win_w = box.width;
	a->win_h = box.height;
	/* continue from the opacity the previous run left the window at: a
	 * close right after map must not flash the window back to 100% first */
	a->op_from = interrupting_fade_in ? a->op_cur : 1.0f;
	a->op_to = 0.0f;
	a->op_cur = a->op_from;

	/* start fully opaque (or at the interrupted fade-in's level); the
	 * window keeps its natural size, only the opacity fades out, then the
	 * xdg close is sent at the end */
	rounded_window_set_opacity(tl, a->op_from);
	struct wlr_box sweep;
	anim_sweep_box(a, &sweep);
	anim_schedule_frames(tl->server, &sweep);
	return true;
}

/* the window unmapped before its animation finished (client hid/closed it
 * itself): stop the animation and leave the window in a clean state */
void animate_toplevel_cancel(struct toplevel *tl) {
	if (tl->anim == NULL) {
		return;
	}
	struct toplevel_anim *a = tl->anim;
	switch (a->kind) {
	case ANIM_FALL_OUT:
		/* finish the minimize: hide at the resting origin */
		wlr_scene_node_set_position(&tl->scene_tree->node,
			a->rest_x, a->rest_y);
		wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
		break;
	case ANIM_FALL_IN:
		/* finish the restore: land at the resting origin */
		wlr_scene_node_set_position(&tl->scene_tree->node,
			a->rest_x, a->rest_y);
		break;
	case ANIM_FADE_OUT:
	case ANIM_FADE_IN:
	case ANIM_GEOM:
	default:
		break;
	}
	/* a running maximize/restore zoom is torn down to its target box */
	morph_teardown(a);
	wlr_log(WLR_DEBUG, "animate: %s cancelled for app_id \"%s\"",
		anim_kind_name(a->kind),
		tl->app_id != NULL ? tl->app_id : "?");
	rounded_window_set_opacity(tl, 1.0f);
	anim_stop(a);
}
