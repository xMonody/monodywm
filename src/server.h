/*
 * server.h - shared types and cross-module declarations for xmonodywm
 *
 * The compositor is split into small modules (main, ipc, scene, border,
 * toplevel, layer, output, input, pointer); everything they need to share
 * lives here.
 */

#ifndef XMONODYWM_SERVER_H
#define XMONODYWM_SERVER_H

#define _POSIX_C_SOURCE 200809L

/* all tunables (shortcuts, edge grab zone) */
#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

/* ordering of scene-graph trees (bottom to top) */
enum scene_layer {
	LAYER_BACKGROUND = 0, /* wlr-layer-shell background   */
	LAYER_BOTTOM,         /* wlr-layer-shell bottom       */
	LAYER_TOPLEVELS,      /* normal windows               */
	LAYER_TOP,            /* wlr-layer-shell top          */
	LAYER_OVERLAY,        /* wlr-layer-shell overlay + drag icon */
	LAYER_COUNT,
};

/* tag stored in wlr_scene_node.data so we can find the owning object from
 * an arbitrary hit-tested scene node (walking up the parents). */
enum scene_tag_type {
	TAG_TOPLEVEL,
	TAG_POPUP,
	TAG_LAYER,
};

struct scene_tag {
	enum scene_tag_type type;
	void *ptr;

	struct wl_listener destroy; /* frees the tag when the node is destroyed */
};

#include <wlr/types/wlr_scene.h>

struct ipc_client;     /* defined in ipc.c */
struct wlr_swapchain;  /* defined in wlr/render/swapchain.h */
struct server;
struct toplevel;
struct toplevel_anim;  /* defined in animate.c */
struct layer_surface;
struct rounded_cache;  /* defined in rounded.c */

/* one connected input method (fcitx5/ibus); only the first one is used */
struct ime {
	struct server *server;
	struct wl_list link; /* server.imes */

	struct wlr_input_method_v2 *input_method;
	struct wl_listener destroy;
	struct wl_listener grab_keyboard;
	struct wl_listener commit;
	struct wl_listener keyboard_grab_destroy;
	struct wl_listener new_popup_surface;

	/* seat keyboard while the IM holds the keyboard grab.  Keys are
	 * forwarded by input.c via ime_forward_key() so compositor shortcuts
	 * are checked first; only modifiers are synced through a listener. */
	struct wlr_keyboard *keyboard;
	struct wl_listener keyboard_grab_modifiers;
	bool keyboard_grab_destroy_added; /* grab destroy listener attached */

	/* keycodes forwarded to the IM grab while pressed; used to suppress
	 * stray key-release events (releases without a matching forwarded
	 * press are not sent to the IM) */
	uint32_t forwarded_keys[16];
	size_t forwarded_key_count;

	/* candidate window shown by the IM (fcitx5) */
	struct wlr_input_popup_surface_v2 *popup_surface;
	struct wlr_scene_surface *popup_scene_surface;
	struct wl_listener popup_commit;
	struct wl_listener popup_destroy;
};

/* one zwp_text_input_v3 object of a client (usually one per client) */
struct text_input {
	struct server *server;
	struct wl_list link; /* server.text_inputs */

	struct wlr_text_input_v3 *text_input;
	/* compositor-side cache of the surface this text input is focused on.
	 * wlr_text_input_v3_send_enter()/send_leave() maintain
	 * text_input->focused_surface inside wlroots; we keep our own copy so
	 * focus transitions can be decided without relying on wlroots' field
	 * during surface destruction. */
	struct wlr_surface *focused_surface;
	struct wl_listener destroy;
	struct wl_listener enable;
	struct wl_listener disable;
	struct wl_listener commit;
};

/* an xdg popup (menu / tooltip / combo box) under a toplevel's content
 * tree; rendered with wlr_scene_xdg_surface_create so subsurfaces are
 * handled and nested popups (Qt submenus) attach recursively.  The struct's
 * lifetime is tied to pp->tree: wlroots destroys the tree when the popup's
 * xdg surface is destroyed (or with the toplevel's tree), and
 * tree_destroy then frees pp - so popups never outlive the toplevel. */
