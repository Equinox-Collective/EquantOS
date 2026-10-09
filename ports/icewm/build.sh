#!/bin/sh
# Builds static IceWM 4.1.0 for EquantOS: the window manager plus icewmbg
# (desktop backgrounds) and icesh, with PNG, JPEG, XPM images and Xft
# text, and installs them as res/icewm.elf, res/icewmbg.elf, res/icesh.elf.
# The stock IceWM themes are copied to res/icewm-themes/.
#
# Like ports/netsurf this must run on Alpine Linux (static musl toolchain):
#   wsl -d EquantBuild -- sh /mnt/d/<repo>/ports/icewm/build.sh
set -e

PORT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$PORT_DIR/../.." && pwd)
WORK=${WORK:-/root/icewm-build}
PREFIX=$WORK/sysroot
SRC=$WORK/src
JOBS=$(nproc)

ICEWM_VER=4.1.0
XORG=https://www.x.org/releases/individual/lib

apk add --no-progress -q build-base coreutils perl pkgconf xz lzip wget tar \
    linux-headers util-macros xorgproto xtrans xcb-proto \
    libx11-dev libx11-static libx11 libxext-dev libxext-static libxcb-dev libxcb-static \
    libice-dev libice-static fontconfig-dev fontconfig-static \
    freetype-dev freetype-static expat-static libpng-dev libpng-static \
    libjpeg-turbo-dev libjpeg-turbo-static zlib-static bzip2-static brotli-static \
    fribidi-dev fribidi-static

mkdir -p "$SRC" "$PREFIX/lib/pkgconfig" "$WORK/bin"

# Every pkg-config query must return the full static dependency closure
cat > "$WORK/bin/pkg-config" <<'EOF'
#!/bin/sh
exec /usr/bin/pkgconf --static "$@"
EOF
chmod 755 "$WORK/bin/pkg-config"
export PATH="$WORK/bin:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig"
export CFLAGS="-O2 -fno-pie -fno-pic"
export CXXFLAGS="-O2 -fno-pie -fno-pic"
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
autotools_lib libXau        $XORG/libXau-1.0.12.tar.xz
# Alpine's libXdmcp.a wants libbsd's arc4random_buf
autotools_lib libXdmcp      $XORG/libXdmcp-1.1.5.tar.xz \
    ac_cv_search_arc4random_buf=no ac_cv_func_arc4random_buf=no
autotools_lib libXrender    $XORG/libXrender-0.9.12.tar.xz
autotools_lib libXfixes     $XORG/libXfixes-6.0.1.tar.xz
autotools_lib libXdamage    $XORG/libXdamage-1.1.6.tar.xz
autotools_lib libXcomposite $XORG/libXcomposite-0.4.6.tar.xz
autotools_lib libXcursor    $XORG/libXcursor-1.2.3.tar.xz
autotools_lib libXrandr     $XORG/libXrandr-1.5.4.tar.xz
autotools_lib libXft        $XORG/libXft-2.3.9.tar.xz
# cxpm/sxpm would pull in libintl: build them without gettext
autotools_lib libXpm        $XORG/libXpm-3.5.17.tar.xz --disable-open-zfile ac_cv_search_gettext=no
autotools_lib libSM         $XORG/libSM-1.2.6.tar.xz --without-libuuid

# --- IceWM -----------------------------------------------------------------
ICEDIR=$SRC/icewm-$ICEWM_VER
if [ ! -f "$PREFIX/.stamp-icewm" ]; then
    echo ">>> icewm"
    rm -rf "$ICEDIR"
    tar -C "$SRC" --lzip -xf "$(fetch "https://github.com/ice-wm/icewm/releases/download/$ICEWM_VER/icewm-$ICEWM_VER.tar.lz")"
    (cd "$ICEDIR" && ./configure --prefix=/usr --sysconfdir=/etc \
        --disable-shared --disable-nls --disable-imlib2 --disable-gdk-pixbuf \
        --disable-librsvg --disable-xinerama --disable-xres \
        --disable-menus-fdo --with-icesound=none \
        --with-theme="Icewm_MinimalDark/default.theme" \
        --with-xterm=/bin/bash >/dev/null \
        && grep -E '^#define CONFIG_(LIBJPEG|LIBPNG|XPM|XFREETYPE)' config.h \
        && make -j"$JOBS" -C src icewm icewmbg icesh LDFLAGS="$LDFLAGS -all-static"             LIBS="$(pkg-config --libs x11 xext xrender)" >/dev/null)
    touch "$PREFIX/.stamp-icewm"
fi

for p in icewm icewmbg icesh; do
    # libtool only honours -all-static; a dynamic binary cannot run on EquantOS
    file "$ICEDIR/src/$p" | grep -q 'statically linked' || { echo "error: $p is not static" >&2; exit 1; }
    strip -o "$REPO/res/$p.elf" "$ICEDIR/src/$p"
    file "$REPO/res/$p.elf"
done

# Stock themes next to the bundled Icewm_MinimalDark
rm -rf "$REPO/res/icewm-themes"
mkdir -p "$REPO/res/icewm-themes"
cp -r "$ICEDIR/lib/themes/." "$REPO/res/icewm-themes/"
find "$REPO/res/icewm-themes" -name 'Makefile*' -delete

# Xlib locale database (C and UTF-8 only): without it Xlib rejects every locale
# and IceWM falls back to Latin-1, so Cyrillic window titles turn into garbage
XL=$REPO/res/X11-locale
rm -rf "$XL"
mkdir -p "$XL/C" "$XL/en_US.UTF-8"
cp /usr/share/X11/locale/locale.alias /usr/share/X11/locale/locale.dir \
   /usr/share/X11/locale/compose.dir "$XL/"
cp /usr/share/X11/locale/C/XLC_LOCALE /usr/share/X11/locale/C/XI18N_OBJS "$XL/C/"
cp /usr/share/X11/locale/en_US.UTF-8/XLC_LOCALE /usr/share/X11/locale/en_US.UTF-8/XI18N_OBJS \
   "$XL/en_US.UTF-8/"
echo "installed res/icewm.elf, res/icewmbg.elf, res/icesh.elf, res/icewm-themes/ and res/X11-locale/"
