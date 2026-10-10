#!/bin/sh
# Builds a static NetSurf 3.11 (framebuffer frontend drawing into an X11 window
# through libnsfb's xcb surface) for EquantOS and packs it as netsurf.epkg.
#
# EquantOS runs static x86_64 Linux/musl binaries, so this script must run on
# Alpine Linux (the same toolchain family that produced res/icewm.elf and
# res/Xfbdev.elf). On Windows an Alpine minirootfs imported into WSL works:
#   wsl --import EquantBuild <dir> alpine-minirootfs-<ver>-x86_64.tar.gz
#   wsl -d EquantBuild -- sh /mnt/d/<repo>/ports/netsurf/build.sh
#
# Output: $WORK/out/netsurf.epkg (tar for epacmg) and $WORK/out/root/ (its tree)
set -e

PORT_DIR=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-/root/ns}
PREFIX=$WORK/sysroot
SRC=$WORK/src
OUT=$WORK/out
JOBS=$(nproc)

NETSURF_VER=3.11
CURL_VER=8.22.0
XAU_VER=1.0.12
XDMCP_VER=1.1.5
XCB_UTIL_VER=0.4.1
XCB_WM_VER=0.4.2

apk add --no-progress -q build-base coreutils perl bison flex gperf pkgconf xz wget tar \
    linux-headers openssl-dev openssl-libs-static zlib-dev zlib-static \
    libpng-dev libpng-static libjpeg-turbo-dev libjpeg-turbo-static \
    libwebp-dev libwebp-static freetype-dev freetype-static \
    brotli-static bzip2-static libxcb-dev libxcb-static libxdmcp-dev \
    xcb-proto xorgproto util-macros font-dejavu

mkdir -p "$SRC" "$PREFIX/lib/pkgconfig" "$OUT" "$WORK/bin"

# Every pkg-config query must return the full static dependency closure
cat > "$WORK/bin/pkg-config" <<'EOF'
#!/bin/sh
exec /usr/bin/pkgconf --static "$@"
EOF
chmod 755 "$WORK/bin/pkg-config"
export PATH="$WORK/bin:$PATH"

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export CFLAGS="-O2 -fno-pie -fno-pic"
export LDFLAGS="-L$PREFIX/lib -static -no-pie"

fetch() { # url
    f=$SRC/$(basename "$1")
    [ -f "$f" ] || wget -q -T 30 -t 5 -O "$f" "$1" || { rm -f "$f"; exit 1; }
    echo "$f"
}

autotools_lib() { # name url [configure args...]
    name=$1; url=$2; shift 2
    [ -f "$PREFIX/.stamp-$name" ] && return 0
    echo ">>> $name"
    tarball=$(fetch "$url")
    dir=$SRC/$(basename "$tarball" | sed 's/\.tar\..*//')
    rm -rf "$dir" && tar -C "$SRC" -xf "$tarball"
    (cd "$dir" && ./configure --prefix="$PREFIX" --enable-static --disable-shared "$@" >/dev/null \
        && make -j"$JOBS" >/dev/null && make install >/dev/null)
    touch "$PREFIX/.stamp-$name"
}

# --- X11 client libraries Alpine does not ship as static archives ---------
autotools_lib libXau "https://www.x.org/releases/individual/lib/libXau-$XAU_VER.tar.xz"
# Alpine's libXdmcp.a wants libbsd's arc4random_buf; build one that does not
autotools_lib libXdmcp "https://www.x.org/releases/individual/lib/libXdmcp-$XDMCP_VER.tar.xz" \
    ac_cv_search_arc4random_buf=no ac_cv_func_arc4random_buf=no
