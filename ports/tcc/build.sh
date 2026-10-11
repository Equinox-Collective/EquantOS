#!/bin/sh
# Builds static tcc (Tiny C Compiler, mob @ d348a9a, see MOB_PIN) for EquantOS.
#
# Like ports/st, this must run on Alpine Linux (static musl toolchain):
#   - Inside Alpine / WSL: sh ports/tcc/build.sh
#   - Or on Linux host with bubblewrap: auto-delegates into /tmp/alpine-rootfs
#
# EquantOS target layout baked into the binary (disk paths first, LiveCD
# fallbacks second; all are colon-separated search lists):
#   tccdir      /lib/tcc            (libtcc1.a, tcc's own include/)
#   sysinclude  /usr/include : /cdrom/usr/include
#   libpaths    /lib : /cdrom/lib   (libc.a)
#   crtprefix   /lib : /cdrom/lib   (crt1.o, crti.o, crtn.o)
#
# Output: res/tcc.elf, res/tcc/libtcc1.a
# (tcc headers + musl headers/libc are copied at ISO build time from
#  ports/tcc/src/include and sdk/sysroot, see Makefile.)
set -e

PORT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$PORT_DIR/../.." && pwd)
SRC="$PORT_DIR/src"

# If running on non-Alpine host with bwrap and /tmp/alpine-rootfs available, run inside it
if [ ! -f /etc/alpine-release ] && command -v bwrap >/dev/null 2>&1 && [ -d /tmp/alpine-rootfs ]; then
    echo ">>> Running tcc build inside Alpine rootfs (/tmp/alpine-rootfs) via bubblewrap..."
    exec bwrap --bind /tmp/alpine-rootfs / \
               --bind "$REPO" "$REPO" \
               --dev /dev \
               --proc /proc \
               --tmpfs /tmp \
               --uid 0 --gid 0 \
               /bin/sh "$PORT_DIR/build.sh" "$@"
fi

if [ -f /etc/alpine-release ]; then
    apk add --no-progress -q build-base 2>/dev/null || true
fi

echo ">>> Configuring tcc for EquantOS..."
cd "$SRC"
if [ ! -f config.h ]; then
    ./configure \
        --config-musl \
        --cc=gcc \
        --extra-cflags="-O2 -static" \
        --extra-ldflags="-static" \
        --prefix=/usr \
        --tccdir=/lib/tcc \
        --sysincludepaths=/usr/include:/cdrom/usr/include \
        --libpaths=/lib:/cdrom/lib \
        --crtprefix=/lib:/cdrom/lib
fi

echo ">>> Building tcc..."
make

if ! file tcc | grep -q 'statically linked'; then
    echo "error: tcc is not statically linked" >&2
    exit 1
fi
if [ ! -f libtcc1.a ]; then
    echo "error: libtcc1.a was not built" >&2
    exit 1
fi

echo ">>> Installing to res/..."
mkdir -p "$REPO/res/tcc"
strip -o "$REPO/res/tcc.elf" tcc
cp libtcc1.a "$REPO/res/tcc/libtcc1.a"
echo "Successfully built:"
file "$REPO/res/tcc.elf"
ls -la "$REPO/res/tcc/libtcc1.a"
echo "Baked paths:"
"$REPO/res/tcc.elf" -vv 2>&1 | head -12 || true