struct toplevel_popup {
	struct toplevel *tl;
	struct wlr_xdg_popup *popup;
	struct wlr_scene_tree *tree;
	struct wl_listener tree_destroy;  /* frees pp when the tree is destroyed */
	struct wl_listener commit;
	struct wl_listener new_popup;
	struct wl_listener destroy;
};

/* a subsurface of a toplevel's surface (Firefox/Chromium cover the window
 * edges with subsurfaces); its commit marks the rounded FBO cache dirty so
 * the offscreen copy is re-rendered when any content changes */
struct toplevel_subsurface {
	struct toplevel *tl;
	struct wlr_subsurface *subsurface;
	struct wl_list link; /* tl->subsurfaces */
	struct wl_listener commit;
	struct wl_listener new_subsurface; /* nested subsurfaces */
	struct wl_listener destroy;

	/* buffer damage accumulated since the last FBO render, in
	 * surface-local coordinates (rounded.c maps it into FBO space at
	 * render time, when the final layout position is known) */
	pixman_region32_t damage;
	/* geometry at the last commit (position relative to the parent
	 * surface + surface-local size), so moves/resizes can damage the
	 * old area too */
	int prev_x, prev_y, prev_w, prev_h;
};

struct toplevel {
	struct server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	struct wlr_foreign_toplevel_handle_v1 *fthandle;
	struct wlr_output *last_output;

	/* offscreen rounded-corner FBO cache (rounded.c); NULL when disabled */
	struct rounded_cache *rounded;

	/* subsurfaces of the window surface (their commits mark the rounded
	 * cache dirty); cleaned up together with the toplevel */
	struct wl_list subsurfaces;      /* struct toplevel_subsurface.link */

	/* popups are scene-tree children of scene_tree and clean themselves up
	 * when their tree is destroyed, so no explicit popup list is needed */

	/* xdg-decoration */
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	enum wlr_xdg_toplevel_decoration_v1_mode decoration_mode;
	bool decoration_configured;

	bool minimized;
	/* a close was requested and the close-fade (animate.c) owns the
	 * wlr_xdg_toplevel close request, which it sends once the window is
	 * invisible.  The window is inert until it actually goes away: not
	 * focusable, not raisable, excluded from the window cycle, and
	 * minimize/maximize are refused.  When the fade finishes, its scene
	 * node is disabled, so a client that never honors the close request
	 * cannot leave an invisible, input-blocking window behind.  Cleared
	 * only when the surface is destroyed (a remap while closing is
	 * re-closed, never re-shown). */
	bool closing;
	bool positioned; /* initial position has been assigned */

	/* per-window animation state (animate.c): map fade-in, minimize /
	 * restore vertical drop, close fade-out, maximize/restore zoom.
	 * NULL while the window has never been animated; the state is freed
	 * when the window's scene tree is destroyed. */
	struct toplevel_anim *anim;

	/* Windows-style maximize/restore zoom (animate.c).  While morph_active
	 * is set, the rounded FBO is published scaled into the morph box
	 * instead of its natural geometry (rounded.c) and toplevel.c anchors
	 * the scene tree at the morph box origin.  Cleared when the zoom ends
	 * or is cancelled. */
	bool morph_active;
	int morph_x, morph_y, morph_w, morph_h;

	/* set while the map handler applies the client's initial state (an
	 * app that starts maximized): animate_toplevel_geometry reads and
	 * clears it so the first maximize is applied instantly, only later
	 * user maximize/restore actions get the zoom */
	bool skip_geom;

	/* auto-centering (place.c): a fresh window is re-centered whenever its
	 * surface size changes, until the user interacts with it (move /
	 * resize / maximize / fullscreen set user_moved and stop it).  Electron
	 * windows (QQ) often map with a small placeholder surface and only
	 * commit the real size on a later frame. */
	bool user_moved;
	bool placed;   /* centered placement has run; placed_w/h = size used */
	int placed_w, placed_h;

	/* geometry to restore when un-maximizing by dragging (Windows style) */
	struct wlr_box restore_box;
	bool has_restore_box;

