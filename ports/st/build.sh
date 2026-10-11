#!/bin/sh
# Builds static st (suckless simple terminal) for EquantOS.
#
# Like ports/icewm and ports/netsurf, this must run on Alpine Linux (static musl toolchain):
#   - Inside Alpine / WSL: sh ports/st/build.sh
#   - Or on Linux host with bubblewrap: auto-delegates into /tmp/alpine-rootfs
#
# Output: res/st.elf
set -e

PORT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$PORT_DIR/../.." && pwd)
SRC="$PORT_DIR/src"

# If running on non-Alpine host with bwrap and /tmp/alpine-rootfs available, run inside it
if [ ! -f /etc/alpine-release ] && command -v bwrap >/dev/null 2>&1 && [ -d /tmp/alpine-rootfs ]; then
    echo ">>> Running st build inside Alpine rootfs (/tmp/alpine-rootfs) via bubblewrap..."
    exec bwrap --bind /tmp/alpine-rootfs / \
               --bind "$REPO" "$REPO" \
               --dev /dev \
               --proc /proc \
               --tmpfs /tmp \
               --uid 0 --gid 0 \
               /bin/sh "$PORT_DIR/build.sh" "$@"
fi

if [ -f /etc/alpine-release ]; then
    apk add --no-progress -q build-base pkgconf \
        libx11-dev libx11-static libxft-dev fontconfig-dev fontconfig-static \
        freetype-dev freetype-static expat-static libpng-static \
        zlib-static bzip2-static brotli-static libxcb-static 2>/dev/null || true
fi

echo ">>> Building st..."
cd "$SRC"

# Re-generate config.h from config.def.h
rm -f config.h st *.o
cp config.def.h config.h

STATIC_LIBS="$(pkgconf --static --libs x11 xft fontconfig freetype2) -lm -lrt -lutil"

make CC=gcc \
     CFLAGS="-O2 -fno-pie -fno-pic" \
     LDFLAGS="-static -no-pie" \
     LIBS="$STATIC_LIBS" \
     st

if ! file st | grep -q 'statically linked'; then
    echo "error: st is not statically linked" >&2
    exit 1
fi

strip -o "$REPO/res/st.elf" st
echo "Successfully built and installed $(file "$REPO/res/st.elf")"
