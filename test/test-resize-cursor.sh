#!/bin/sh
# Cursor stability during edge-resize drags.
#
# Scenario (build/test-resize-cursor via a virtual pointer):
#   - an SSD terminal-like client sets a text cursor on enter and
#     re-requests it on every configure commit
#   - the LEFT button is pressed in the right-edge zone and dragged right
#     (client honors configure sizes), then bottom, then left
#
# The compositor must keep the resize cursor for the whole drag: no
# cursor-image change may happen between a press and its release.  The
# only cursor decisions in the whole run are the four hover transitions:
#   ew-resize (right edge) -> ns-resize (bottom edge) -> left_ptr
#   (release below the window) -> ew-resize (left edge)
# Any extra "cursor:" line means the cursor image changed mid-drag.
#
# Usage: ./test/test-resize-cursor.sh   (from the repo root, after
#        cmake --build build)
set -u
cd "$(dirname "$0")/.."

RUNDIR=$(mktemp -d /tmp/wmrc.XXXXXX)
trap 'rm -rf "$RUNDIR"' EXIT
mkdir -p "$RUNDIR/config"

# 1. start the compositor headless with cursor debug logging
XDG_CONFIG_HOME="$RUNDIR/config" XDG_RUNTIME_DIR="$RUNDIR" \
  WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_DEBUG=1 \
  ./build/monodywm > "$RUNDIR/wm.log" 2>&1 &
WM=$!
sleep 2

# 2. drive the virtual pointer through the resize drags
XDG_RUNTIME_DIR="$RUNDIR" WAYLAND_DISPLAY=wayland-0 \
  ./build/test-resize-cursor > "$RUNDIR/client.log" 2>&1
RC=$?
sleep 0.3
kill "$WM" 2>/dev/null
wait "$WM" 2>/dev/null

if [ "$RC" -ne 0 ]; then
    echo "FAIL: test client failed (rc=$RC)"
    cat "$RUNDIR/client.log"
    exit 1
fi

echo "---- compositor cursor decisions ----"
grep -E "cursor:" "$RUNDIR/wm.log"

FAILED=0

# the exact sequence of cursor decisions the run may produce
CURSORS=$(grep -E "cursor:" "$RUNDIR/wm.log" | sed -E 's/.*cursor: ([a-z_-]+).*/\1/')
EXPECTED="ew-resize
ns-resize
left_ptr
ew-resize"
if [ "$CURSORS" != "$EXPECTED" ]; then
    echo "FAIL: cursor changed mid-drag (expected '$EXPECTED', got:)"
    echo "$CURSORS"
    FAILED=1
fi

if [ "$FAILED" -eq 0 ]; then
    echo "PASS: resize cursor stayed stable during all drags"
    exit 0
fi
exit 1
