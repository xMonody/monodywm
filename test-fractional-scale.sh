#!/bin/sh
# Fractional-scale pixel-alignment test for monodywm.
#
# Ports the client half of Wayfire's scaling test (PR #3032 / commit
# 35f8f2ca7a) to monodywm: a client binds wp_fractional_scale_v1 +
# wp_viewporter, attaches a physical-size checkerboard for a logical size,
# captures the output through wlr-screencopy (raw physical pixels) and checks
# that the checkerboard is sampled 1:1 (no blended pixels).
#
# Usage: ./test-fractional-scale.sh [scale]      (default 1.75)
#   From the repo root after `cmake -S . -B build-test -DTEST=ON &&
#   cmake --build build-test`.
set -u
cd "$(dirname "$0")"

BUILD=${BUILD:-build-test}
SCALE=${1:-1.75}
RUNDIR=$(mktemp -d /tmp/wmscale.XXXXXX)
WM=

cleanup() {
    [ -n "$WM" ] && kill "$WM" 2>/dev/null
    rm -rf "$RUNDIR"
}
trap cleanup EXIT
mkdir -p "$RUNDIR/config"

XDG_CONFIG_HOME="$RUNDIR/config" XDG_RUNTIME_DIR="$RUNDIR" \
  WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=gles2 \
  EGL_PLATFORM=surfaceless \
  "./$BUILD/monodywm" > "$RUNDIR/wm.log" 2>&1 &
WM=$!
sleep 2

if ! XDG_RUNTIME_DIR="$RUNDIR" wlr-randr --output HEADLESS-1 --scale "$SCALE" \
        > "$RUNDIR/randr.log" 2>&1; then
    echo "FAIL: wlr-randr could not set scale $SCALE"
    cat "$RUNDIR/randr.log"
    exit 1
fi

# The client prints READY, captures, analyses and exits on its own.
echo "scale=$SCALE"
XDG_RUNTIME_DIR="$RUNDIR" WAYLAND_DISPLAY=wayland-0 \
  "./$BUILD/test-fractional-scale"
RC=$?

echo "---"
if [ "$RC" -eq 0 ]; then
    echo "PASS (scale $SCALE)"
else
    echo "FAIL (scale $SCALE, client rc=$RC)"
    tail -5 "$RUNDIR/wm.log"
fi
exit "$RC"
