#!/bin/sh
# Builds a static fastfetch for EquantOS against the musl SDK in sdk/sysroot
# and installs it as res/fastfetch.elf (picked up by the main Makefile).
#
# usage: sh ports/fastfetch/build.sh        (run from the repository root)
# needs: git, cmake, make, python3, x86_64-elf-gcc, sdk/sysroot/lib/crt{1,i,n}.o
set -e

FASTFETCH_VERSION=2.69.0
FASTFETCH_REPO=https://github.com/fastfetch-cli/fastfetch.git

ROOT=$(pwd)
PORT_DIR="$ROOT/ports/fastfetch"
SRC_DIR="$ROOT/build/ports/fastfetch-src"
BUILD_DIR="$ROOT/build/ports/fastfetch-build"

for f in crt1.o crti.o crtn.o; do
    if [ ! -f "$ROOT/sdk/sysroot/lib/$f" ]; then
        echo "error: sdk/sysroot/lib/$f is missing (see README, 'NOTE!' section)" >&2
        exit 1
    fi
done

if [ ! -d "$SRC_DIR" ]; then
    mkdir -p "$ROOT/build/ports"
    git -c core.autocrlf=false clone --depth 1 --branch "$FASTFETCH_VERSION" "$FASTFETCH_REPO" "$SRC_DIR"
    for p in "$PORT_DIR"/patches/*.patch; do
        echo "applying $(basename "$p")"
        git -C "$SRC_DIR" apply --whitespace=nowarn "$p"
    done
fi

# Native Windows CMake (Git Bash / MSYS2) needs Windows paths and the MinGW generator
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        GENERATOR="MinGW Makefiles"
        TOOLCHAIN=$(cygpath -m "$PORT_DIR/equantos-toolchain.cmake")
        MAKE_ARG="-DCMAKE_MAKE_PROGRAM=$(cygpath -m "$(command -v make)")"
        ;;
    *)
        GENERATOR="Unix Makefiles"
        TOOLCHAIN="$PORT_DIR/equantos-toolchain.cmake"
        MAKE_ARG=""
        ;;
esac

# EquantOS has none of the optional runtime libraries, and static musl cannot dlopen anyway
DISABLED="VULKAN WAYLAND XCB_RANDR XRANDR DRM VADRM VAX11 VDPAU GIO DCONF EET DBUS SQLITE3 RPM \
EGL GLX OPENCL FREETYPE PULSE DDCUTIL ELF THREADS IMAGE_LOGO IMAGEMAGICK7 IMAGEMAGICK6 SIXEL \
CHAFA ZLIB LTO LUA QUICKJS LIBZFS WORDEXP"
OPTS=""
for o in $DISABLED; do
    OPTS="$OPTS -DENABLE_$o=OFF"
done

cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G "$GENERATOR" $MAKE_ARG \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBINARY_LINK_TYPE=static \
    -DIS_MUSL=ON \
    -DBUILD_FLASHFETCH=OFF \
    -DBUILD_TESTS=OFF \
    -DSET_TWEAK=OFF \
    $OPTS

cmake --build "$BUILD_DIR" --target fastfetch -j "$(nproc 2>/dev/null || echo 4)"

x86_64-elf-strip -o "$ROOT/res/fastfetch.elf" "$BUILD_DIR/fastfetch"
echo "installed res/fastfetch.elf ($(wc -c < "$ROOT/res/fastfetch.elf") bytes)"
