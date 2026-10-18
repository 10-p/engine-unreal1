#!/usr/bin/env bash
# botmatch.sh -- a headless native bot match: a dedicated server (`Unreal.bin -server`) and one client that joins
# it. Prints one summary line.
#
#   tools/dev/botmatch.sh <server-run-tree> <seconds> [<client-run-tree>] [<map>]
#
# Bots: a v200 server is bot-free unless the server tree's Unreal.ini sets the game's globalconfig
# bMultiPlayerBots=True (and InitialBots=<n>) in the section of the class that declares them. The server port is
# that ini's [URL] Port. The client may be another run tree, e.g. a 32-bit client joining a 64-bit server. Output
# lands in <server-run-tree>/botmatch/.
#
# v200 writes its log only to a file (System/<exe>.log), so server and client from one tree would share it: each
# gets its own with LOG=<file>. The timeout runs inside the wrappers (xvfb-run, script): `timeout xvfb-run ...`
# kills the wrapper and leaves the game running. Console output is 4 KB block-buffered and stdbuf's preload
# library is 64-bit only, so every game runs on a pty (`script -f`) and prints line by line at either width. A
# game is stopped by its own command line, never by a regex over a map name (DM-Deck16][ is not a valid regex).
# A sanitizer report counts as a crash marker (an ASan abort in the client otherwise looks like a player leaving).
set -eu
SRV=$(readlink -f "${1:?usage: botmatch.sh <server-run-tree> <seconds> [<client-run-tree>] [<map>]}")
SECS=${2:?}
CLI=$(readlink -f "${3:-$SRV}")
MAP=${4:-DmDeck16}
OUT=$SRV/botmatch; rm -rf "$OUT"; mkdir -p "$OUT"
PORT=$(sed -n '/^\[URL\]/,/^\[/{s/^Port=\([0-9]*\).*/\1/p}' "$SRV/System/Unreal.ini" | head -1)
PORT=${PORT:-7777}
# A game saves its config on exit, and -NULLRENDERER would save NullDrv as the renderer: keep the trees' ini files
# here and restore them afterwards.
mkdir -p "$OUT/ini/srv" "$OUT/ini/cli"
cp -p "$SRV"/System/*.ini "$OUT/ini/srv/"; cp -p "$CLI"/System/*.ini "$OUT/ini/cli/"
cd "$SRV/System"
rm -f botmatch-server.log
script -qfc "timeout $((SECS+60)) ./Unreal.bin '$MAP?game=UnrealI.DeathMatchGame' -server LOG=botmatch-server.log" "$OUT/server.pty" > /dev/null 2>&1 &
SPID=$!
for _ in $(seq 1 90); do grep -qa "Game engine initialized" botmatch-server.log 2>/dev/null && break; sleep 1; done
cd "$CLI/System"
rm -f botmatch-client.log
xvfb-run -a -s "-screen 0 1024x768x24" script -qfc "timeout $((SECS+30)) ./Unreal.bin 127.0.0.1:$PORT -NULLRENDERER LOG=botmatch-client.log" "$OUT/client.pty" > /dev/null 2>&1 &
CPID=$!
sleep "$SECS"
# Stop the games, not the wrappers: xvfb-run then shuts its own X server down.
pkill -TERM -f "^./Unreal.bin 127.0.0.1:$PORT " 2>/dev/null || true
pkill -TERM -f "^./Unreal.bin .*-server LOG=botmatch-server.log$" 2>/dev/null || true
wait 2>/dev/null || true
cp -p "$OUT"/ini/srv/*.ini "$SRV/System/"; cp -p "$OUT"/ini/cli/*.ini "$CLI/System/"
# v200 logs to a file only (unbuffered, so a killed game's log is complete); each process names its own.
cp "$SRV/System/botmatch-server.log" "$OUT/server.log"; cp "$CLI/System/botmatch-client.log" "$OUT/client.log"
BAD='SIGSEGV|Signal|Assertion failed|appError|Critical|General protection|Segmentation fault|\[layout\]|ERROR: AddressSanitizer|runtime error:'
echo "server=$(file -b "$SRV/System/Unreal.bin" | cut -d, -f1-2) client=$(file -b "$CLI/System/Unreal.bin" | cut -d, -f1-2)"
echo "joined=$(grep -ac 'Join succeeded' "$OUT/server.log") maps=$(grep -ac 'LoadMap:' "$OUT/server.log") client_maps=$(grep -ac 'LoadMap:' "$OUT/client.log") accessed_none=$(grep -ac 'Accessed None' "$OUT/server.log") warnings=$(grep -aci 'warning' "$OUT/server.log") crash_markers=$(grep -acE "$BAD" "$OUT/server.log" "$OUT/client.log" "$OUT/server.pty" "$OUT/client.pty" | awk -F: '{s+=$2} END{print s}')"	# sanitizer reports go to stderr (the pty)
