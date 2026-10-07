#!/bin/bash
# EquantOS Automated X11 Desktop & IceWM Bootstrap

echo -e "\033[1;36m[EQUANT-X11] Initializing X Window System & IceWM...\033[0m"

if [ -f /bin/bash ]; then
    /bin/busybox.elf cp /bin/bash /bin/sh 2>/dev/null
    /bin/busybox.elf chmod +x /bin/sh 2>/dev/null
fi

# 1. Kill stale instances
/bin/busybox.elf killall icewm.elf 2>/dev/null
/bin/busybox.elf killall twm.elf 2>/dev/null
/bin/busybox.elf killall Xfbdev.elf 2>/dev/null
/bin/busybox.elf killall xeyes.elf 2>/dev/null
/bin/busybox.elf sleep 1

# 2. Prepare directories & configs
/bin/busybox.elf mkdir -p /tmp/.X11-unix 2>/dev/null
/bin/busybox.elf mkdir -p /etc/fonts 2>/dev/null
/bin/busybox.elf mkdir -p /usr/share/fonts 2>/dev/null
/bin/busybox.elf mkdir -p /etc/icewm 2>/dev/null
/bin/busybox.elf mkdir -p /.icewm 2>/dev/null
/bin/busybox.elf mkdir -p /root/.icewm 2>/dev/null
/bin/busybox.elf mkdir -p /usr/share/icewm 2>/dev/null
/bin/busybox.elf mkdir -p /.icewm/themes/ 2>/dev/null
/bin/busybox.elf mkdir -p /usr/share/icewm/themes/ 2>/dev/null

export ICEWM_PRIVCFG=/.icewm
export HOME=/

# Copy theme directory
if [ -d /cdrom/Icewm_MinimalDark ]; then
    /bin/busybox.elf cp -r /cdrom/Icewm_MinimalDark /.icewm/themes/ 2>/dev/null
    /bin/busybox.elf cp -r /cdrom/Icewm_MinimalDark /usr/share/icewm/themes/ 2>/dev/null
elif [ -d /bin/Icewm_MinimalDark ]; then
    /bin/busybox.elf cp -r /bin/Icewm_MinimalDark /.icewm/themes/ 2>/dev/null
    /bin/busybox.elf cp -r /bin/Icewm_MinimalDark /usr/share/icewm/themes/ 2>/dev/null
fi

# Setup Fontconfig & TrueType font
if [ -f /bin/fonts.conf ]; then
    /bin/busybox.elf cp /bin/fonts.conf /etc/fonts/fonts.conf 2>/dev/null
fi

if [ -f /bin/font.ttf ]; then
    /bin/busybox.elf cp /bin/font.ttf /usr/share/fonts/font.ttf 2>/dev/null
fi

# Setup IceWM preferences, menu, programs and toolbar
for dir in /.icewm /etc/icewm /etc/X11/icewm /usr/share/icewm; do
    if [ -f /bin/preferences ]; then
        /bin/busybox.elf tr -d '\r' < /bin/preferences > "$dir/preferences" 2>/dev/null
    fi
    if [ -f /bin/menu ]; then
        /bin/busybox.elf tr -d '\r' < /bin/menu > "$dir/menu" 2>/dev/null
        /bin/busybox.elf tr -d '\r' < /bin/menu > "$dir/programs" 2>/dev/null
        /bin/busybox.elf tr -d '\r' < /bin/menu > "$dir/toolbar" 2>/dev/null
    fi
    echo "Theme=\"Icewm_MinimalDark/default.theme\"" > "$dir/theme"
done

/bin/busybox.elf rm -f /tmp/.X0-lock /tmp/.tX0-lock /tmp/.X11-unix/X0 2>/dev/null

# 3. Check framebuffer device
if [ ! -e /dev/fb0 ]; then
    echo -e "\033[1;31m[ERROR] /dev/fb0 not found!\033[0m"
    exit 1
fi

# 4. Launch Xfbdev Server
echo -e "\033[1;34m[EQUANT-X11] Starting Xfbdev on :0 (1600x900x32 TrueColor)...\033[0m"
/bin/Xfbdev.elf :0 -screen 1600x900x32 -ac -nolock -fp built-ins -mouse /dev/mouse,3 &
X_PID=$!

/bin/busybox.elf sleep 1

# 5. Set Environment
export DISPLAY=:0
export FONTCONFIG_PATH=/etc/fonts
export FONTCONFIG_FILE=/etc/fonts/fonts.conf
echo -e "\033[1;36m[EQUANT-X11] DISPLAY set to :0\033[0m"

# 6. Launch IceWM Window Manager with Dark Theme
if [ -f /bin/icewm.elf ]; then
    echo -e "\033[1;32m[EQUANT-X11] Spawning IceWM Window Manager...\033[0m"
    /bin/icewm.elf --theme Icewm_MinimalDark/default.theme &
fi

echo -e "\033[1;32m[EQUANT-X11] Desktop environment is active.\033[0m"