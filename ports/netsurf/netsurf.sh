#!/bin/sh
# Starts NetSurf in a window on the running X server (see /start.sh).
# usage: netsurf-x [url]      (default: the start page, /usr/share/netsurf/start.html)
export DISPLAY=${DISPLAY:-:0}
export HOME=${HOME:-/}
mkdir -p "$HOME/.netsurf" 2>/dev/null
if [ -n "$1" ]; then
    exec /usr/bin/netsurf -f x -w 1280 -h 820 "$1"
fi
exec /usr/bin/netsurf -f x -w 1280 -h 820
