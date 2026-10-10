#!/bin/sh
# Development helper: regenerates ../patches/equantos.patch from the edited
# NetSurf tree in the build distro, by comparing the files listed below with
# the release tarball. (Whole new files live in ../overlay/ instead.)
#   wsl -d EquantBuild -- sh ports/netsurf/dev/mkpatch.sh
set -e
PORT_DIR=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-/root/ns}
VER=3.11
TREE=$WORK/src/netsurf-all-$VER
PRISTINE=$WORK/pristine

PATCHED="
libhubbub/src/treebuilder/element-type.gperf
netsurf/content/Makefile
netsurf/content/handlers/html/box_special.c
netsurf/content/handlers/html/html.c
netsurf/content/handlers/html/interaction.c
netsurf/content/handlers/html/private.h
netsurf/content/handlers/image/image.c
netsurf/frontends/framebuffer/fbtk/text.c
netsurf/frontends/framebuffer/fbtk/widget.h
netsurf/frontends/framebuffer/gui.c
netsurf/resources/default.css
"

if [ ! -d "$PRISTINE/netsurf-all-$VER" ]; then
    mkdir -p "$PRISTINE"
    tar -C "$PRISTINE" -xf "$WORK/src/netsurf-all-$VER.tar.gz"
fi

mkdir -p "$PORT_DIR/patches"
OUT=$PORT_DIR/patches/equantos.patch
: > "$OUT"
for f in $PATCHED; do
    diff -u --label "a/$f" --label "b/$f" "$PRISTINE/netsurf-all-$VER/$f" "$TREE/$f" >> "$OUT" || true
done
echo "wrote $OUT: $(grep -c '^+++ ' "$OUT") files, $(wc -l < "$OUT") lines"
