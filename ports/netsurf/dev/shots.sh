#!/bin/sh
# Development helper: copies the overlay (site scripts) into the built tree
# and takes one screenshot per "<name> <url>" line read from stdin.
#   wsl -d EquantBuild -- sh ports/netsurf/dev/shots.sh <outdir> [wait_s] [WxH] < list.txt
set -e
PORT_DIR=$(cd "$(dirname "$0")/.." && pwd)
OUTDIR=${1:-/tmp}
WAIT=${2:-9}
GEOM=${3:-1280x900}
cp -r "$PORT_DIR/overlay/." "${WORK:-/root/ns}/src/netsurf-all-3.11/"
while read -r name url; do
    [ -n "$name" ] || continue
    sh "$PORT_DIR/dev/shot.sh" "$url" "$OUTDIR/$name.png" "$WAIT" "$GEOM" >/dev/null
    grep -a "sitejs\|nsvideo: failed" "${NSLOG:-/tmp/netsurf.log}" | head -n 5 || true
    echo "$name done"
done