	/* geometry to restore when leaving fullscreen; kept separate from
	 * restore_box so entering fullscreen from a maximized window does not
	 * clobber the floating geometry saved by maximize */
	struct wlr_box fullscreen_restore_box;
	bool has_fullscreen_restore_box;

	/* fullscreen state (tracks current.fullscreen which only updates on ack) */
	bool fullscreen;

	/* id exposed to status bars over the IPC socket */
	int id;
	bool ipc_added; /* window_added was emitted */
	char *app_id;   /* cached app_id (survives teardown for window_removed) */
	pid_t pid;      /* client process id (lets the bar match tray items) */
	int after_id;   /* id of the window this one was launched from (0 = none); focus returns there on close; prefers a same-process sibling window over a terminal */

	struct wl_list link; /* server.toplevels */

	struct wl_listener destroy;
	struct wl_listener toplevel_destroy;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener request_maximize;
	struct wl_listener request_minimize;
	struct wl_listener request_fullscreen;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
	struct wl_listener new_popup;
	struct wl_listener new_subsurface;

	struct wl_listener ft_request_maximize;
	struct wl_listener ft_request_minimize;
	struct wl_listener ft_request_activate;
	struct wl_listener ft_request_close;
	struct wl_listener ft_destroy;

	struct wl_listener deco_request_mode;
	struct wl_listener deco_destroy;
};

struct layer_surface {
	struct server *server;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene_layer;

	struct wl_list link; /* server.layer_surfaces */

	/* last exclusive-zone signature: when it changes (a bar appears,
	 * resizes or goes away) the work area changes and existing windows
	 * must be re-arranged out of the exclusive zone */
	uint32_t last_anchor;
	int32_t last_zone;
	int32_t last_margin_top, last_margin_bottom;
	int32_t last_margin_left, last_margin_right;
	bool last_mapped;
	bool has_last_state;

	/* keyboard focus: a layer surface that asked for keyboard interactivity
	 * (rofi / wofi launcher overlay, ...) holds the seat's keyboard as long
	 * as it stays mapped.  last_keyboard_interactive remembers the previous
	 * commit's interactivity so focus is only grabbed on the map /
	 * interactivity transition, never re-stolen on later commits (a bar
	 * that re-renders every second must not yank the keyboard). */
	enum zwlr_layer_surface_v1_keyboard_interactivity last_keyboard_interactive;
	bool keyboard_focused;

	struct wl_listener destroy;
	struct wl_listener commit;
};

/* double-click action armed by a press on the top title strip: the action
 * fires when the (second) click is released without a drag */
enum zone_action {
	ZONE_NONE = 0,     /* no armed action */
	ZONE_MINIMIZE,     /* left third of the strip: minimize */
	ZONE_MAXIMIZE,     /* middle third: toggle maximize / restore */
	ZONE_CLOSE,        /* right third: close the window */
};

/* pointer interaction mode, mirroring labwc's input_mode state machine:
 * PASSTHROUGH = events and cursor go to the focused client; MOVE / RESIZE
 * = the compositor consumes pointer events and owns the cursor. */
enum input_mode {
	INPUT_MODE_PASSTHROUGH = 0,
	INPUT_MODE_MOVE,
	INPUT_MODE_RESIZE,
};

/* button presses the compositor swallowed (so their releases are swallowed
 * too and never reach the client); mirrors labwc's bound_buttons set. */
#define BOUND_BUTTONS_MAX 8
struct bound_buttons {
	uint32_t values[BOUND_BUTTONS_MAX];
	int size;
};

