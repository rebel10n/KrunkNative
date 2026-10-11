#!/bin/bash
# nat.sh start|stop|shot <png>|x <xdotool args...>
# Own Xvfb :77 because it drives the client with xdotool; for xdotool-free captures use
# KRUNKNATIVE_HEADLESS=1 + KRUNKNATIVE_INPUT_SCRIPT instead (client/include/headless.h).
export DISPLAY=:77
case "$1" in
  start)
    pgrep -f "Xvfb :77" >/dev/null || { nohup setsid Xvfb :77 -screen 0 1280x720x24 >/dev/null 2>&1 </dev/null & sleep 2; }
    cd /home/user/src/KrunkNative
    mkdir -p ${KN_CFG:-/tmp/kn-menus-cfg}; nohup setsid env XDG_CONFIG_HOME=${KN_CFG:-/tmp/kn-menus-cfg} KRUNKNATIVE_NO_BROWSER=1 XDG_DOWNLOAD_DIR=${KN_OUT:-/tmp/kn-menus-ref} LIBGL_ALWAYS_SOFTWARE=1 stdbuf -oL ${KN_BIN:-/tmp/kn-menus/client_bin} --offline ${2:-} > ${KN_OUT:-/tmp/kn-menus-ref}/native.log 2>&1 &
    echo $! > ${KN_OUT:-/tmp/kn-menus-ref}/native.pid
    sleep ${WAIT:-10}
    ;;
  stop) kill $(cat ${KN_OUT:-/tmp/kn-menus-ref}/native.pid) 2>/dev/null; sleep 0.5 ;;
  shot) import -window root "$2" ;;
  x) shift; xdotool "$@" ;;
esac
