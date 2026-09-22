# monodywm

A minimal floating Wayland compositor written in C on top of **wlroots 0.20**
and **scenefx 0.5**.

## Design

* Floating windows only — no tiling, no tabs.
* The compositor draws its own **rounded frame** with scenefx: every window
  gets a rounded border ring, a drop shadow (focused windows only) and an
  optional background blur, all tunable in `src/config.h`
  (`CONFIG_ROUNDED_RADIUS`, `CONFIG_BORDER_*`, `CONFIG_SHADOW_*`,
  `CONFIG_BLUR*`).  The top border is split into three colored thirds
  (left = minimize, middle = maximize/restore, right = close) with a
  configurable gradient between them.  Client-side decorated windows keep
  their native controls, and undecorated windows get only invisible grab
  zones (title strip and resize edges) on top of it.
* **Client-side decorated** windows (mode `CLIENT_SIDE` via xdg-decoration, or
  apps with their own header bars) keep their native controls: the client's own
  title bar moves the window through `xdg_toplevel.move`, its buttons work, its
  edges resize natively, and `request_maximize` / `request_minimize` are honored.
* Some clients paint their own CSD but have no working compositor-side resize
  path (QQ, Chromium/Chrome, Firefox, VS Code, Clash Verge, …).  Those
  `app_id`s are listed in `config_force_undecorated[]` in `src/config.h` and are
  treated as undecorated: the compositor supplies the resize edges and the
  resize cursor, and ignores the client's own resize cursor shapes.