struct server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_scene *scene;
	struct wlr_scene_tree *layers[LAYER_COUNT];
	struct wlr_output_layout *output_layout;
	struct wlr_output_manager_v1 *output_manager;
	struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;

	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor_manager;
	struct wl_list keyboards; /* struct keyboard.link (per attached device) */

	/* xdg-activation-v1: client-driven focus/activation requests (focus
	 * stealing prevention); the request_activate signal is handled by
	 * toplevel.c.  wp_fractional_scale_v1 is the matching global for
	 * fractional surface scaling. */
	struct wlr_xdg_activation_v1 *activation;
	struct wl_listener activation_request_activate;
	struct wlr_fractional_scale_manager_v1 *fractional_scale_manager;

	/* linux-drm-syncobj-v1: explicit buffer synchronization.  The manager
	 * is created only when both the renderer and the backend advertise
	 * timeline support; the DRM fd comes from the renderer, so clients
	 * import timelines on the same device the compositor renders with. */
	struct wlr_linux_drm_syncobj_manager_v1 *linux_drm_syncobj_manager;

	/* cursor-shape-v1: lets clients pick a cursor shape which the compositor
	 * renders with its own theme (so the size always matches the output
	 * scale, no client-side guessing); handled like wl_pointer.set_cursor */
	struct wlr_cursor_shape_manager_v1 *cursor_shape_manager;
	struct wl_listener cursor_shape_set_shape;

	/* ext-data-control-v1: privileged selection/clipboard control for tools
	 * like wl-clipboard (wl-copy/wl-paste) and clipboard managers, which is
	 * how terminal editors such as vim reach the Wayland clipboard. */
	struct wlr_ext_data_control_manager_v1 *ext_data_control_manager;

	/* wlr-data-control-unstable-v1: legacy clipboard-control protocol still
	 * bound by CopyQ and older clipboard tools instead of the ext- one. */
	struct wlr_data_control_manager_v1 *data_control_manager;


	struct wl_list toplevels;      /* struct toplevel.link */
	struct wl_list layer_surfaces; /* struct layer_surface.link */
	struct toplevel *focused;
	struct layer_surface *layer_focused; /* layer surface currently holding the
					       seat's keyboard (rofi overlay, ...);
					       NULL while a toplevel has the keyboard */

	/* input method relay (fcitx5 / ibus) */
	struct wl_list imes;            /* struct ime.link */
	struct wl_list text_inputs;     /* struct text_input.link */
	struct wlr_input_method_v2 *input_method;     /* active input method */
	struct wlr_text_input_v3 *focused_text_input; /* enabled text input of the
							 * focused surface, i.e. the one the IM
							 * talks to (NULL when none) */
	struct wlr_surface *ime_focused_surface; /* surface owning text input */
	struct wl_listener ime_focused_surface_destroy;
	/* IPC socket for status bars (JSON events) */
	int ipc_fd;
	struct wl_event_source *ipc_source;
	struct wl_list ipc_clients; /* ipc_client.link */
	uint32_t next_window_id;

	struct wlr_scene_tree *drag_tree;

	/* pointer interaction state */
	struct toplevel *zone_toplevel; /* toplevel under an active zone press */
	bool zone_press;                /* press in the title strip zone, swallowed */
	bool right_button_held;         /* right mouse button currently pressed */
	bool left_button_held;          /* left mouse button currently pressed */
	/* chord gestures: hold one button, then press the other (double-click
	 * the other = toggle maximize/restore, hold the other = move the window
	 * under the cursor; releasing restores the cursor style) */
	bool chord_active;               /* a chord gesture is in progress */
	uint32_t chord_button;           /* the button pressed second (trigger) */
	bool chord_pending;              /* first trigger press: disambiguating
	                                    double-click vs hold */
	bool chord_moving;               /* the hold turned into a window move */
	struct toplevel *chord_toplevel; /* window the chord acts on */
	bool chord_swallow_left;         /* left press was consumed by a chord:
	                                    swallow its release too */
	bool chord_swallow_right;        /* same for right */
	struct wl_event_source *chord_timer; /* double-click vs hold timer */
	bool moving;                    /* a window move is in progress */
	struct toplevel *move_toplevel;
	enum input_mode input_mode;      /* PASSTHROUGH / MOVE / RESIZE */
	struct bound_buttons bound_buttons; /* presses swallowed by the compositor */
	double grab_x, grab_y; /* cursor offset from the window origin */
	/* the cursor position when the move started: used to re-anchor the
	 * grab after a maximized window is restored mid-drag (the client's
	 * own title bar sends xdg_toplevel.move on a plain click, so the
	 * restore is deferred until the user actually drags) */
	double move_ref_x, move_ref_y;
	double move_max_w, move_max_h; /* maximized size at move start: the press offset is mapped proportionally into the restored box */
	bool move_deferred_restore;    /* xdg_toplevel.move from a maximized window: restore + proportional re-anchor on first motion (zone/chord drags already re-anchor before begin_move) */
	double press_x, press_y;
	bool dragged;       /* moved beyond CONFIG_DRAG_THRESHOLD during a press */
	enum zone_action zone_action; /* armed double-click action on the title
	                                strip, executed on release; ZONE_NONE when
	                                no double-click is in progress */
	struct wl_event_source *zone_timer; /* holding the title strip this long
	                                       grabs the window for moving */
	uint32_t zone_button;         /* the button held in the title strip zone */
	struct timespec last_release_time;
	struct timespec wheel_burst_start; /* first tick of the current wheel
	                                      burst: ticks inside the burst
	                                      window count as one action */
	struct timespec wheel_last_tick;   /* time of the previous wheel tick:
	                                      two ticks CONFIG_WHEEL_TICK_GAP_NS
	                                      apart start a new action */
	bool last_was_click;
	uint32_t last_click_button;

	/* edge resize state */
	bool resizing;
	struct toplevel *resize_toplevel;
	uint32_t resize_edges;     /* enum wlr_edges */
	struct wlr_box resize_orig; /* window box at grab start */
	int resize_last_w, resize_last_h; /* size last sent to the client */

	/* resize outline (CONFIG_RESIZE_DRAW_CONTENTS=0): a target box drawn
	 * over the scene while dragging instead of live-resizing the client, so
	 * the dragged edge tracks the cursor 1:1 like a window move */
	struct wlr_scene_tree *resize_outline;
	struct wlr_scene_rect *resize_outline_edges[4];
	struct wlr_box resize_target; /* target box in outline mode */
	bool resize_final_pending;    /* outline mode: waiting for the final commit */
	struct wl_event_source *resize_final_timer; /* outline mode: watchdog if the client never commits */

	/* animate.c: server-wide pacing watchdog, armed while any window
	 * animation runs (the animation state itself is advanced per rendered
	 * frame in output.c's frame handler, vsync-locked - this timer only
	 * keeps frames flowing where scene damage cannot and enforces
	 * wall-clock timeouts).  Lazily created, disarmed when idle. */
	struct wl_event_source *anim_timer;

	/* current compositor-driven cursor name, NULL when the client's cursor
	 * is shown (used to avoid redundant updates) */
	const char *cursor_override;

	/* last cursor image the focused client set (wl_pointer.set_cursor or
	 * wp_cursor_shape_device_v1.set_shape); restored by pointer.c when the
	 * compositor cursor override ends so the cursor doesn't stay stuck on
	 * resize/move.  client_cursor_shape is an xcursor-theme-independent
	 * shape enum (0 = none); the shape is rendered by the compositor, the
	 * surface by the client.  When both are set (mixed-protocol clients)
	 * the shape wins: it is always rendered at the correct scale. */
	struct wlr_surface *client_cursor_surface;
	int client_cursor_hotspot_x, client_cursor_hotspot_y;
	enum wp_cursor_shape_device_v1_shape client_cursor_shape;
	/* the seat client that owns client_cursor_shape (0 = none): the shape is
	 * only restored while that client is focused, so a stale shape from a
	 * previous client is never shown */
	struct wlr_seat_client *client_cursor_shape_client;
	struct wl_listener client_cursor_shape_client_destroy;
	struct wl_listener client_cursor_destroy;

	struct wl_listener new_output;
	struct wl_listener new_input;
	struct wl_listener new_virtual_pointer;
	struct wl_listener new_virtual_keyboard;
	struct wl_listener layout_change;
	struct wl_listener output_manager_apply;
	struct wl_listener output_manager_test;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_layer_surface;
	struct wl_listener new_decoration;
	struct wl_listener new_ime;
	struct wl_listener new_text_input;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener seat_request_set_cursor;
	struct wl_listener seat_request_set_selection;
	struct wl_listener seat_request_set_primary_selection;
	struct wl_listener seat_request_start_drag;
	struct wl_listener seat_start_drag;
};

