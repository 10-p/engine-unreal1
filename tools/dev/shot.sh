#!/usr/bin/env bash
# shot.sh -- run the native SoftDrv build headlessly under Xvfb and grab a frame.
#
#   tools/dev/shot.sh <Map.unr> <out.png> [seconds] [extra args...]
#
# Xvfb's -fbdir dump is used instead of the engine's SHOT command: SHOT needs a keypress,
# and there is no key-injection tool on this box.
set -u

MAP="${1:?usage: shot.sh <Map.unr> <out.png> [seconds] [args...]}"
OUT="${2:?}"
SECS="${3:-20}"
shift 3 || true

RUN=${U1RUN:-/tmp/u1soft}
DISP=${U1DISP:-:99}
FBDIR=$(mktemp -d)
RES=${U1RES:-800x600}

Xvfb "$DISP" -screen 0 "${RES}x24" -fbdir "$FBDIR" >/dev/null 2>&1 &
XPID=$!
sleep 1

( cd "$RUN/System" && DISPLAY="$DISP" SDL_VIDEODRIVER=x11 \
    exec ./Unreal.bin "$MAP" -log "$@" >/tmp/u1run.log 2>&1 ) &
GPID=$!	# exec: the subshell BECOMES the game, so the kill below reaches it (ufront 2.84; it used to leave every game running)

sleep "$SECS"

cp "$FBDIR/Xvfb_screen0" /tmp/u1fb.xwd 2>/dev/null
kill "$GPID" 2>/dev/null; wait "$GPID" 2>/dev/null
kill "$XPID" 2>/dev/null; wait "$XPID" 2>/dev/null

if [ -s /tmp/u1fb.xwd ]; then
    ffmpeg -y -loglevel error -i /tmp/u1fb.xwd "$OUT" && echo "wrote $OUT"
else
    echo "no framebuffer dump" >&2
fi
rm -rf "$FBDIR"
tail -5 /tmp/u1run.log
