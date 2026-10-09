#!/bin/sh
# Starts NetSurf in a window on the running X server (see /start.sh).
# usage: netsurf-x [url]      (default: the bundled welcome page)
export DISPLAY=${DISPLAY:-:0}
export HOME=${HOME:-/}
mkdir -p "$HOME/.netsurf" 2>/dev/null
exec /usr/bin/netsurf -f x -w 1200 -h 800 "${1:-about:welcome}"