/* ---- main.c ---- */
void spawn_command(const char *cmd);

/* ---- scene.c: scene-graph tagging / hit-testing ---- */
void xdg_surface_tag(struct wlr_scene_tree *tree, enum scene_tag_type type,
	void *ptr);
void *scene_tag_at(struct server *server, enum scene_tag_type type,
	double lx, double ly);
/* the toplevel *drawn* under (lx, ly): while a window morphs (animate.c)
 * its visible content is the rounded FBO scaled into the morph box, which
 * the scene's raw hit test cannot see, so a cursor over the morph box
 * resolves to the morphing window even where its raw client surface has
 * not grown into it yet (restore/shrink zoom); see scene.c */
struct toplevel *toplevel_morph_at(struct server *server, double lx,
	double ly);
struct toplevel *toplevel_at(struct server *server);
/* the cursor is over a popup surface (menu / dropdown / tooltip): the popup
 * wins the pointer over the compositor frame (resize edges, title strip) */
bool pointer_over_popup(struct server *server);
/* the cursor is over a layer-shell surface (status bar / menu overlay):
 * like popups it wins the pointer - no window move/resize grabs while the
 * cursor is on it, clicks go to the layer surface instead */
bool pointer_over_layer_surface(struct server *server);
/* a dialog / transient window (declared a parent toplevel via
 * xdg_toplevel.set_parent, as GTK/Qt dialogs do): not resizable, no
 * maximize/minimize, its top border is a single close button */
