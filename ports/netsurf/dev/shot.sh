#!/bin/sh
# Development helper: runs the freshly built NetSurf under Xvfb inside the
# Alpine build distro and saves screenshots, so pages can be checked without
# booting EquantOS.
#   wsl -d EquantBuild -- sh ports/netsurf/dev/shot.sh <url> <out.png> [wait_s] [WxH]
# Environment:
#   XDO    xdotool commands run after the first wait (clicks, typing); a second
#          screenshot <out>-2.png is taken WAIT2 seconds later
#   NSLOG  where NetSurf's stderr goes (default /tmp/netsurf.log)
set -e
URL=${1:-about:welcome}
OUT=${2:-/tmp/shot.png}
WAIT=${3:-8}
GEOM=${4:-1280x900}
WORK=${WORK:-/root/ns}
BIN=${NSBIN:-$WORK/src/netsurf-all-3.11/netsurf/nsfb}
RES=${NSRES:-$WORK/src/netsurf-all-3.11/netsurf/frontends/framebuffer/res}
DPY=${DPY:-:77}
N=${DPY#:}

W=${GEOM%x*}; H=${GEOM#*x}
if pkill -f "Xvfb $DPY" 2>/dev/null; then sleep 1; fi
rm -f /tmp/.X$N-lock /tmp/.X11-unix/X$N
Xvfb $DPY -screen 0 ${W}x${H}x24 -nolisten tcp -ac >/dev/null 2>&1 &
XPID=$!
for i in $(seq 1 50); do
    [ -e /tmp/.X11-unix/X$N ] && break
    sleep 0.1
done
export DISPLAY=$DPY HOME=${NSHOME:-/root/nshome} NETSURFRES=$RES
mkdir -p "$HOME/.netsurf"
"$BIN" -f x -w "$W" -h "$H" $NSARGS "$URL" >"${NSLOG:-/tmp/netsurf.log}" 2>&1 &
NPID=$!
sleep "$WAIT"
xwd -root -silent -display $DPY | magick xwd:- "$OUT"
echo "saved $OUT"
if [ -n "$XDO" ]; then
    eval "xdotool $XDO" || true
    sleep "${WAIT2:-$WAIT}"
    xwd -root -silent -display $DPY | magick xwd:- "${OUT%.png}-2.png"
    echo "saved ${OUT%.png}-2.png"
fi
kill $NPID 2>/dev/null || true
sleep 0.3
kill $XPID 2>/dev/null || true
wait 2>/dev/null || true
