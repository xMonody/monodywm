#!/bin/sh
# Rounded-mask re-render guard: the mask pass must only run when the commit
# actually changed the content.
#
# Scenario (build/test-mask-guard, an SSD client that never sets app_id):
#   1. attach buffer A (200x100) + damage   -> mask renders
#   2. state-only commit (no attach, no damage) -> MUST NOT render
#   3. re-attach buffer A + damage          -> mask renders (damage)
#   4. attach buffer B (240x120)            -> mask renders (new buffer)
#
# The compositor logs every render as "rounded: published FBO for app_id..." (WLR_DEBUG);
# exactly 3 renders must appear.  A 4th line means the state-only commit
# re-rendered; fewer means a damaged/new-buffer commit was skipped.
#
# Usage: ./test/test-mask-guard.sh   (from the repo root, after cmake --build build)
set -u
cd "$(dirname "$0")/.."

RUNDIR=$(mktemp -d /tmp/wmmg.XXXXXX)
trap 'rm -rf "$RUNDIR"' EXIT
mkdir -p "$RUNDIR/config"

XDG_CONFIG_HOME="$RUNDIR/config" XDG_RUNTIME_DIR="$RUNDIR" \
  WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_DEBUG=1 \
  ./build/monodywm > "$RUNDIR/wm.log" 2>&1 &
WM=$!
sleep 2

XDG_RUNTIME_DIR="$RUNDIR" WAYLAND_DISPLAY=wayland-0 \
  ./build/test-mask-guard > "$RUNDIR/client.log" 2>&1
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
echo "mask renders: $RENDERS (expected 3)"
if [ "$RENDERS" -ne 3 ]; then
    echo "FAIL: expected exactly 3 mask renders (state-only commit must be skipped)"
    grep "rounded: published FBO for app_id" "$RUNDIR/wm.log"
    exit 1
fi

echo "PASS: state-only commit skipped, buffer/damage commits re-rendered"
exit 0