bool toplevel_is_dialog(struct toplevel *tl);
/* a fixed-size window (min == max): cannot resize / maximize / minimize */
bool toplevel_is_fixed_size(struct toplevel *tl);

/* ---- toplevel.c: xdg-shell windows, window state, decorations ---- */
void toplevel_box(struct toplevel *tl, struct wlr_box *box);
struct wlr_output *toplevel_output(struct server *server,
	struct toplevel *tl);
struct toplevel *neighbor_toplevel(struct server *server,
	struct toplevel *tl, bool next, bool include_minimized);
struct toplevel *toplevel_by_id(struct server *server, int id);
void close_toplevel(struct toplevel *tl);
void set_fullscreen(struct server *server, struct toplevel *tl,
	bool fullscreen);
void set_maximized(struct server *server, struct toplevel *tl,
	bool maximized);
void restore_maximized_toplevel(struct toplevel *tl, bool animate);
void set_minimized(struct server *server, struct toplevel *tl,
	bool minimized);
void focus_toplevel(struct server *server, struct toplevel *tl);
void xdg_activation_request_activate(struct wl_listener *listener, void *data);
void update_toplevel_output(struct server *server, struct toplevel *tl);
void arrange_toplevels_work_area(struct server *server,
	struct wlr_output *output);
/* maximized window geometry for an output (see toplevel.c): used by the
 * restore/drag clamps so they land on the same pixel as the maximized box */
void maximized_box(struct server *server, struct wlr_output *output,
	struct wlr_box *box);
void server_new_toplevel(struct wl_listener *listener, void *data);
void server_new_decoration(struct wl_listener *listener, void *data);

/* ---- rounded.c: offscreen rounded-corner FBO cache ---- */
struct rounded_cache *rounded_cache_create(struct server *server,
	struct toplevel *tl);
void rounded_cache_destroy(struct rounded_cache *rc);
void rounded_cache_dirty(struct toplevel *tl);
void rounded_cache_dirty_content(struct toplevel *tl);
void rounded_cache_dirty_mask(struct toplevel *tl);
void rounded_cache_content_commit(struct toplevel *tl);
void rounded_cache_subsurface_commit(struct toplevel *tl,
	struct toplevel_subsurface *ts);
void rounded_cache_hide_content(struct toplevel *tl);
/* fade a whole window (animate.c): normally only the rounded FBO node is
 * visible (the raw client content is hidden at opacity 0), so it animates
 * that node; before the first FBO publish / with rounded corners disabled
 * every scene buffer under the window tree is faded instead */
