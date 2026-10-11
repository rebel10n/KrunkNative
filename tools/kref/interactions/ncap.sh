#!/bin/bash
# ncap.sh name [x y]...  -- clicks each point (hover first), shoots, then Escape if CLOSE=1 (default)
export DISPLAY=:77
name=$1; shift
while [ $# -ge 2 ]; do xdotool mousemove $1 $2; sleep 0.25; xdotool click 1; sleep 0.5; shift 2; done
xdotool mousemove ${PARK:-1270 380}; sleep ${WAIT:-0.9}
import -window root ${KN_OUT:-/tmp/kn-menus-ref}/ours/$name.png
if [ "${CLOSE:-1}" = 1 ]; then xdotool key Escape; sleep 0.4; fi