* **Server-side / undecorated** windows get an *invisible* frame owned by the
  compositor: grab zones around the
  left/right/bottom edges and corners, and a title strip across the top
  whose left/middle/right thirds map to minimize / maximize-restore / close;
  the cursor style updates as soon as the pointer enters them and reverts
  as soon as it crosses back into the window):

  | pointer position | cursor | gesture | action |
  |---|---|---|---|
  | top strip (`CONFIG_TITLEBAR_HEIGHT` px) | `pointer` (`CONFIG_TITLEBAR_CURSOR`) | hover | hints the strip is draggable |
  | top strip | `all-scroll` (`CONFIG_MOVE_CURSOR`) | hold (left or right) + drag | move the window |
  | top strip | `all-scroll` | long press (~350 ms) | grab the window, it follows the cursor |
  | top strip, left third  | `pointer` | double click | minimize the window |
  | top strip, middle third | `pointer` | double click | toggle maximize / restore |
  | top strip, right third | `pointer` | double click | close the window |
  | anywhere over the window | (client's) | hold right + wheel up    | toggle maximize / restore |
  | anywhere over the window | (client's) | hold right + wheel down  | toggle minimize / restore |
  | anywhere over the window | (client's) | hold right, double-click left | toggle maximize / restore |
  | anywhere over the window | (client's) | hold left, double-click right | close the window |
  | anywhere over the window | `all-scroll` | hold one button, hold the other | move the window (release restores the cursor) |
  | left/right edge    | `ew-resize`   | hold + drag              | resize horizontally |
  | bottom edge        | `ns-resize`   | hold + drag              | resize vertically |
  | bottom corners     | `nwse/nesw`   | hold + drag              | resize diagonally |

  Clicking anywhere on a window focuses and raises it.
* **Real-time resize or outlined resize.**  With
  `CONFIG_RESIZE_DRAW_CONTENTS=1` (default) the client is resized live while
  dragging.  With `=0` the compositor only draws a target outline while the
  pointer moves and applies the final size on button release; if the client
  doesn't commit that size within `CONFIG_RESIZE_FINAL_TIMEOUT_MS` the resize
  is force-finished anyway.
* **Popups win the pointer over the compositor frame.**  While a popup
  (menu / dropdown / tooltip) is open, everything under it belongs to the
  popup: the compositor's resize edges and title strip are disabled there,
  so hovering a menu that overlaps the window's border never starts a
  resize and clicks land on the menu instead of the frame.
* **Right-hold + wheel and the two-button chords are gated on
  `CONFIG_WHEEL_DEBOUNCE_ENABLED`** (on by default).  A trackpad flick or a
  high-resolution wheel delivers many ticks in one burst, so two
  thresholds coalesce them: one continuous scroll (however fast) counts
  as a single action for at most `CONFIG_WHEEL_BURST_NS` (800 ms), and
  two ticks at least `CONFIG_WHEEL_TICK_GAP_NS` (300 ms) apart are the
  next action — so a flick toggles maximize/restore once, while a slow,
  deliberate scroll fires per notch.  The burst resets on right-button
  release.
* **Dragging the top strip of a maximized window restores it** to its previous
  position and size, then the drag continues with the cursor gripping the
  restored title bar — same as Windows.
* **Dragging is clamped Windows-style.**  A layer-shell bar at the top is a
  hard boundary: the window's top edge can never slide above it.  A bottom
  bar does not block dragging: the cursor itself is kept above the bar
  while moving, and the window follows it with no artificial limit, so its
  bottom may slide past the bar and off the bottom of the screen.  The
  cursor can never enter a bar's exclusive zone during a move.
* **New windows are centered** on the output under the cursor, keeping the
  size the client asked for.  Because some Electron apps (e.g. QQ) map a
  small placeholder surface first and commit the real size later, the window
  is re-centered on every surface size change until the user interacts with
  it (move / resize / maximize / fullscreen).  `CONFIG_CENTER_AVOID_BARS`
  switches the reference from the whole output to the work area so a
  layer-shell bar can never cover a new window.
* **Dialogs and fixed-size windows are recognized.**  A toplevel that has a
  parent, or that is explicitly marked via `xdg-dialog-v1`, is treated as a
  dialog: it is kept off the taskbar (no IPC id) and focus on it is reported as
  focus on its owner window.  A toplevel whose `min_width/height` equals its
  `max_width/height` (login boxes, splash screens) is treated as fixed-size.
  Neither can be maximized, minimized or resized from the compositor frame.
* **The cursor size is never guessed by applications.**  The compositor
  implements `cursor-shape-v1`: a client picks a shape (text, pointer, resize
  handles, ...) and the compositor renders it itself, from its own xcursor
  theme at the output's exact fractional scale.  On a 1.75x output the
  compositor's own cursor and the client's cursor are therefore the same
  36×36 image (24 px × 1.75 = 42 px, snapped to the theme's 36 px image) —
  previously each toolkit rounded 1.75 to 2 (or used its own size setting)
  and drew its own bitmap, so the cursor visibly changed size when crossing
  between client cursors, the compositor's title-strip/resize cursors and
  layer-shell bars.  Apps that still draw custom cursors via
  `wl_pointer.set_cursor` keep doing so, but the standard shapes all go
  through the compositor.

## Protocols

| protocol | notes |
|---|---|
| `wl_compositor` (v6) | with `wl_subcompositor` |
| `wl_surface` / `wl_region` | part of the compositor |
| `wl_seat` | pointer + keyboard |
| `wl_shm` | via `wlr_renderer_init_wl_display` |
| `zwp_linux_dmabuf_v1` (v5) | via `wlr_renderer_init_wl_display` |
| `xdg_wm_base` (v6) | toplevels, popups, move/resize/maximize/minimize requests |
| `xdg_wm_dialog_v1` | explicitly marks a toplevel as a dialog (kept out of the taskbar) |
| `wp_viewporter` | |
| `wp_presentation` (v2) | frame callbacks via `wlr_scene_output_send_frame_done` |
| `zwlr_layer_shell_v1` (v5) | background/bottom/top/overlay layers |
| `zxdg_decoration_manager_v1` | client requests honored; default `CLIENT_SIDE` |
| `zwlr_output_manager_v1` | apply/test + config broadcast |
| `zxdg_output_manager_v1` | logical output geometry/name (needed by grim, xwayland) |
| `zwlr_foreign_toplevel_manager_v1` | title/app_id/state + requests |
| `zwlr_virtual_pointer_manager_v1` | extra, used for input testing |
| `zwp_virtual_keyboard_manager_v1` | extra, used by the IME and input testing |
| `wp_cursor_shape_manager_v1` | clients pick a cursor shape; the compositor renders it from its own xcursor theme at the output's (fractional) scale, so the size always matches — no client-side guessing |
| `xdg_activation_v1` | client-driven window activation/focus; activation requests focus (and restore) the matching toplevel |
| `wp_fractional_scale_v1` | surfaces are told the output's exact fractional scale |
| `wp_color_manager_v1` (v2) | client-declared **parametric** image descriptions (named transfer function + named primaries); only advertised when the renderer supports input color transforms (currently only the Vulkan renderer, so run with `WLR_RENDERER=vulkan`), and the manager is attached to the scene so surfaces render with their description — otherwise clients fall back to sRGB. ICC v2/v4 and mastering-display metadata are not supported |
| `wp_single_pixel_buffer_manager_v1` | clients fill a surface with one solid color (backgrounds/overlays) without allocating a 1×1 `wl_shm`/dmabuf buffer |
| `ext_data_control_manager_v1` | privileged selection/clipboard control used by `wl-clipboard` (`wl-copy`/`wl-paste`) and clipboard managers, which is how terminal editors such as `vim` reach the Wayland clipboard |
| `zwlr_data_control_manager_v1` | legacy wlroots clipboard-control protocol; still the only one bound by CopyQ and older clipboard tools, advertised alongside the `ext-` variant |
| `zwp_primary_selection_device_manager_v1` | the standard PRIMARY selection (middle-click paste); the seat request listener drives `wlr_seat_set_primary_selection()` |
| `zwp_pointer_constraints_v1` | clients lock/confine the pointer to a surface — QEMU's captured mouse (Ctrl+Alt+G grab), games' mouselook; confined motion is clamped to the region, a locked pointer stays put |
| `zwp_relative_pointer_v1` | raw pointer deltas delivered while the pointer is locked (`QEMU`'s relative mouse mode), so the client's own cursor still tracks the hardware |
| `zwp_keyboard_shortcuts_inhibit_manager_v1` | a focused client (GTK4 app, remote desktop, a VM UI) asks the compositor to pass its keys through instead of running bindings; while an inhibitor is active the compositor forwards every key |
| `zwlr_screencopy_manager_v1` | screen capture for grim / slurp / wf-recorder (frame capture, damage, cursor overlay implemented by wlroots) |
| `wp_linux_drm_syncobj_manager_v1` | explicit buffer synchronization via DRM syncobj timelines (only when renderer + backend support timelines) |
| `zwp_input_method_v2` | input method (fcitx5/ibus) — activation, keyboard grab, preedit/commit |
| `zwp_text_input_v3` | per-window text input — enter/leave, surrounding text, commit string |

## Source layout

`main.c` was split into small modules under `src/`:

```
src/
  config.h    all tunables: shortcuts, decoration, cursor, gestures, app list
  server.h    shared structs (server/toplevel/layer_surface) + cross-module API
  main.c      entry point: display/backend/scene setup, protocol globals, run loop
  ipc.c/h     status-bar socket (JSON over a Unix domain socket)
  scene.c     scene-graph tagging / hit-testing helpers
  toplevel.c  xdg-shell windows, window state (max/min/fullscreen)
  decor.c     scenefx decoration: rounded corners, border ring, shadow, blur
  place.c     new-window placement (auto-centering)
  layer.c     wlr-layer-shell surfaces + work area
  output.c    monitors, output layout, wlr-output-management
  input.c     seat, keyboard, compositor shortcuts
  ime.c       input method relay: zwp_input_method_v2 <-> zwp_text_input_v3
              (fcitx5 / ibus Chinese input)
  pointer.c   cursor interaction (move / resize / title-bar gestures)
```

## Chinese input (fcitx5)

The compositor implements the **input method relay** so fcitx5 (or ibus)
can type Chinese.  The two protocols involved are wired together in
`ime.c`:

* `zwp_input_method_unstable_v2` — fcitx5 connects as the input method;
  the relay activates it when a surface gains keyboard focus and forwards
  its preedit/commit state to the focused window.
* `zwp_text_input_unstable_v3` — each focused window exposes its text input;
  surrounding text and content type flow to the input method, and the
  committed string flows back into the window.

While fcitx5 holds the **keyboard grab** (it does this while composing),
raw key events are forwarded to it instead of the focused client, and its
candidate window (an input popup surface) is shown in the overlay layer
following the cursor.

Start fcitx5 before the applications you want to type into:

```sh
fcitx5 -d
```

The relay is verified end to end by `test-ime-relay` (synthetic app + IM)
and by driving a real fcitx5 with the bundled `test-ime-app` + a virtual
keyboard.  If fcitx5 starts but stays in *keyboard-us* passthrough instead
of composing Chinese, the compositor is fine — check that:

* pinyin is in fcitx5's group (`fcitx5-configtool`, or
  `fcitx5-remote -s pinyin` with an active window), and
* the pinyin dictionary exists (`ls /usr/share/libime/pinyin.dict`).
  On some systems the `libime-data` package ships without it, which makes
  fcitx5 silently fall back to passthrough; fix with
  `sudo apt-get install --reinstall libime-data` (or reinstall
  `fcitx5-chinese-addons`).

## Protocol code generation

Every protocol this project speaks is described by an XML file.  Most XMLs
come from the system wayland / wayland-protocols trees
(`/usr/share/wayland`, `/usr/share/wayland-protocols`); only the `wlr-*`
protocols and the wlroots-maintained extras (input-method, virtual-keyboard)
are vendored in `protocol/` (8 files).  Sources are looked up by XML file
name, so a version bump or a move between `stable/` and `staging/` is picked
up without editing anything.

At build time CMake runs `wayland-scanner` over each file and emits three
artifacts into `build/protocol/`:

* `<name>-protocol.h`         (server-side header; wlroots' installed headers
                               `#include` these, e.g. `wlr_layer_shell_v1.h`)
* `<name>-client-protocol.h`  (client-side header)
* `<name>-protocol.c`         (marshalling code)

The compositor itself links none of that code: as a wlroots compositor,
wlroots implements the globals server-side.  The generated client code is
what the bundled test clients link against, so the tests are fully
self-contained.  To add a protocol: drop (or find) the XML, add its name to
the `PROTOCOLS` list in `CMakeLists.txt` and (for tests) include the
generated client header.

## Layout

```
layers (bottom -> top):
  background < bottom < toplevels < top < overlay
```

Layer-shell exclusive zones shrink the work area that maximized windows use.
A maximized window fills the work area exactly, flush against any
layer-shell status bars' exclusive zones.

## Build

Built with **CMake**.  It needs `wlroots-0.20`, `scenefx-0.5` and the usual
Wayland libraries (`wayland-server`, `wayland-client`, `xkbcommon`,
`libcjson`, `pixman`, `libinput`, `wayland-scanner`).  The installed wlroots
package doesn't ship the generated protocol headers it `#include`s
internally; they are generated from the protocol XMLs into the build tree,
so no external protocol build directory is needed:

```sh
PKG_CONFIG_PATH=/usr/local/lib/x86_64-linux-gnu/pkgconfig \
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The compositor is linked with **scenefx before wlroots** on purpose: both
libraries export `wlr_scene_*`, and only the scenefx implementation is
ABI-compatible with the scene nodes it creates.

Run (from a TTY / with a seat):

```sh
./build/monodywm [-s 'startup command']
```

**Autostart** — once the compositor is up (backend started, Wayland socket
live) it reads the user's startup commands from **`~/.config/monodywm/run`**
(`$XDG_CONFIG_HOME/monodywm/run` if set) and launches them one per line through
`/bin/sh -c`, so `~`, $VARS, quotes and shell syntax all work. **Every line
is backgrounded automatically** (an explicit trailing `&` is optional and
handled without double-`&` errors), blank lines and `#` comments are
skipped (trailing comments too), and each line is logged at startup.
Every spawned process is **detached the standard way**: `setsid()` puts it
in its own session without a controlling terminal (no Ctrl+C / SIGHUP from
the tty, survives the terminal going away), stdin/stdout/stderr are
redirected to `/dev/null` (no output polluting the compositor's tty, no
blocking on terminal I/O), and helpers are reaped via SIGCHLD so they never
pile up as zombies. Typical contents:

```sh
# output configuration, wallpaper, input method
wlr-randr --output Virtual-1 --mode 2880x1800 --scale 1.75
swaybg -i ~/1.jpg -f full
pgrep -x fcitx5 > /dev/null 2>&1 || fcitx5 -d
```

`-s 'cmd'` still works as a one-off startup command (run after the file).

The bundled test clients are opt-in: configure with `-DTEST=ON`.

## Configuration

Everything tunable lives in **`src/config.h`** (edit and rebuild).  The main
options:

| option | meaning | default |
|---|---|---|
| `CONFIG_CURSOR_THEME` | xcursor theme used for the compositor's cursors | `Adwaita` |
| `CONFIG_TITLEBAR_CURSOR` | cursor shown while hovering the title strip | `pointer` |
| `CONFIG_MOVE_CURSOR` | cursor shown while moving a window | `all-scroll` |
| `CONFIG_ROUNDED_RADIUS` | window corner radius (px) | `8` |
| `CONFIG_BORDER_WIDTH` | border ring width (px) | `2.0` |
| `CONFIG_BORDER_FOCUSED` / `CONFIG_BORDER_UNFOCUSED` | focused / unfocused border color (`0xRRGGBB`) | `0x7c73b0` |
| `CONFIG_BORDER_UNFOCUSED_DRAW` | draw a border on unfocused windows too (0/1) | `1` |
| `CONFIG_BORDER_TOP_LEFT` / `_MID` / `_RIGHT` | top-strip third colors (minimize / maximize / close) | see `config.h` |
| `CONFIG_BORDER_GRADIENT_WIDTH` / `CONFIG_BORDER_GRADIENT_STEPS` | width (px) and approximation steps of the color gradient between the three top segments | `15` / `8` |
| `CONFIG_FULLSCREEN_BORDER*` | draw/color/radius of the border on fullscreen windows | on / `0xb87898` / `8` |
| `CONFIG_BLUR` / `CONFIG_BLUR_LAYER` | background blur for windows / for non-overlay layer surfaces | `1` / `1` |
| `CONFIG_BLUR_RADIUS` / `CONFIG_BLUR_PASSES` | blur sample radius / downsample passes | `5` / `3` |
| `CONFIG_SHADOW_BLUR_SIGMA` / `_COLOR` / `_ALPHA` | drop-shadow softness / color / opacity | `16.0` / `0x22222f` / `0.4` |
| `CONFIG_TITLEBAR_HEIGHT` | title strip grab zone at the window top (px) | `6` |
| `CONFIG_EDGE_THICKNESS` | grab zone on window edges/corners for move+resize (px) | `6` |
| `CONFIG_RESIZE_DRAW_CONTENTS` / `CONFIG_RESIZE_FINAL_TIMEOUT_MS` | resize live (`1`) or outline (`0`) / grace period for the final commit | `1` / `500 ms` |
| `CONFIG_DOUBLE_CLICK_NS` | double-click detection window | `400 ms` |
| `CONFIG_LONG_PRESS_NS` | holding the strip this long grabs the window for moving | `350 ms` |
| `CONFIG_DRAG_THRESHOLD` | pointer travel (px) before a press counts as a drag | `4.0` |
| `CONFIG_WHEEL_DEBOUNCE_ENABLED` / `_BURST_NS` / `_TICK_GAP_NS` | right-hold + wheel: coalesce rapid ticks (bool) / one continuous scroll = one action for at most this long / two ticks this far apart = next action | `true` / `800 ms` / `300 ms` |
| `CONFIG_CENTER_AVOID_BARS` | center new windows on the work area instead of the whole output (0/1) | `0` |
| `CONFIG_MODKEY0..4` | modifier combinations (`Alt`, `Win`, `Shift+Alt`, `Shift+Ctrl`, `Ctrl+Alt`) | — |
| `config_action_shortcuts[]` | compositor key bindings (see below) | — |
| `config_app_shortcuts[]` | launch-app key bindings | — |
| `config_force_undecorated[]` | case-insensitive `app_id` prefixes forced to undecorated | see `config.h` |

## Shortcuts

All key bindings (modifier combos and keysyms) are defined in
**`src/config.h`** — edit it and rebuild.  Defaults:

| keys            | action                     |
|---|---|
| `Win+Enter`, `Shift+Alt+Enter` | toggle maximize / restore the focused window |
| `Shift+Alt+M`    | minimize the focused window |
| `Ctrl+Alt+N`     | focus the next window (minimized windows are restored) |
| `Ctrl+Alt+P`     | focus the previous window (minimized windows are restored) |
| `Ctrl+Alt+C`     | close the focused window      |
| `Ctrl+Alt+Q`     | close every other window      |
| `Shift+Ctrl+Q`   | quit the compositor        |
| `Win+1` … `Win+9`| switch to the N-th window     |
| `Win+T`          | open a `foot` terminal        |
| `Win+K`          | open `kitty`                  |
| `Win+W`          | open `wezterm`                |
| `Win+F`          | open `firefox`                |
| `Win+Q`          | open `qq`                     |
| `Win+S`, `Win+P` | open `rofi -show drun`        |

Notes:

* Maximizing remembers the floating geometry; pressing the maximize shortcut
  again restores the previous size and position.
* Minimizing only hides the window, so it reappears at its original position
  when restored (via the focus cycle or a foreign-toplevel activate).
* Closing or minimizing the focused window hands keyboard/cursor focus to the
  previous visible window (or clears it when no other window is shown).

Shortcuts are consumed by the compositor and not forwarded to clients.  While a
client on the focused surface holds a keyboard-shortcuts inhibitor, the
compositor stops running its own bindings and every key is forwarded to the
client.

## IPC (status bar)

The compositor exposes a Unix domain socket at
`$XDG_RUNTIME_DIR/monodywm.sock` (fallback `/tmp/monodywm.sock`). Status
bars connect and receive **newline-delimited JSON** messages. Each window is
identified by a stable `id` assigned by the compositor; `app_id` is the
client-provided application id (e.g. `firefox`) and `pid` its process id.
Dialogs are attached to their owner window for IPC purposes.

Events sent to every connected client:

```json
{"event":"window_added","id":1,"app_id":"firefox","pid":4242}
{"event":"window_focus","id":1,"app_id":"firefox","pid":4242}
{"event":"window_full","id":1,"app_id":"firefox","pid":4242}
{"event":"window_removed","id":1,"app_id":"firefox","pid":4242}
{"event":"window_focus","id":0,"app_id":"","pid":0}
```

* `window_added` – a window was mapped (id/app_id/pid of the new window).
* `window_removed` – a window was destroyed (id/app_id/pid of the closed window).
* `window_focus` – focus changed; `id` is the newly focused window, or `0`
  when nothing is focused anymore.
* `window_full` – a window entered or left fullscreen.

On connect, and in reply to a client request
`{"action":"list_windows"}`, the compositor sends the current window list
plus the currently focused id:

```json
{"event":"window_list","windows":[{"id":1,"app_id":"firefox","pid":4242}],"focused_id":1}
```

Requests a client can send (one JSON object per line):

| request | effect |
|---|---|
| `{"action":"list_windows"}` | reply with the current `window_list` |
| `{"action":"focus_window","id":2}` | focus (and restore if minimized) window 2; a `window_focus` event follows |
| `{"action":"close_window","id":2}` | request window 2 to close; `window_removed` follows |
| `{"action":"maximize_window","id":2}` | toggle maximize / restore for window 2 |
| `{"action":"minimize_window","id":2}` | minimize window 2 |

A taskbar can use this to switch focus when an icon is clicked.

**The compositor never dies on a dead bar.**  A status bar that exits or
crashes mid-session is harmless: writes to its closed socket are reported
as a normal error (SIGPIPE is ignored) and the disconnected client is
cleaned up without the event path ever touching it again — closing a
window while the bar is gone is safe.  Cursor state owned by a closing
client (cursor shape / cursor surface) is fully detached from the seat and
surface destroy signals, so the client teardown always stays clean.


## Tests

The repo contains a set of small Wayland clients used to validate the
compositor; CMake builds them from the client headers generated out of the
protocol XMLs (no dependency on a wlroots build tree).  They are opt-in:
configure with `-DTEST=ON`.

* `test-client.c` — xdg-shell configure/maximize/minimize/move and
  xdg-decoration mode negotiation.
* `test-interaction.c` — drives a virtual pointer to exercise the whole
  gesture set: drag-move and long-press-move, wheel maximize/minimize,
  double-click title-strip segments (left = minimize, middle =
  maximize/restore, right = close), client-side-decoration pass-through,
  edge resize, Windows-style restore-from-maximize, and the chord gestures
  (hold right + double-click left to toggle maximize, hold left +
  double-click right to close, hold the other button to move).
* `test-cursor.c` — terminal-like client (SSD + I-beam cursor request)
  driven by a virtual pointer; checks the cursor decisions through the
  compositor's `WLR_DEBUG` "cursor: ..." log.
* `test-cursor-shape.c` — cursor-shape-v1 negotiation: binds the
  cursor-shape global, sets a shape on the pointer and drives the pointer
  into a window to check the compositor renders it and restores it after an
  override.
* `test-select-drag.c` + `test-select-drag.sh` — regression test for the
  implicit pointer grab: a terminal-like client sets the text cursor, then
  the RIGHT button is held and the virtual pointer is dragged to the left /
  top / bottom edges (and LEFT is pressed at an edge while RIGHT is held).
  While any button is held the compositor must keep the client's cursor
  (never the edge-resize / title-strip hover cursors) and must freeze the
  client's mid-drag cursor-shape requests.  Run with `./test-select-drag.sh`
  (auto-asserts PASS/FAIL against the compositor's `WLR_DEBUG` log).
* `test-resize-cursor.c` + `test-resize-cursor.sh` — cursor stability
  during edge-resize drags: an SSD client honors configure sizes (real
  resize behavior) and re-requests its text cursor on every commit, while
  a virtual pointer presses the LEFT button at the right / bottom / left
  edges and drags.  The compositor must keep the resize cursor for the
  whole drag — the only cursor decisions allowed are the hover
  transitions between drags.  Run with `./test-resize-cursor.sh`
  (auto-asserts the exact cursor sequence from the `WLR_DEBUG` log).
* `test-fractional-scale.c` + `test-fractional-scale.sh` — attaches a
  physical-size checkerboard to a `wp_fractional_scale_v1` +
  `wp_viewporter` surface so a screen capture can tell 1:1 sampling apart
  from resampling at a fractional output scale.
* `test-mask-guard.c` — (obsolete) exercised the removed custom
  rounded-corner mask re-render path; kept only as a client commit
  smoke test.
* `test-restack.c` — subsurface restack test: `place_below`/`place_above`
  carry no damage and are applied on the parent commit, so the FBO cache
  has to detect the order change.
* `test-bar-clamp.c` — layer-shell bars (top/bottom, NULL-output and
  per-output) + a virtual pointer: verifies a dragged window can never
  slide underneath a status bar, and that its top never goes closer than
  `CONFIG_EDGE_THICKNESS` px to the screen top on a bar-less edge.
* `test-border.c` — two toplevels + focus switches (border color follows focus).
* `test-quit.c` — a virtual keyboard presses `Shift+Ctrl+Q`; verifies the
  compositor shuts down cleanly (no wlroots teardown assertions).
* `test-ime-relay.c` — drives the input method relay end to end: a fake
  app (text-input-v3) plus a fake input method (input-method-v2) verify
  that focus activates the IM, that the IM receives the keymap and key
  events through the keyboard grab, and that its preedit/commit string
  reaches the app.
* `test-grab-shortcut.c` — while the IM holds the keyboard grab, a global
  shortcut must still fire and the consumed key must not reach the IM.
* `test-ime-app.c` — real-app side of the fcitx5 test: opens a toplevel
  with text input enabled, drives a virtual keyboard (so it also needs the
  `virtual-keyboard-unstable-v1` protocol) and prints whatever the input
  method commits; run it with a real fcitx5 to type Chinese.

The compositor is exercised headless with:

```sh
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 ./build/monodywm \
  -s 'WAYLAND_DISPLAY=wayland-0 ./build/test-client; WAYLAND_DISPLAY=wayland-0 ./build/test-interaction'
```

## Acknowledgements

monodywm stands on the shoulders of these projects:

* **[dwl](https://codeberg.org/dwl/dwl)** — the tiny wlroots compositor whose
  layout, input handling and general philosophy shaped much of this one.
* **[dwl-patches / scenefx (stale patches)](https://codeberg.org/dwl/dwl-patches/src/branch/main/stale-patches/scenefx)**
  — the scenefx patch series that inspired the rounded corners, border,
  shadow and blur design used here.
* **[scenefx](https://github.com/wlrfx/scenefx)** — the wlroots scene fork
  that provides the rounded-corner, shadow and blur nodes this compositor
  draws its window frames with.
* **[wlroots](https://gitlab.freedesktop.org/wlroots/wlroots)** — the
  compositor library that implements most protocol globals, the backend and
  the render loop.
* **[labwc](https://github.com/labwc/labwc)** and DreamMaoMao's dwl PR #235
  (see the [dwl-patches](https://codeberg.org/dwl/dwl-patches) collection) —
  the input-method relay design (`ime.c`) connecting `zwp_input_method_v2` to
  `zwp_text_input_v3`.