void rounded_window_set_opacity(struct toplevel *tl, float opacity);
/* maximize/restore zoom & open/close scale support (animate.c): */
/* maximize/restore zoom support (animate.c): */
bool rounded_morph_supported(struct toplevel *tl); /* FBO visible & scalable */
bool rounded_cache_size_ready(struct toplevel *tl, int width, int height);
void rounded_cache_morph_apply(struct toplevel *tl);
void rounded_render_all(struct server *server);

/* ---- border.c: window border width and focus-dependent color ---- */
float border_width(struct toplevel *tl);
float border_gradient_width(struct toplevel *tl);
struct wlr_render_color border_color(struct server *server,
	struct toplevel *tl);
void border_top_colors(struct toplevel *tl,
	struct wlr_render_color *left, struct wlr_render_color *mid,
	struct wlr_render_color *right);
void border_focus_changed(struct toplevel *tl, struct toplevel *prev);

/* ---- shadow.c: window shadow width/opacity policy ---- */
float shadow_sigma(struct toplevel *tl);
float shadow_alpha(void);
struct wlr_render_color shadow_color(void);
int shadow_padding(void);

/* ---- place.c: initial window placement ----
 * size fully client-driven, position centered on the output (screen) */
bool place_toplevel(struct server *server, struct toplevel *tl);

/* ---- animate.c: window animations ----
 *
 * Each entry point returns true when an animation was started.  When it
 * returns false the caller must apply the state change instantly (old,
 * animation-less behavior):
 *
 *   animate_toplevel_minimize: fall vertically from the current position
 *     until below the bottom edge of the output, then hide the scene node
 *     (caller has already set tl->minimized and moved focus away);
 *   animate_toplevel_restore:   the hidden window drops back in from above
 *     its own top edge and lands exactly where it was;
 *   animate_toplevel_fade_in:   new window fades in from opacity 0;
 *   animate_toplevel_close:     fade out, then send the xdg close once the
 *     window is invisible (returns true while the fade runs; the caller
 *     must not send the close itself);
 *   animate_toplevel_cancel:    stop the running animation and bring the
 *     window back to a clean visible/hidden state (used when the window
 *     unmaps before an animation finished). */
bool animate_toplevel_minimize(struct server *server, struct toplevel *tl);
bool animate_toplevel_restore(struct server *server, struct toplevel *tl);
bool animate_toplevel_fade_in(struct server *server, struct toplevel *tl);
bool animate_toplevel_close(struct toplevel *tl);
/* Windows-style maximize/restore zoom.  `from` is the box the window is
 * shown in now, `to` the target box; the caller has already sent the final
 * size configure and must NOT move the scene node itself - the zoom scales
 * the window between the two boxes and leaves the node at `to`'s origin.
 * Returns false when the zoom cannot run (caller applies `to` instantly). */
bool animate_toplevel_geometry(struct server *server, struct toplevel *tl,
	const struct wlr_box *from, const struct wlr_box *to);
/* stop an in-flight maximize/restore zoom and land the window on its zoom
 * target (used by toplevel.c when a new maximize/restore request replaces
 * the running one, e.g. rapid toggling or clients re-asserting the state
 * before the first configure is acked) */
void animate_toplevel_abort_geometry(struct toplevel *tl);
void animate_toplevel_cancel(struct toplevel *tl);
/* advance every running window animation to the given CLOCK_MONOTONIC
 * instant; called by each output's frame handler right before the scene
 * is rendered (output.c), so every rendered frame shows the eased state
 * of its own vblank (no beat between a fixed timer and the display
 * refresh; high-refresh outputs interpolate proportionally more states) */
void anim_frame_tick(struct server *server, uint32_t now_ms);

/* ---- layer.c: wlr-layer-shell + work area ---- */
void get_work_area(struct server *server, struct wlr_output *output,
	struct wlr_box *area);
void server_new_layer_surface(struct wl_listener *listener, void *data);
/* release the interactive layer surface's keyboard hold (if any) and move
 * the seat keyboard to `surface` in one step (NULL clears it) */
void layer_keyboard_clear(struct server *server, struct wlr_surface *surface);

