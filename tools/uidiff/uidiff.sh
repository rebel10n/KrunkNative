#!/usr/bin/env bash
# uidiff.sh - per-region pixel diff of the native HTML UI against the Krunker
# v3.1.1 reference captures (doc/ref/krunker-v3.1.1/).
#
#   tools/uidiff/uidiff.sh <client_bin> [scenario-name...]
#
# For every tools/uidiff/scenarios/<name>.json `client_bin --render-scenario`
# renders the scenario to a PNG (client/src/ui/ui_scenario.c; under its own
# xvfb-run + llvmpipe, or headless), then every region of
# tools/uidiff/regions/<name>.json is cropped from the reference and the render
# and compared with `compare -metric AE -fuzz <fuzz>%`. A region passes when the
# percentage of differing pixels is <= its max_pct. Regions with "skip": true
# are reported but never fail (3D scene, animation phases).
#
# A region with "mask": "ink" compares only "ink" pixels: pixels that differ
# from their local background (a mask_kernel x mask_kernel median of the same
# image) by more than mask_threshold % in the reference OR in the render. This
# keeps opaque text/icons and drops the flat translucent boxes around them,
# which an in-game scenario (reference PNG as background) blends twice. The
# share is then differing ink pixels / ink pixels.
# "expect": "background" compares the render against the scenario's flat
# background colour instead of the reference (regions that must stay empty).
# "ref_offset": [dx, dy] crops the reference at rect + (dx, dy) and the render at
# rect, for content a capture artifact moved in the reference.
# "mask": "colors" does the same with a colour key instead: ink = pixels within
# mask_color_fuzz % of one of mask_colors (the opaque text/fill colours of the
# element) in the reference OR the render. Use it for opaque flat fills (health
# bar segments) that a median would swallow.
#
# Outputs (per scenario, in $UIDIFF_OUT):
#   <name>.png        the render
#   <name>.log        the renderer's stdout/stderr
#   <name>.diff.png   compare's visual (differences in red) with the region
#                     rectangles drawn on top (green PASS, red FAIL, gray SKIP)
#   <name>.sbs.png    reference | render side by side
#   <name>.regions/   per-region crops (ref-, render-, diff-<region>.png;
#                     mask-<region>.png for ink-masked regions)
#
# Environment:
#   UIDIFF_OUT         output dir (default cmake-build-debug/uidiff)
#   UIDIFF_FUZZ_SCALE  multiply every region fuzz by this factor (default 1)
#   UIDIFF_RENDER_CMD  replaces "<client_bin> --render-scenario" (and its xvfb-run): run as
#                      `$UIDIFF_RENDER_CMD <scenario.json> <out.png>` with
#                      UIDIFF_REF/UIDIFF_W/UIDIFF_H/UIDIFF_SCENARIO exported;
#                      the literal value `cp` copies the reference as the
#                      render (pipeline self-test)
#   UIDIFF_NO_XVFB=1   run the renderer without xvfb-run, on the inherited DISPLAY
#                      (already inside an Xvfb; never the user's desktop)
#   UIDIFF_HEADLESS=1  render with no window and no X server at all: the renderer runs
#                      with KRUNKNATIVE_HEADLESS=1 (EGL device context, client/include/
#                      headless.h) on Mesa llvmpipe, pixel-identical to the Xvfb path
#   UIDIFF_EGL_DEVICE  nvidia|mesa|<index>: headless on that EGL device (implies
#                      UIDIFF_HEADLESS=1); nvidia reproduces NVIDIA-only driver bugs
#   UIDIFF_XVFB_ARGS   extra arguments for xvfb-run (default: -a)
#
# Exit status: 0 when every non-skipped region of every scenario passes,
# 1 when any region fails or a render is missing/has the wrong size,
# 2 on usage/tool errors.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
cd "$repo"

usage() {
    echo "usage: $0 <client_bin> [scenario-name...]" >&2
    exit 2
}

