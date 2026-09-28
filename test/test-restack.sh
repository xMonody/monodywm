#!/bin/sh
# Subsurface restack re-render guard: wl_subsurface.place_below/place_above
# restacks are applied on the parent's commit and carry no buffer damage, so
# the damage-driven FBO cache must re-render on the changed stacking order.
#
# Scenario (build/test-restack, an SSD client that never sets app_id):
#   1. attach main buffer + damage         -> re-renders
#   2. add two subsurfaces + parent commit -> re-renders
#   3. place_below(sub2, parent) + commit  -> MUST re-render (order changed)
#
# The compositor logs every render as "rounded: published FBO for app_id..."
# (WLR_DEBUG); exactly 3 renders must appear.  Fewer means the undamaged
# restack was skipped (stale FBO).
#
# Usage: ./test/test-restack.sh   (from the repo root, after cmake --build build)
set -u
cd "$(dirname "$0")/.."

RUNDIR=$(mktemp -d /tmp/wmrs.XXXXXX)
trap 'rm -rf "$RUNDIR"' EXIT
mkdir -p "$RUNDIR/config"

XDG_CONFIG_HOME="$RUNDIR/config" XDG_RUNTIME_DIR="$RUNDIR" \
  WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_DEBUG=1 \
  ./build/monodywm > "$RUNDIR/wm.log" 2>&1 &
WM=$!
sleep 2

XDG_RUNTIME_DIR="$RUNDIR" WAYLAND_DISPLAY=wayland-0 \
  ./build/test-restack > "$RUNDIR/client.log" 2>&1
RC=$?
sleep 0.3
kill "$WM" 2>/dev/null
wait "$WM" 2>/dev/null

if [ "$RC" -ne 0 ]; then
    echo "FAIL: test client failed (rc=$RC)"
    cat "$RUNDIR/client.log"
    exit 1
fi

RENDERS=$(grep -c "rounded: published FBO for app_id" "$RUNDIR/wm.log")
echo "renders: $RENDERS (expected 3)"
if [ "$RENDERS" -ne 3 ]; then
    echo "FAIL: expected exactly 3 renders (undamaged restack must re-render)"
    grep "rounded: published FBO for app_id" "$RUNDIR/wm.log"
    exit 1
fi

echo "PASS: undamaged subsurface restack re-rendered"
exit 0