/* ---- output.c: monitors + output management ---- */
void server_new_output(struct wl_listener *listener, void *data);
void server_layout_change(struct wl_listener *listener, void *data);
void output_manager_apply(struct wl_listener *listener, void *data);
void output_manager_test(struct wl_listener *listener, void *data);

/* ---- input.c: seat, keyboard, shortcuts ---- */
void focus_window(struct server *server, struct toplevel *tl);
void seat_request_set_cursor(struct wl_listener *listener, void *data);
void seat_request_set_shape(struct wl_listener *listener, void *data);
void seat_request_set_selection(struct wl_listener *listener, void *data);
void seat_request_set_primary_selection(struct wl_listener *listener,
	void *data);
void seat_request_start_drag(struct wl_listener *listener, void *data);
void seat_start_drag(struct wl_listener *listener, void *data);
void server_new_virtual_pointer(struct wl_listener *listener, void *data);
void server_new_virtual_keyboard(struct wl_listener *listener, void *data);
void server_new_input(struct wl_listener *listener, void *data);

/* one attached keyboard device (real or virtual): keeps per-device key /
 * modifiers listeners and owns the IM-grab key forwarding state */
struct keyboard {
	struct server *server;
	struct wlr_keyboard *keyboard;
	struct wl_list link; /* server.keyboards */

	struct wl_listener key;
	struct wl_listener modifiers;
	struct wl_listener destroy;
};

/* ---- input.c: seat, keyboard focus, shortcuts ---- */
void server_new_virtual_keyboard(struct wl_listener *listener, void *data);
bool keyboard_is_typing(struct wlr_input_device *device);
/* move the seat keyboard to a surface (NULL clears the focus) */
void seat_keyboard_focus(struct server *server, struct wlr_surface *surface);

/* ---- ime.c: input method relay (fcitx5 / ibus) ---- */
void ime_set_focus(struct server *server, struct wlr_surface *surface);
void ime_attach_keyboard(struct server *server,
	struct wlr_keyboard *keyboard);
void ime_detach_keyboard(struct server *server,
	struct wlr_keyboard *keyboard);
/* true if the input method's keyboard grab is connected to this keyboard */
bool ime_keyboard_grabbed(struct server *server,
	struct wlr_keyboard *keyboard);
/* forward a key event to the input method(s) whose grab is connected to
 * this keyboard; called from input.c's single key handler after compositor
 * shortcuts were checked, so a consumed key is simply never passed here */
void ime_forward_key(struct server *server, struct wlr_keyboard *keyboard,
	struct wlr_keyboard_key_event *event);
void ime_update_popup(struct server *server);
void ime_new_input_method(struct wl_listener *listener, void *data);
void ime_new_text_input(struct wl_listener *listener, void *data);

/* ---- pointer.c: cursor interaction (move / resize / gestures) ---- */
void begin_move(struct server *server, struct toplevel *tl,
	double ref_x, double ref_y);
void begin_resize(struct server *server, struct toplevel *tl, uint32_t edges);
void end_move(struct server *server);
void end_resize(struct server *server);
/* clear the resize grab state without applying geometry (used once the final
 * commit lands in outline mode, and when the toplevel goes away mid-grab) */
void resize_grab_clear(struct server *server);
/* is the cursor over the compositor's own frame zone (title strip / resize
 * edge) of any window?  There the compositor owns the cursor, so client
 * cursor requests are ignored (input.c) - the client still receives motion
 * and keeps its hover feedback, it just cannot change the cursor. */
bool pointer_over_frame_zone(struct server *server);
void update_cursor_style(struct server *server);
/* show the cursor the focused client currently wants (shape, surface or the
 * default arrow); used when a compositor cursor override ends */
void reapply_client_cursor(struct server *server);
void cursor_motion(struct wl_listener *listener, void *data);
void cursor_motion_absolute(struct wl_listener *listener, void *data);
void cursor_button(struct wl_listener *listener, void *data);
void cursor_axis(struct wl_listener *listener, void *data);
void cursor_frame(struct wl_listener *listener, void *data);

#endif /* XMONODYWM_SERVER_H */
