#!/bin/bash
# usage: native-shot.sh <name> [extra xdotool commands after spawn, separated by ';']
# Stays on its own Xvfb: it drives the client with xdotool, which KRUNKNATIVE_HEADLESS=1
# (no window, EGL; client/include/headless.h) cannot take. Never point it at DISPLAY=:0.
name=$1; shift; extra="$*"
cd /home/user/src/KrunkNative/cmake-build-debug || exit 1
xvfb-run -a -s "-screen 0 1280x720x24" bash -c '
  LIBGL_ALWAYS_SOFTWARE=1 stdbuf -oL ./client_bin --offline > /tmp/krunker-ref/native-'"$name"'.log 2>&1 & pid=$!
  sleep 9
  xdotool mousemove 640 330 click 1
  sleep 3
  if [ -n "'"$extra"'" ]; then eval "'"$extra"'"; fi
  import -window root /tmp/krunker-ref/ours-native-'"$name"'.png
  kill $pid; wait $pid 2>/dev/null; true'
echo "ours-native-$name: $(identify -format '%wx%h' /tmp/krunker-ref/ours-native-$name.png 2>/dev/null)"; grep -ciE 'error|fail' /tmp/krunker-ref/native-$name.log
