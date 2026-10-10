#!/bin/bash
# EquantOS X11 desktop: Xfbdev + IceWM (+ icewmbg for the wallpaper)

BB=/bin/busybox.elf
MEDIA=/cdrom
[ -d "$MEDIA/Icewm_MinimalDark" ] || MEDIA=/bin

echo -e "\033[1;36m[EQUANT-X11] Initializing X Window System & IceWM...\033[0m"

if [ -f /bin/bash ]; then
    $BB cp /bin/bash /bin/sh 2>/dev/null
    $BB chmod +x /bin/sh 2>/dev/null
fi

# 1. Stop a previous session
$BB killall icewm.elf icewmbg.elf twm.elf xeyes.elf Xfbdev.elf 2>/dev/null
$BB sleep 1

# 2. Directories and environment
$BB mkdir -p /tmp/.X11-unix /etc/fonts /usr/share/fonts /etc/icewm /.icewm \
    /usr/share/icewm/themes /usr/share/X11 2>/dev/null

export HOME=/
export ICEWM_PRIVCFG=/.icewm
export LANG=C.UTF-8
export FONTCONFIG_PATH=/etc/fonts
export FONTCONFIG_FILE=/etc/fonts/fonts.conf

# Themes: the bundled MinimalDark plus IceWM's stock themes (Themes menu)
for t in "$MEDIA/Icewm_MinimalDark" "$MEDIA"/icewm-themes/*; do
    [ -d "$t" ] || continue
    name=${t##*/}
    [ -d "/usr/share/icewm/themes/$name" ] || $BB cp -r "$t" /usr/share/icewm/themes/ 2>/dev/null
done

# Xlib locale database: needed for UTF-8 (Cyrillic) window titles and menus
if [ -d "$MEDIA/X11-locale" ] && [ ! -d /usr/share/X11/locale ]; then
    $BB cp -r "$MEDIA/X11-locale" /usr/share/X11/locale 2>/dev/null
fi

# Fontconfig & TrueType font (NetSurf adds DejaVu under /usr/share/fonts too)
[ -f /bin/fonts.conf ] && $BB cp /bin/fonts.conf /etc/fonts/fonts.conf 2>/dev/null
[ -f /bin/font.ttf ] && $BB cp /bin/font.ttf /usr/share/fonts/font.ttf 2>/dev/null

# IceWM configuration: system defaults are refreshed, the chosen theme is kept
for dir in /etc/icewm "$ICEWM_PRIVCFG"; do
    for f in preferences menu toolbar; do
        [ -f "/bin/$f" ] && $BB tr -d '\r' < "/bin/$f" > "$dir/$f" 2>/dev/null
    done
    [ -f /bin/menu ] && $BB tr -d '\r' < /bin/menu > "$dir/programs" 2>/dev/null
done
[ -f /etc/icewm/theme ] || echo 'Theme="Icewm_MinimalDark/default.theme"' > /etc/icewm/theme
[ -f "$ICEWM_PRIVCFG/theme" ] || echo 'Theme="Icewm_MinimalDark/default.theme"' > "$ICEWM_PRIVCFG/theme"

$BB rm -f /tmp/.X0-lock /tmp/.tX0-lock /tmp/.X11-unix/X0 2>/dev/null

# 3. Check framebuffer device
if [ ! -e /dev/fb0 ]; then
    echo -e "\033[1;31m[ERROR] /dev/fb0 not found!\033[0m"
    exit 1
fi

# 4. Launch Xfbdev Server
echo -e "\033[1;34m[EQUANT-X11] Starting Xfbdev on :0 (1600x900x32 TrueColor)...\033[0m"
/bin/Xfbdev.elf :0 -screen 1600x900x32 -ac -nolock -fp built-ins -mouse /dev/mouse,3 &

# Wait for the X socket instead of guessing how long startup takes
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    [ -e /tmp/.X11-unix/X0 ] && break
    $BB sleep 0.25 2>/dev/null || $BB sleep 1
done

export DISPLAY=:0
echo -e "\033[1;36m[EQUANT-X11] DISPLAY set to :0\033[0m"

# 5. Wallpaper (theme's DesktopBackgroundImage) and window manager
[ -f /bin/icewmbg.elf ] && /bin/icewmbg.elf &
if [ -f /bin/icewm.elf ]; then
    echo -e "\033[1;32m[EQUANT-X11] Spawning IceWM Window Manager...\033[0m"
    /bin/icewm.elf &
fi

echo -e "\033[1;32m[EQUANT-X11] Desktop environment is active.\033[0m"