[ $# -ge 1 ] || usage
client_bin=$1
shift

out_dir=${UIDIFF_OUT:-cmake-build-debug/uidiff}
fuzz_scale=${UIDIFF_FUZZ_SCALE:-1}
render_cmd=${UIDIFF_RENDER_CMD:-}
egl_device=${UIDIFF_EGL_DEVICE:-}
if [ -z "$egl_device" ] && [ "${UIDIFF_HEADLESS:-0}" = 1 ]; then
    egl_device=mesa
fi
# Headless renders must never reach a display: drop DISPLAY/WAYLAND_DISPLAY for them.
headless_env=()
if [ -n "$egl_device" ]; then
    headless_env=(env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 "KRUNKNATIVE_EGL_DEVICE=$egl_device")
fi
mkdir -p "$out_dir"

# ImageMagick 7 ships `magick`; 6 only has convert/compare.
if command -v magick >/dev/null 2>&1; then
    im_convert() { magick "$@"; }
    im_compare() { magick compare "$@"; }
else
    for t in convert compare; do
        command -v "$t" >/dev/null 2>&1 || { echo "uidiff: ImageMagick ($t) not found" >&2; exit 2; }
    done
    im_convert() { convert "$@"; }
    im_compare() { compare "$@"; }
fi
command -v python3 >/dev/null 2>&1 || { echo "uidiff: python3 not found" >&2; exit 2; }
# Default renderer: client_bin --render-scenario, each scenario in its own xvfb-run
# unless headless or UIDIFF_NO_XVFB=1.
own_xvfb=0
if [ -z "$render_cmd" ]; then
    [ -x "$client_bin" ] || { echo "uidiff: client_bin not executable: $client_bin (set UIDIFF_RENDER_CMD to override)" >&2; exit 2; }
    if [ "${UIDIFF_NO_XVFB:-0}" != 1 ] && [ -z "$egl_device" ]; then
        command -v xvfb-run >/dev/null 2>&1 || { echo "uidiff: xvfb-run not found (or set UIDIFF_HEADLESS=1)" >&2; exit 2; }
        own_xvfb=1
    fi
fi

json_py="$here/uidiff_json.py"

# image_size <png> -> "w h"
image_size() {
    im_convert "$1" -format '%w %h\n' info: 2>/dev/null || true
}

# render_scenario <name> <scenario.json> <ref> <w> <h> <out.png>
render_scenario() {
    local name=$1 scenario=$2 ref=$3 w=$4 h=$5 out=$6
    rm -f "$out"
    if [ "$render_cmd" = cp ]; then
        cp "$ref" "$out"
    elif [ -n "$render_cmd" ]; then
        # shellcheck disable=SC2086  # UIDIFF_RENDER_CMD is a command line
        UIDIFF_REF=$ref UIDIFF_W=$w UIDIFF_H=$h UIDIFF_SCENARIO=$name \
            "${headless_env[@]}" $render_cmd "$scenario" "$out"
    elif [ "$own_xvfb" = 1 ]; then
        # shellcheck disable=SC2086  # UIDIFF_XVFB_ARGS is a list of options
        xvfb-run ${UIDIFF_XVFB_ARGS:--a} -s "-screen 0 ${w}x${h}x24" \
            env LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe "$client_bin" --render-scenario "$scenario" "$out"
    else
        "${headless_env[@]}" LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe "$client_bin" --render-scenario "$scenario" "$out"
    fi
}

# pct <ae> <w> <h> -> percentage with 3 decimals
pct() {
    awk -v ae="$1" -v w="$2" -v h="$3" 'BEGIN { printf "%.3f", ((w * h) > 0 ? 100.0 * ae / (w * h) : 100.0) }'
}

# scaled_fuzz <fuzz> -> fuzz * UIDIFF_FUZZ_SCALE
scaled_fuzz() {
    awk -v f="$1" -v s="$fuzz_scale" 'BEGIN { printf "%g", f * s }'
}

# le <a> <b> -> true when a <= b (floats)
le() {
    awk -v a="$1" -v b="$2" 'BEGIN { exit !(a + 0 <= b + 0) }'
}

# ink_mask <in.png> <kernel> <threshold%> <out.png>: white where the pixel differs
# from its local median background by more than threshold (max over channels).
ink_mask() {
    im_convert "$1" \( +clone -statistic Median "${2}x${2}" \) -compose difference -composite \
        -separate -evaluate-sequence max -threshold "${3}%" "$4"
}

# color_mask <in.png> <fuzz%> <colors,comma,separated> <out.png>: white where the
# pixel is within fuzz of one of the colours.
color_mask() {
    local in=$1 fz=$2 cols=$3 out=$4 c
    local args=("$in" -alpha off)
    IFS=, read -r -a clist <<<"$cols"
    for c in "${clist[@]}"; do
        args+=(\( -clone 0 -fuzz "${fz}%" -fill black +opaque "$c" -fill white -opaque "$c" \))
    done
    args+=(-delete 0 -background black -compose lighten -flatten -colorspace gray "$out")
    im_convert "${args[@]}"
}

# white_count <binary.png> -> number of white pixels
white_count() {
    im_convert "$1" -format '%[fx:round(mean*w*h)]\n' info: 2>/dev/null || echo 0
}

# masked_diff <ref> <render> <fuzz%> <mode> <kernel> <threshold%> <colors> <color-fuzz%> <mask-out> <diff-out>
#   -> "differing-ink-pixels ink-pixels"
masked_diff() {
    local a=$1 b=$2 fz=$3 mode=$4 k=$5 t=$6 cols=$7 cfz=$8 mask=$9 diff=${10}
    if [ "$mode" = colors ]; then
        color_mask "$a" "$cfz" "$cols" "$mask.a.png"
        color_mask "$b" "$cfz" "$cols" "$mask.b.png"
    else
        ink_mask "$a" "$k" "$t" "$mask.a.png"
        ink_mask "$b" "$k" "$t" "$mask.b.png"
    fi
    im_convert "$mask.a.png" "$mask.b.png" -compose lighten -composite "$mask"
    rm -f "$mask.a.png" "$mask.b.png"
    im_convert "$a" "$b" -compose difference -composite -separate -evaluate-sequence max \
        -threshold "${fz}%" "$mask" -compose multiply -composite "$diff"
    echo "$(white_count "$diff") $(white_count "$mask")"
}

scenarios=()
if [ $# -gt 0 ]; then
    for n in "$@"; do
        n=${n%.json}
        n=${n##*/}
        scenarios+=("$n")
    done
else
    for f in "$here"/scenarios/*.json; do
        [ -e "$f" ] || continue
        n=${f##*/}
        scenarios+=("${n%.json}")
    done
fi
[ ${#scenarios[@]} -gt 0 ] || { echo "uidiff: no scenarios" >&2; exit 2; }

total=0
failed=0
skipped=0
passed=0
failed_scenarios=()

printf '%-22s %-20s %9s %9s %6s %s\n' SCENARIO REGION 'DIFF%' 'MAX%' FUZZ RESULT
printf '%-22s %-20s %9s %9s %6s %s\n' ---------------------- -------------------- --------- --------- ------ ------

for name in "${scenarios[@]}"; do
    scenario="$here/scenarios/$name.json"
    regions="$here/regions/$name.json"
    if [ ! -f "$scenario" ]; then
        echo "uidiff: scenario not found: $scenario" >&2
        exit 2
    fi
    if [ ! -f "$regions" ]; then
        echo "uidiff: regions not found: $regions" >&2
        exit 2
    fi
    IFS=$'\t' read -r ref w h background _frames < <(python3 -I "$json_py" scenario "$scenario") || true
    if [ ! -f "$ref" ]; then
        echo "uidiff: $name: reference missing: $ref" >&2
        exit 2
    fi
    read -r ref_w ref_h < <(image_size "$ref")
    if [ "$ref_w" != "$w" ] || [ "$ref_h" != "$h" ]; then
        echo "uidiff: $name: reference $ref is ${ref_w}x${ref_h}, scenario size is ${w}x${h}" >&2
    fi

    render="$out_dir/$name.png"
    scenario_abs=$(cd "$(dirname "$scenario")" && pwd)/$(basename "$scenario")
    render_abs=$(cd "$(dirname "$render")" && pwd)/$(basename "$render")
    scenario_failed=0
    if ! render_scenario "$name" "$scenario_abs" "$ref" "$w" "$h" "$render_abs" >"$out_dir/$name.log" 2>&1 \
        || [ ! -s "$render" ]; then
        printf '%-22s %-20s %9s %9s %6s %s\n' "$name" '(render)' - - - "FAIL (no render, see $out_dir/$name.log)"
        tail -n 5 "$out_dir/$name.log" >&2 || true
        failed=$((failed + 1)); total=$((total + 1)); failed_scenarios+=("$name")
        continue
    fi
    read -r rw rh < <(image_size "$render")
    if [ "$rw" != "$w" ] || [ "$rh" != "$h" ]; then
        printf '%-22s %-20s %9s %9s %6s %s\n' "$name" '(render)' - - - "FAIL (render is ${rw}x${rh}, want ${w}x${h})"
        failed=$((failed + 1)); total=$((total + 1)); failed_scenarios+=("$name")
        continue
    fi

    crop_dir="$out_dir/$name.regions"
    mkdir -p "$crop_dir"
    draw_ops=()
    while IFS=$'\t' read -r rname rx ry rwid rhgt rfuzz rmax rskip rmask rkernel rthresh rcolors rcfuzz rdx rdy rexpect; do
        [ -n "$rname" ] || continue
        total=$((total + 1))
        fuzz=$(scaled_fuzz "$rfuzz")
        geom="${rwid}x${rhgt}+${rx}+${ry}"
        ref_crop="$crop_dir/ref-$rname.png"
        ren_crop="$crop_dir/render-$rname.png"
        diff_crop="$crop_dir/diff-$rname.png"
        # ref_offset [dx, dy]: the reference crop is taken at rect + offset (a capture
        # artifact moved that content; the render is always cropped at rect).
        ref_geom="${rwid}x${rhgt}+$((rx + ${rdx:-0}))+$((ry + ${rdy:-0}))"
        if [ "${rexpect:-reference}" = background ]; then
            # "expect": "background": nothing may be drawn here; the render must show the
            # scenario's flat background colour.
            if [[ $background =~ ^#[0-9a-fA-F]{6}$ ]]; then
                im_convert -size "${rwid}x${rhgt}" "xc:$background" "$ref_crop"
            else
                echo "uidiff: $name/$rname: expect=background needs a #rrggbb scenario background" >&2
                im_convert -size "${rwid}x${rhgt}" xc:'#ff00ff' "$ref_crop"
            fi
        else
            im_convert "$ref" -crop "$ref_geom" +repage "$ref_crop"
        fi
        im_convert "$render" -crop "$geom" +repage "$ren_crop"
        note=
        if [ "$rmask" = ink ] || [ "$rmask" = colors ]; then
            read -r bad ink < <(masked_diff "$ref_crop" "$ren_crop" "$fuzz" "$rmask" "$rkernel" "$rthresh" "$rcolors" "$rcfuzz" \
                "$crop_dir/mask-$rname.png" "$diff_crop")
            if [ "${ink:-0}" -lt 16 ]; then
                p=100.000; note=" (empty ink mask)"
            else
                p=$(awk -v b="$bad" -v n="$ink" 'BEGIN { printf "%.3f", 100.0 * b / n }')
                note=" (ink ${ink}px)"
            fi
        else
            # compare exits 1 when the images differ, 2 on error; AE goes to stderr.
            ae=$(im_compare -metric AE -fuzz "${fuzz}%" "$ref_crop" "$ren_crop" "$diff_crop" 2>&1 >/dev/null) || cmp_rc=$?
            cmp_rc=${cmp_rc:-0}
            ae=${ae%% *}  # ImageMagick 7 prints "N (normalized)"
            if [ "$cmp_rc" -gt 1 ] || ! [[ $ae =~ ^[0-9.e+]+$ ]]; then
                ae=$((rwid * rhgt))
            fi
            unset cmp_rc
            p=$(pct "${ae%%.*}" "$rwid" "$rhgt")
        fi
        x2=$((rx + rwid - 1)); y2=$((ry + rhgt - 1))
        if [ "$rskip" = 1 ]; then
            result=SKIP; skipped=$((skipped + 1)); color='#9e9e9e'
        elif le "$p" "$rmax"; then
            result=PASS; passed=$((passed + 1)); color='#00c853'
        else
            result=FAIL; failed=$((failed + 1)); scenario_failed=1; color='#ff1744'
        fi
        draw_ops+=(-stroke "$color" -draw "rectangle $rx,$ry $x2,$y2")
        printf '%-22s %-20s %9s %9s %5s%% %s%s\n' "$name" "$rname" "$p" "$rmax" "$fuzz" "$result" "$note"
    done < <(python3 -I "$json_py" regions "$regions")

    # Whole-image visual + side by side.
    im_compare -metric AE -fuzz "$(scaled_fuzz 6)%" "$ref" "$render" "$out_dir/$name.diff.png" >/dev/null 2>&1 || true
    if [ ${#draw_ops[@]} -gt 0 ]; then
        im_convert "$out_dir/$name.diff.png" -fill none -strokewidth 1 "${draw_ops[@]}" "$out_dir/$name.diff.png"
    fi
    im_convert "$ref" "$render" +append "$out_dir/$name.sbs.png"
    [ "$scenario_failed" = 0 ] || failed_scenarios+=("$name")
done

echo
echo "uidiff: $passed passed, $failed failed, $skipped skipped of $total regions; outputs in $out_dir"
if [ "$failed" -gt 0 ]; then
    echo "uidiff: FAILED scenarios: ${failed_scenarios[*]}"
    exit 1
fi
exit 0
