#!/bin/sh
# Grab-shortcut regression test: with the input method holding the keyboard
# grab (cursor in a text field, fcitx5 composing), a global shortcut
# (Ctrl+Alt+P -> rofi) must still fire and the consumed key must not be
# forwarded to the IM grab.
#
# Usage: ./test/test-grab-shortcut.sh   (from the repo root, after cmake --build build)
set -u
cd "$(dirname "$0")/.."

RUNDIR=$(mktemp -d /tmp/wmgrab.XXXXXX)
trap 'kill "$WM" 2>/dev/null; rm -rf "$RUNDIR"' EXIT
mkdir -p "$RUNDIR/config"

XDG_CONFIG_HOME="$RUNDIR/config" XDG_RUNTIME_DIR="$RUNDIR" \
  WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_DEBUG=1 \
  ./build/monodywm > "$RUNDIR/wm.log" 2>&1 &
WM=$!
sleep 2

XDG_RUNTIME_DIR="$RUNDIR" WAYLAND_DISPLAY=wayland-0 \
  ./build/test-grab-shortcut
RC=$?

echo "---"
if [ "$RC" -eq 0 ]; then
    echo "PASS: shortcut fired during IM grab; consumed key withheld from IM"
else
    echo "FAIL (see output above)"
    grep -E "spawn:|ime: connecting" "$RUNDIR/wm.log" | tail -5
fi
exit "$RC"
