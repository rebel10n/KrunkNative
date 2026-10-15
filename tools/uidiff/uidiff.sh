#!/usr/bin/env bash
# uidiff.sh - per-region pixel diff of the native HTML UI against the Krunker
# v10.0.2 reference captures (doc/ref/krunker-v10.0.2/, doc/ref/krunker-v10/) and
# KrunkNative's own (doc/ref/krunknative/).
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
# Frame sequences: a scenario with "anim_times" [ms, ...] (+ "anim_origin") renders
# <name>.<t>.png per time (client/src/ui/ui_scenario.c) against the reference frames
# <ref>/<t>.png (ref is a directory: tools/uidiff/capture-v10-seq.js). Every frame is
# diffed with the regions whose "times" include it (default: all), each with its
# "at": {"<t>": {...}} overrides. Every "timing" entry {name, rect, threshold (0.5),
# tolerance_ms (one 60 Hz frame)} is a timing assertion: the time where the region's
# change from the first frame first reaches threshold x its largest change
# (interpolated between frames) must agree between reference and render.
#
# "pending": "<owner: what is missing>" on a region (or a timing entry) skips it with that
# text, on a scenario skips the whole scenario without rendering it: the native side is not
# built yet (UI-PARITY-AUDIT section 4); its owner removes the key with the implementation.
#
# Outputs (per scenario, in $UIDIFF_OUT):
#   <name>.png        the render
#   <name>.log        the renderer's stdout/stderr
#   <name>.diff.png   compare's visual (differences in red) with the region
#                     rectangles drawn on top (green PASS, red FAIL, gray SKIP)
#   <name>.sbs.png    reference | render side by side
#   <name>.regions/   per-region crops (ref-, render-, diff-<region>.png;
#                     mask-<region>.png for ink-masked regions)
#   sequences: <name>.<t>.png, <name>.<t>.diff.png, <name>.<t>.sbs.png and
#   <name>.<t>.regions/ per frame, <name>.strip.png (reference frames over the
#   rendered ones, quarter size)
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
# 1 when any region fails or a render or a reference frame is missing/has the wrong size
# (each one a FAIL row for its scenario; the run goes on), 2 on usage/tool errors.
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
        # a name, a path, or a subdirectory scenario ("strike/main-menu": scenarios/strike/)
        case "$n" in
            */scenarios/*) n=${n##*/scenarios/} ;;
            scenarios/*) n=${n#scenarios/} ;;
            */*/*) n=${n##*/} ;;
        esac
        scenarios+=("$n")
    done
