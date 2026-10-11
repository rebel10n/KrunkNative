#!/bin/bash
# Captures every pose in world-poses.txt (or the names given) from KrunkNative and, unless
# NOREF=1, from the real game, into $OUT (default /tmp/kn-world-out):
#   <name>-native.png, <name>-krunker.png, <name>-sbs.png (50% side by side)
# A pose's optional 4th column is a v3.1.1 settings JSON (e.g. {"lighting":2}): KrunkNative
# gets it as its stored settings, the real game as localStorage kro_setngss_* in a separate
# Chromium profile (CDP port 7774), so it never leaks into the other captures.
# Env: BUILD (KrunkNative build dir), see world-native.sh / world-ref.js for the rest.
cd "$(dirname "$0")" || exit 1
OUT=${OUT:-/tmp/kn-world-out}; mkdir -p "$OUT"
for n in ${@:-$(awk '!/^#/ && NF {print $1}' world-poses.txt)}; do
  set -- $(grep "^$n " world-poses.txt)
  name=$1 map=$2 pose=$3 settings=$4
  if [ -n "$settings" ]; then
    cfg=/tmp/kn-world-cfg-$name; mkdir -p "$cfg/krunknative"; echo "$settings" > "$cfg/krunknative/knSettings.json"
    CFG=$cfg ./world-native.sh "$map" "$pose" "$OUT/$name-native.png" ${BUILD:+"$BUILD"} > /dev/null
    [ -n "$NOREF" ] || CDP_PORT=7774 CHROME_PROFILE=/tmp/kn-world-ref/chrome-$name KR_SETTINGS="$settings" \
      timeout 200 node world-ref.js "$map" "$pose" "$OUT/$name-krunker.png" || echo "real-game capture failed: $name"
  else
    ./world-native.sh "$map" "$pose" "$OUT/$name-native.png" ${BUILD:+"$BUILD"} > /dev/null
    [ -n "$NOREF" ] || timeout 200 node world-ref.js "$map" "$pose" "$OUT/$name-krunker.png" || echo "real-game capture failed: $name"
  fi
  if [ -f "$OUT/$name-krunker.png" ]; then
    magick "$OUT/$name-native.png" -resize 50% /tmp/kn-wc-a.png
    magick "$OUT/$name-krunker.png" -resize 50% /tmp/kn-wc-b.png
    magick /tmp/kn-wc-a.png /tmp/kn-wc-b.png +append "$OUT/$name-sbs.png"
  fi
done
