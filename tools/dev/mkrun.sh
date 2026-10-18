#!/usr/bin/env bash
# mkrun.sh -- assemble a scratch run tree for the native build.
#
#   UE_ASSETS_DIR=<game folder> tools/dev/mkrun.sh <build-dir> <run-dir>
#   UE_ASSETS_DIR=~/games/Unreal tools/dev/mkrun.sh build-native /tmp/u1
#
# Symlinks the read-only asset trees from $UE_ASSETS_DIR (an Unreal v200 install: Maps, System, ...), gives the
# run tree its own Save and Cache, copies the config files (the engine rewrites them) and installs the built
# modules.
#
# Every copy into System uses --remove-destination: the System entries are symlinks into
# $UE_ASSETS_DIR, and a plain `cp` follows a symlink and writes through into the asset tree.
set -eu

BUILD="${1:?usage: mkrun.sh <build-dir> <run-dir>}"
RUN="${2:?}"
A="${UE_ASSETS_DIR:?set UE_ASSETS_DIR to the Unreal game folder}"

rm -rf "$RUN"
mkdir -p "$RUN/System"

for d in Maps Textures Sounds Music Help; do
    [ -d "$A/$d" ] && ln -sfn "$A/$d" "$RUN/$d"
done
# Save and Cache are written by the engine (save games, downloaded packages), so they are the run tree's own;
# the asset tree's saves are copied so they stay loadable.
mkdir -p "$RUN/Cache"
if [ -d "$A/Save" ]; then cp -a "$A/Save" "$RUN/Save"; else mkdir -p "$RUN/Save"; fi

# Packages and the like: symlinks are fine, nothing writes to them.
for f in "$A"/System/*; do
    case "$(basename "$f")" in
        *.log) continue ;;   # the engine appends to this; keep it out of the asset tree
    esac
    ln -sfn "$f" "$RUN/System/$(basename "$f")"
done

# Config: real copies -- the engine writes these back on exit.
for f in "$A"/System/*.ini "$A"/System/*.int; do
    [ -e "$f" ] && cp -f --remove-destination "$f" "$RUN/System/"
done

# Our build wins over whatever the asset tree happens to hold.
find "$BUILD" -name '*.so' -not -path '*_deps*' -exec \
    cp -f --remove-destination {} "$RUN/System/" \;
cp -f --remove-destination "$BUILD/Unreal/Unreal.bin" "$RUN/System/"
# The bundled openal-soft, which the binary finds beside itself ($ORIGIN).
if [ -e "$BUILD/Unreal/libopenal.so.1" ]; then
    cp -f --remove-destination "$BUILD/Unreal/libopenal.so.1" "$RUN/System/"
fi
chmod +x "$RUN/System/Unreal.bin"

echo "$RUN ready: $(ls "$RUN"/System/*.so | wc -l) modules from $BUILD"