# xcb.pc from Alpine lists xau; our static libXau.a lives in $PREFIX
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig"
autotools_lib xcb-util "https://xcb.freedesktop.org/dist/xcb-util-$XCB_UTIL_VER.tar.xz"
autotools_lib xcb-util-image "https://xcb.freedesktop.org/dist/xcb-util-image-$XCB_UTIL_VER.tar.xz"
autotools_lib xcb-util-keysyms "https://xcb.freedesktop.org/dist/xcb-util-keysyms-$XCB_UTIL_VER.tar.xz"
autotools_lib xcb-util-wm "https://xcb.freedesktop.org/dist/xcb-util-wm-$XCB_WM_VER.tar.xz"
# libnsfb still asks for the pre-0.3.8 "xcb-atom" module, now part of xcb-util
[ -f "$PREFIX/lib/pkgconfig/xcb-atom.pc" ] || \
    sed 's/^Name:.*/Name: XCB Atom (xcb-util)/' "$PREFIX/lib/pkgconfig/xcb-util.pc" \
        > "$PREFIX/lib/pkgconfig/xcb-atom.pc"

# --- libcurl: HTTP(S) only, blocking getaddrinfo (no resolver threads) ------
autotools_lib curl "https://curl.se/download/curl-$CURL_VER.tar.xz" \
    --with-openssl --with-zlib --without-brotli --without-zstd \
    --without-libpsl --without-libidn2 --without-nghttp2 --without-nghttp3 \
    --without-ngtcp2 --without-libssh2 --without-librtmp --disable-ldap \
    --disable-threaded-resolver --disable-ipv6 --disable-unix-sockets \
    --disable-dict --disable-ftp --disable-gopher --disable-imap --disable-mqtt \
    --disable-pop3 --disable-rtsp --disable-smb --disable-smtp --disable-telnet \
    --disable-tftp --disable-docs --disable-manual \
    --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --with-ca-path=/etc/ssl/certs

# --- NetSurf and its libraries ---------------------------------------------
NSDIR=$SRC/netsurf-all-$NETSURF_VER
if [ ! -d "$NSDIR" ]; then
    tar -C "$SRC" -xf "$(fetch "https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-$NETSURF_VER.tar.gz")"
    for p in "$PORT_DIR"/patches/*.patch; do
        [ -f "$p" ] && patch -d "$NSDIR" -p1 < "$p"
    done
fi
cp "$PORT_DIR/Makefile.config" "$NSDIR/netsurf/Makefile.config"

echo ">>> netsurf"
make -C "$NSDIR" -j"$JOBS" TARGET=framebuffer PREFIX=/usr

# --- package tree: /usr/bin/netsurf + resources + fonts + CA bundle ---------
ROOT=$OUT/root
rm -rf "$ROOT"
mkdir -p "$ROOT/usr/bin" "$ROOT/usr/share/netsurf" "$ROOT/usr/share/fonts/truetype/dejavu" \
         "$ROOT/etc/ssl/certs"
cp "$NSDIR/netsurf/nsfb" "$ROOT/usr/bin/netsurf"
strip "$ROOT/usr/bin/netsurf"
# resources: follow symlinks so the tar only holds regular files (epacmg extracts no links)
cp -rL "$NSDIR/netsurf/frontends/framebuffer/res/." "$ROOT/usr/share/netsurf/"
for f in DejaVuSans DejaVuSans-Bold DejaVuSans-Oblique DejaVuSans-BoldOblique \
         DejaVuSerif DejaVuSerif-Bold DejaVuSansMono DejaVuSansMono-Bold; do
    cp "/usr/share/fonts/dejavu/$f.ttf" "$ROOT/usr/share/fonts/truetype/dejavu/"
done
cp /etc/ssl/certs/ca-certificates.crt "$ROOT/etc/ssl/certs/"
cp "$PORT_DIR/netsurf.sh" "$ROOT/usr/bin/netsurf-x"
chmod 755 "$ROOT/usr/bin/netsurf-x"

# epacmg's tar reader only handles plain ustar names (<100 chars, no prefix field)
(cd "$ROOT" && find . -type f | sed 's|^\./||' | sort > "$OUT/filelist" \
    && tar --format=ustar --owner=0 --group=0 -cf "$OUT/netsurf.epkg" -T "$OUT/filelist")
echo "built $OUT/netsurf.epkg ($(wc -c < "$OUT/netsurf.epkg") bytes)"
