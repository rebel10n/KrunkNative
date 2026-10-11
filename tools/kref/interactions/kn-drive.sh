#!/bin/bash
# kn-drive.sh: drive a running client_bin through KRUNKNATIVE_UI_CMDS (client/src/debug_capture.c),
# the native counterpart of capture-real.js's Runtime.evaluate. No X server needed (headless too).
#
#   kn-drive.sh start [client args...]   start client_bin (KN_BIN) headless or on DISPLAY, wait for it
#   kn-drive.sh cmd '<line>'...          append command lines (ui showWindow(14) / click x y / key 256 / con ...)
#   kn-drive.sh shot <out.png> [wait]    request a frame capture and wait until it is written
#   kn-drive.sh stop
# Env: KN_BIN (client_bin), KN_RUN (run dir: cmds file, log, pid; default /tmp/kn-drive),
#      KN_CFG (XDG_CONFIG_HOME, default $KN_RUN/cfg), KN_HEADLESS=1 + KN_EGL=nvidia|mesa,
#      KN_SIZE=1280x720, KN_WAIT seconds after start (default 12).
set -u
RUN=${KN_RUN:-/tmp/kn-drive}; CMDS=$RUN/cmds; mkdir -p "$RUN"
case "${1:-}" in
  start)
    shift
    : > "$CMDS"
    cfg=${KN_CFG:-$RUN/cfg}; mkdir -p "$cfg" "$RUN/state"
    envs=(XDG_CONFIG_HOME="$cfg" XDG_STATE_HOME="$RUN/state" KRUNKNATIVE_NO_BROWSER=1 KRUNKNATIVE_UI_CMDS="$CMDS"
          KRUNKNATIVE_WINDOW_SIZE="${KN_SIZE:-1280x720}")
    if [ "${KN_HEADLESS:-1}" = 1 ]; then
      envs+=(KRUNKNATIVE_HEADLESS=1 KRUNKNATIVE_EGL_DEVICE="${KN_EGL:-mesa}")
      cd "${KN_ROOT:-/home/user/src/KrunkNative}" && nohup setsid env -u DISPLAY -u WAYLAND_DISPLAY "${envs[@]}" \
        stdbuf -oL "${KN_BIN:?KN_BIN}" "$@" > "$RUN/log" 2>&1 < /dev/null &
    else
      [ "${DISPLAY:-:0}" = ":0" ] && { echo "refusing DISPLAY=:0"; exit 1; }
      cd "${KN_ROOT:-/home/user/src/KrunkNative}" && nohup setsid env LIBGL_ALWAYS_SOFTWARE=1 "${envs[@]}" \
        stdbuf -oL "${KN_BIN:?KN_BIN}" "$@" > "$RUN/log" 2>&1 < /dev/null &
    fi
    echo $! > "$RUN/pid"; sleep "${KN_WAIT:-12}" ;;
  cmd) shift; for l in "$@"; do printf '%s\n' "$l" >> "$CMDS"; done ;;
  shot)
    rm -f "$2"; printf 'shot %s\n' "$2" >> "$CMDS"
    for _ in $(seq 1 $(( ${3:-20} * 10 ))); do [ -s "$2" ] && break; sleep 0.1; done
    [ -s "$2" ] || echo "shot $2: timed out" ;;
  stop) kill "$(cat "$RUN/pid")" 2>/dev/null; sleep 0.5 ;;
  *) sed -n 2,12p "$0" ;;
esac
