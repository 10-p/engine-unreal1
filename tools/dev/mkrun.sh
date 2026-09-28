#!/usr/bin/env bash
# mkrun.sh -- assemble a scratch run tree for the native build.
#
#   tools/dev/mkrun.sh <build-dir> <run-dir>
#   tools/dev/mkrun.sh build-soft /tmp/u1soft
#
# Symlinks the read-only asset trees, copies the config files (the engine rewrites them), and
# installs the freshly built modules.
#
# NOTE: --remove-destination on every copy is load-bearing. The System entries are symlinks into
# $UNREAL_ASSETS_DIR, and a plain `cp` FOLLOWS a symlink and writes straight through into the asset
# tree. Do not drop it.
set -eu

BUILD="${1:?usage: mkrun.sh <build-dir> <run-dir>}"
RUN="${2:?}"
A="${UNREAL_ASSETS_DIR:-/home/ion/unreal-assets}"

rm -rf "$RUN"
mkdir -p "$RUN/System"

for d in Maps Textures Sounds Music Help; do
    [ -d "$A/$d" ] && ln -sfn "$A/$d" "$RUN/$d"
done
# Save and Cache are written by the engine (save games, downloaded packages), so they are the run tree's own
# (ufront 2.84: they were symlinks, and a test's save game or download went straight into the asset tree). The
# asset tree's saves are copied so they stay loadable.
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
chmod +x "$RUN/System/Unreal.bin"

echo "$RUN ready: $(ls "$RUN"/System/*.so | wc -l) modules from $BUILD"
