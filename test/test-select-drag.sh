#!/bin/sh
# Regression test for cursor behavior during a held-button drag.
#
# Scenario (driven by build/test-select-drag via a virtual pointer):
#   - a terminal-like client sets the "text" cursor on pointer enter
#   - the RIGHT button is held and the pointer is dragged to the left edge,
#     to the top edge, to the bottom edge, and out of the edge zones
#   - while RIGHT is held, LEFT is also pressed at an edge
#   - the LEFT button is released, then the RIGHT button is released
#
# The compositor must:
#   1. keep the pointer focused on the grabbed surface and keep forwarding
#      motion to it during the hold (implicit grab);
#   2. never switch to the edge-resize / title-strip hover cursors while a
#      button is held (buttons >= 1);
#   3. freeze cursor changes the client requests mid-drag (the client tries
#      to set the E_RESIZE shape near the top edge - it must be ignored).
# After the release, normal hover behavior resumes.
#
# Usage: ./test/test-select-drag.sh   (from the repo root, after
#        cmake --build build)
set -u
cd "$(dirname "$0")/.."

RUNDIR=$(mktemp -d /tmp/wmsel.XXXXXX)
trap 'rm -rf "$RUNDIR"' EXIT
mkdir -p "$RUNDIR/config"

# read the configured title-strip cursor name so the assertion follows config.h
TITLEBAR=$(sed -n 's/.*#define[[:space:]]*CONFIG_TITLEBAR_CURSOR[[:space:]]*"\([^"]*\)".*/\1/p' src/config.h)
[ -z "$TITLEBAR" ] && TITLEBAR="pointer"

# 1. start the compositor headless with cursor debug logging
XDG_CONFIG_HOME="$RUNDIR/config" XDG_RUNTIME_DIR="$RUNDIR" \
  WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_DEBUG=1 \
  ./build/monodywm > "$RUNDIR/wm.log" 2>&1 &
WM=$!
sleep 2

# 2. drive the virtual pointer through the scenario
XDG_RUNTIME_DIR="$RUNDIR" WAYLAND_DISPLAY=wayland-0 \
  ./build/test-select-drag > "$RUNDIR/client.log" 2>&1
RC=$?
sleep 0.3
kill "$WM" 2>/dev/null
wait "$WM" 2>/dev/null

if [ "$RC" -ne 0 ]; then
    echo "FAIL: test client failed (rc=$RC)"
    cat "$RUNDIR/client.log"
    exit 1
fi

# the client must have received motion while the button was held
if ! grep -q "motion" "$RUNDIR/client.log"; then
    echo "FAIL: no motion was forwarded to the client during the drag"
    exit 1
fi

# 3. assertions on the compositor cursor decisions
FAILED=0
# no hover cursor (edge resize / title strip) while a button is held
if grep -E "cursor: (ew-resize|ns-resize|nesw-resize|nwse-resize|$TITLEBAR) \(buttons=[1-9]" "$RUNDIR/wm.log"; then
    echo "FAIL: compositor switched to a hover cursor while a button was held"
    FAILED=1
fi
# the client's mid-drag E_RESIZE shape request must have been frozen
if grep -q "cursor: shape 18" "$RUNDIR/wm.log"; then
    echo "FAIL: client's E_RESIZE shape was applied during the hold (freeze broken)"
    FAILED=1
fi

if [ "$FAILED" -eq 0 ]; then
    echo "PASS: cursor stayed on the client during the held-button drag"
    exit 0
fi
echo "---- compositor cursor log ----"
grep -E "cursor:" "$RUNDIR/wm.log"
exit 1