else
    for f in "$here"/scenarios/*.json "$here"/scenarios/strike/*.json; do
        [ -e "$f" ] || continue
        n=${f#"$here"/scenarios/}
        # only the scenarios with a reference and regions (the others are smoke-ui renders)
        [ -f "$here/regions/$n" ] || continue
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

# diff_frame <name> <label> <ref.png> <render.png> <crop-dir> <diff.png> <sbs.png> <background> [t]:
# every region of regions/<name>.json (of frame t of a sequence) against the reference; prints
# the table rows and counts into the totals; returns 1 when a region failed.
diff_frame() {
    local name=$1 label=$2 ref=$3 render=$4 crop_dir=$5 diff_png=$6 sbs_png=$7 background=$8 t=${9:-}
    local regions="$here/regions/$name.json" frame_failed=0
    local rname rx ry rwid rhgt rfuzz rmax rskip rmask rkernel rthresh rcolors rcfuzz rdx rdy rexpect
    local fuzz geom ref_crop ren_crop diff_crop ref_geom note bad ink p ae x2 y2 result color
    mkdir -p "$crop_dir"
    local draw_ops=()
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
        if [ "$rskip" != 0 ]; then
            result=SKIP; skipped=$((skipped + 1)); color='#9e9e9e'
            [ "$rskip" = 1 ] || note="$note (pending: $rskip)"
        elif le "$p" "$rmax"; then
            result=PASS; passed=$((passed + 1)); color='#00c853'
        else
            result=FAIL; failed=$((failed + 1)); frame_failed=1; color='#ff1744'
        fi
        draw_ops+=(-stroke "$color" -draw "rectangle $rx,$ry $x2,$y2")
        printf '%-22s %-20s %9s %9s %5s%% %s%s\n' "$label" "$rname" "$p" "$rmax" "$fuzz" "$result" "$note"
    done < <(python3 -I "$json_py" regions "$regions" ${t:+"$t"})

    # Whole-image visual + side by side.
    im_compare -metric AE -fuzz "$(scaled_fuzz 6)%" "$ref" "$render" "$diff_png" >/dev/null 2>&1 || true
    if [ ${#draw_ops[@]} -gt 0 ]; then
        im_convert "$diff_png" -fill none -strokewidth 1 "${draw_ops[@]}" "$diff_png"
    fi
    im_convert "$ref" "$render" +append "$sbs_png"
    return "$frame_failed"
}

# region_change <a.png> <b.png> <geometry> -> mean |a - b| over the region (0..1, grey)
region_change() {
    im_convert "$1" "$2" -crop "$3" +repage -compose difference -composite -colorspace gray -format '%[fx:mean]\n' info: 2>/dev/null \
        | head -n 1
}

# timing_check <name> <ref-dir> <render-base> <times...>: every "timing" entry of regions/<name>.json:
# the time its region's change from the first frame first reaches `threshold` of its largest change,
# in the reference and in the render, must agree within tolerance_ms (default one 60 Hz frame).
timing_check() {
    local name=$1 ref_dir=$2 base=$3; shift 3
    local times=("$@") regions="$here/regions/$name.json" check_failed=0
    local tname tx ty tw th tthr ttol tskip geom t first_ref first_ren ref_cross ren_cross ref_peak ren_peak delta result
    while IFS=$'\t' read -r tname tx ty tw th tthr ttol tskip; do
        [ -n "$tname" ] || continue
        total=$((total + 1))
        geom="${tw}x${th}+${tx}+${ty}"
        first_ref="$ref_dir/${times[0]}.png"
        first_ren="$base.${times[0]}.png"
        read -r ref_cross ref_peak < <(for t in "${times[@]}"; do echo "$t $(region_change "$first_ref" "$ref_dir/$t.png" "$geom")"; done \
            | python3 -I "$json_py" crossing "$tthr")
        read -r ren_cross ren_peak < <(for t in "${times[@]}"; do echo "$t $(region_change "$first_ren" "$base.$t.png" "$geom")"; done \
            | python3 -I "$json_py" crossing "$tthr")
        if [ "$ref_cross" = none ]; then
            result="FAIL (the reference does not change here: fix the region)"; delta=-
        elif [ "$ren_cross" = none ]; then
            result="FAIL (no change in the render; reference crosses at ${ref_cross} ms)"; delta=-
        else
            delta=$(awk -v a="$ren_cross" -v b="$ref_cross" 'BEGIN { d = a - b; printf "%.1f", d < 0 ? -d : d }')
            if le "$delta" "$ttol"; then result="PASS"; else result="FAIL"; fi
            result="$result (crosses ${tthr}: reference ${ref_cross} ms, render ${ren_cross} ms)"
        fi
        if [ "$tskip" != 0 ]; then
            skipped=$((skipped + 1)); result="SKIP ${result#* }"
            [ "$tskip" = 1 ] || result="$result (pending: $tskip)"
        elif [[ $result == PASS* ]]; then
            passed=$((passed + 1))
        else
            failed=$((failed + 1)); check_failed=1
        fi
        printf '%-22s %-20s %9s %9s %6s %s\n' "$name" "timing:$tname" "$delta" "$(printf '%.1f' "$ttol")" ms "$result"
    done < <(python3 -I "$json_py" timing "$regions")
    return "$check_failed"
}

for name in "${scenarios[@]}"; do
    scenario="$here/scenarios/$name.json"
    regions="$here/regions/$name.json"
    if [ ! -f "$scenario" ]; then
        echo "uidiff: scenario not found: $scenario" >&2
        exit 2
    fi
    IFS=$'\t' read -r ref w h background _frames anim_times pending < <(python3 -I "$json_py" scenario "$scenario") || true
    # "pending" (the native side is not built yet) / "unreachable" (no v10 reference can be captured
    # offline): the whole scenario is reported, not rendered
    if [ -n "${pending:-}" ] && [ "$pending" != - ]; then
        printf '%-22s %-20s %9s %9s %6s %s\n' "$name" '(scenario)' - - - "SKIP ($pending)"
        skipped=$((skipped + 1)); total=$((total + 1))
        continue
    fi
    if [ ! -f "$regions" ]; then
        echo "uidiff: regions not found: $regions" >&2
        exit 2
    fi
    # a frame sequence (anim_times): ref is a directory of <t>.png, the render <name>.<t>.png
    seq_times=()
    if [ -n "${anim_times:-}" ] && [ "$anim_times" != - ]; then
        IFS=, read -r -a seq_times <<<"$anim_times"
        ref_first="$ref/${seq_times[0]}.png"
        # a missing reference (sequence dir or frame) is this scenario's FAIL row, not a run error
        ref_missing=
        if [ ! -d "$ref" ]; then
            ref_missing="reference sequence missing: $ref/ (tools/uidiff/capture-v10-seq.js)"
        else
            for t in "${seq_times[@]}"; do
                [ -f "$ref/$t.png" ] || { ref_missing="reference frame missing: $ref/$t.png"; break; }
            done
        fi
        if [ -n "$ref_missing" ]; then
            echo "uidiff: $name: $ref_missing" >&2
            printf '%-22s %-20s %9s %9s %6s %s\n' "$name" '(reference)' - - - "FAIL ($ref_missing)"
            failed=$((failed + 1)); total=$((total + 1)); failed_scenarios+=("$name")
            continue
        fi
    else
        if [ ! -f "$ref" ]; then
            echo "uidiff: $name: reference missing: $ref" >&2
            exit 2
        fi
        ref_first=$ref
    fi
    read -r ref_w ref_h < <(image_size "$ref_first")
    if [ "$ref_w" != "$w" ] || [ "$ref_h" != "$h" ]; then
        echo "uidiff: $name: reference $ref_first is ${ref_w}x${ref_h}, scenario size is ${w}x${h}" >&2
    fi

    render="$out_dir/$name.png"
    mkdir -p "$(dirname "$render")"
    scenario_abs=$(cd "$(dirname "$scenario")" && pwd)/$(basename "$scenario")
    render_abs=$(cd "$(dirname "$render")" && pwd)/$(basename "$render")
    scenario_failed=0
    for t in "${seq_times[@]}"; do rm -f "$out_dir/$name.$t.png"; done
    missing=
    if ! ( render_scenario "$name" "$scenario_abs" "$ref_first" "$w" "$h" "$render_abs" ) >"$out_dir/$name.log" 2>&1 \
        || [ ! -s "$render" ]; then
        missing="no render, see $out_dir/$name.log"
    fi
    if [ -z "$missing" ] && [ "$render_cmd" = cp ] && [ ${#seq_times[@]} -gt 0 ]; then
        for t in "${seq_times[@]}"; do cp "$ref/$t.png" "$out_dir/$name.$t.png"; done # self-test: the reference frames
    fi
    for t in "${seq_times[@]}"; do
        [ -n "$missing" ] || [ -s "$out_dir/$name.$t.png" ] || missing="no frame $t ms, see $out_dir/$name.log"
    done
    if [ -n "$missing" ]; then
        printf '%-22s %-20s %9s %9s %6s %s\n' "$name" '(render)' - - - "FAIL ($missing)"
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

    if [ ${#seq_times[@]} -eq 0 ]; then
        diff_frame "$name" "$name" "$ref" "$render" "$out_dir/$name.regions" "$out_dir/$name.diff.png" "$out_dir/$name.sbs.png" \
            "$background" || scenario_failed=1
    else
        # per frame: the regions of that frame (region "times" / "at"), then the timing checks
        for t in "${seq_times[@]}"; do
            diff_frame "$name" "$name@$t" "$ref/$t.png" "$out_dir/$name.$t.png" "$out_dir/$name.$t.regions" \
                "$out_dir/$name.$t.diff.png" "$out_dir/$name.$t.sbs.png" "$background" "$t" || scenario_failed=1
        done
        timing_check "$name" "$ref" "$out_dir/$name" "${seq_times[@]}" || scenario_failed=1
        # the whole sequence at a glance: reference row over render row, a quarter size
        ref_frames=(); ren_frames=()
        for t in "${seq_times[@]}"; do ref_frames+=("$ref/$t.png"); ren_frames+=("$out_dir/$name.$t.png"); done
        im_convert \( "${ref_frames[@]}" +append \) \( "${ren_frames[@]}" +append \) -append \
            -resize 25% "$out_dir/$name.strip.png" 2>/dev/null || true
    fi
    [ "$scenario_failed" = 0 ] || failed_scenarios+=("$name")
done

echo
echo "uidiff: $passed passed, $failed failed, $skipped skipped of $total regions; outputs in $out_dir"
if [ "$failed" -gt 0 ]; then
    echo "uidiff: FAILED scenarios: ${failed_scenarios[*]}"
    exit 1
fi
exit 0
