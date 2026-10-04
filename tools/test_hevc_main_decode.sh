#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/card0}
driver_name=${LIBVA_DRIVER_NAME:-hobot}

command -v "$ffmpeg_bin" >/dev/null 2>&1 || {
    printf 'missing ffmpeg: %s\n' "$ffmpeg_bin" >&2
    exit 2
}
command -v "$ffprobe_bin" >/dev/null 2>&1 || {
    printf 'missing ffprobe: %s\n' "$ffprobe_bin" >&2
    exit 2
}
"$ffmpeg_bin" -hide_banner -h encoder=libx265 >/dev/null 2>&1 || {
    printf 'ffmpeg must include the libx265 encoder\n' >&2
    exit 2
}
[[ -e "$drm_device" ]] || {
    printf 'DRM device does not exist: %s\n' "$drm_device" >&2
    exit 2
}

if [[ $# -gt 1 ]]; then
    printf 'usage: %s [new-output-directory]\n' "$0" >&2
    exit 2
fi
if [[ $# -eq 1 ]]; then
    output_dir=$1
    if [[ -e "$output_dir" ]]; then
        printf 'output path already exists: %s\n' "$output_dir" >&2
        exit 2
    fi
    mkdir -p -- "$output_dir"
else
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-main.XXXXXX")
fi

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

run_case() {
    local name=$1 frames=$2 rate=$3 bframes=$4 x265_params=$5 width=$6 height=$7
    local level=${8:-4.1}
    local expected_level_idc=123
    local required_level_idc=0
    if [[ "$level" == 5.0 ]]; then
        expected_level_idc=150
        if [[ "$width" == 1920 && "$height" == 1080 && "$rate" == 60 ]]; then
            required_level_idc=150
        fi
    elif [[ "$level" == 5.1 ]]; then
        expected_level_idc=153
        if [[ "$width" == 3840 && "$height" == 2160 && "$rate" == 60 ]]; then
            required_level_idc=153
        fi
    elif [[ "$level" != 4.1 ]]; then
        printf 'unsupported test level: %s\n' "$level" >&2
        exit 2
    fi
    local clip="$output_dir/hevc-main-$name.mp4"
    local software_hashes="$output_dir/$name-software.framemd5"
    local hardware_hashes="$output_dir/$name-hardware.framemd5"
    local params_with_level="${x265_params}:level-idc=${level}"

    "$ffmpeg_bin" -hide_banner -loglevel error -y \
        -f lavfi -i "testsrc2=size=${width}x${height}:rate=$rate" \
        -frames:v "$frames" -an -c:v libx265 -profile:v main \
        -preset ultrafast -pix_fmt yuv420p -bf "$bframes" \
        -g 32 -x265-params "$params_with_level" "$clip"

    local stream_info
    stream_info=$("$ffprobe_bin" -v error -select_streams v:0 \
        -show_entries stream=profile,pix_fmt,width,height \
        -of csv=p=0 "$clip")
    if [[ "$stream_info" != "Main,${width},${height},yuv420p" ]]; then
        printf 'unexpected HEVC stream properties for %s: %s\n' \
            "$name" "$stream_info" >&2
        exit 1
    fi
    local level_idc
    level_idc=$($ffprobe_bin -v error -select_streams v:0 \
        -show_entries stream=level -of default=noprint_wrappers=1:nokey=1 "$clip")
    if [[ ! "$level_idc" =~ ^[0-9]+$ ]] || ((level_idc > expected_level_idc)) || \
       ((required_level_idc > 0 && level_idc != required_level_idc)); then
        printf 'HEVC level mismatch for %s: cap=%s required=%s actual_level_idc=%s\n' \
            "$name" "$level" "$required_level_idc" "$level_idc" >&2
        exit 1
    fi

    local frame_stats
    frame_stats=$("$ffprobe_bin" -v error -select_streams v:0 \
        -show_entries frame=pict_type -of csv=p=0 "$clip" |
        awk 'NF { frames++; if ($1 == "B") { run++; if (run > max) max = run } else run = 0 }
             END { printf "%d %d\n", frames, max }')
    local frame_count max_b_run
    read -r frame_count max_b_run <<< "$frame_stats"
    if [[ "$frame_count" -ne "$frames" || "$max_b_run" -lt "$bframes" ]]; then
        printf 'generated %s stream failed frame/B-frame contract: frames=%s B-run=%s\n' \
            "$name" "$frame_count" "$max_b_run" >&2
        exit 1
    fi

    "$ffmpeg_bin" -hide_banner -loglevel error -i "$clip" -map 0:v:0 \
        -vf format=nv12 -f framemd5 "$software_hashes"
    env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
        -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
        -i "$clip" -map 0:v:0 -vf 'hwdownload,format=nv12' \
        -f framemd5 "$hardware_hashes"

    if ! diff -u "$software_hashes" "$hardware_hashes"; then
        printf '%s HEVC Main output differs from software; artifacts: %s\n' \
            "$name" "$output_dir" >&2
        exit 1
    fi
    printf 'PASS: HEVC Main %s %sx%s (%s frames, max consecutive B=%s), bit-exact to software\n' \
        "$name" "$width" "$height" "$frame_count" "$max_b_run"
}

run_weighted_p_case() {
    local clip="$output_dir/hevc-main-weighted-p.mp4"
    local software_hashes="$output_dir/weighted-p-software.framemd5"
    local hardware_hashes="$output_dir/weighted-p-hardware.framemd5"

    "$ffmpeg_bin" -hide_banner -loglevel error -y \
        -f lavfi -i 'testsrc2=size=640x360:rate=30' \
        -vf "eq=brightness='if(gte(n,4),0.1,0)':eval=frame" \
        -frames:v 24 -an -c:v libx265 -profile:v main -preset ultrafast \
        -pix_fmt yuv420p -bf 0 \
        -x265-params 'log-level=error:pools=none:frame-threads=1:bframes=0:ref=1:weightp=1:weightb=0:sao=0:keyint=24:min-keyint=24:scenecut=0' \
        "$clip"

    local header_trace
    header_trace=$("$ffmpeg_bin" -hide_banner -i "$clip" -c:v copy \
        -bsf:v trace_headers -f null - 2>&1)
    if ! grep -Eq 'num_short_term_ref_pic_sets[[:space:]]+1 = 0' <<<"$header_trace" || \
       ! grep -Eq 'weighted_pred_flag[[:space:]]+1 = 1' <<<"$header_trace" || \
       ! grep -Eq 'luma_weight_l0_flag\[[0-9]+\][[:space:]]+1 = 1' <<<"$header_trace" || \
       ! grep -Eq 'luma_offset_l0\[[0-9]+\].*= -?[1-9][0-9]*' <<<"$header_trace"; then
        printf 'weighted-P test stream did not contain a non-default luma weight\n' >&2
        exit 1
    fi

    "$ffmpeg_bin" -hide_banner -loglevel error -i "$clip" -map 0:v:0 \
        -vf format=nv12 -f framemd5 "$software_hashes"
    env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
        -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
        -i "$clip" -map 0:v:0 -vf 'hwdownload,format=nv12' \
        -f framemd5 "$hardware_hashes"

    if ! diff -u "$software_hashes" "$hardware_hashes"; then
        printf 'weighted-P HEVC output differs from software; artifacts: %s\n' \
            "$output_dir" >&2
        exit 1
    fi
    printf 'PASS: HEVC weighted P 640x360 (24 frames, non-default luma weight), bit-exact to software\n'
}

run_weighted_b_case() {
    local clip="$output_dir/hevc-main-weighted-b.hevc"
    local software_hashes="$output_dir/weighted-b-software.framemd5"
    local hardware_hashes="$output_dir/weighted-b-hardware.framemd5"

    "$ffmpeg_bin" -hide_banner -loglevel error -y \
        -f lavfi -i 'testsrc2=size=640x360:rate=30' \
        -vf "eq=brightness='if(gte(n,8),0.1,0)':eval=frame" \
        -frames:v 32 -an -c:v libx265 -profile:v main -preset ultrafast \
        -pix_fmt yuv420p -bf 3 \
        -x265-params 'log-level=error:pools=none:frame-threads=1:bframes=3:b-adapt=0:rc-lookahead=10:ref=2:weightp=1:weightb=1:sao=0:keyint=32:min-keyint=32:scenecut=0:open-gop=0' \
        -f hevc "$clip"

    local frame_stats frame_count max_b_run
    frame_stats=$("$ffprobe_bin" -v error -select_streams v:0 \
        -show_entries frame=pict_type -of csv=p=0 "$clip" |
        awk 'NF { frames++; if ($1 == "B") { run++; if (run > max) max = run } else run = 0 }
             END { printf "%d %d\n", frames, max }')
    read -r frame_count max_b_run <<< "$frame_stats"
    if [[ "$frame_count" -ne 32 || "$max_b_run" -lt 3 ]]; then
        printf 'weighted-B test stream failed frame/B-frame contract: frames=%s B-run=%s\n' \
            "$frame_count" "$max_b_run" >&2
        exit 1
    fi

    local header_trace
    header_trace=$("$ffmpeg_bin" -hide_banner -i "$clip" -c:v copy \
        -bsf:v trace_headers -f null - 2>&1)
    if ! grep -Eq 'num_short_term_ref_pic_sets[[:space:]]+1 = 0' <<<"$header_trace" || \
       ! grep -Eq 'weighted_bipred_flag[[:space:]]+1 = 1' <<<"$header_trace" || \
       ! grep -Eq 'luma_weight_l0_flag\[[0-9]+\][[:space:]]+1 = 1' <<<"$header_trace" || \
       ! grep -Eq 'luma_offset_l0\[[0-9]+\].*= -?[1-9][0-9]*' <<<"$header_trace" || \
       ! grep -Eq 'luma_weight_l1_flag\[0\][[:space:]]+1 = 1' <<<"$header_trace" || \
       ! grep -Eq 'luma_offset_l1\[0\].*= -?[1-9][0-9]*' <<<"$header_trace"; then
        printf 'weighted-B test stream did not contain non-default P/B luma weights\n' >&2
        exit 1
    fi

    "$ffmpeg_bin" -hide_banner -loglevel error -i "$clip" -map 0:v:0 \
        -vf format=nv12 -f framemd5 "$software_hashes"
    env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
        -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
        -i "$clip" -map 0:v:0 -vf 'hwdownload,format=nv12' \
        -f framemd5 "$hardware_hashes"

    if ! diff -u "$software_hashes" "$hardware_hashes"; then
        printf 'weighted-B HEVC output differs from software; artifacts: %s\n' \
            "$output_dir" >&2
        exit 1
    fi
    printf 'PASS: HEVC weighted B 640x360 (32 frames, B3, non-default P-L0/B-L1 luma weights), bit-exact to software\n'
}

if [[ "${HOBOT_HEVC_WEIGHTED_ONLY:-0}" == 1 ]]; then
    run_weighted_p_case
    run_weighted_b_case
    exit 0
fi

if [[ "${HOBOT_HEVC_WEIGHTED_P_ONLY:-0}" == 1 ]]; then
    run_weighted_p_case
    exit 0
fi

run_case p 30 30 0 'log-level=error:bframes=0:keyint=32:min-keyint=32:scenecut=0:open-gop=0' 640 360
run_case b3 64 60 3 'log-level=error:bframes=3:b-adapt=0:keyint=32:min-keyint=32:scenecut=0:open-gop=0' 1280 720
run_case b6 64 60 6 'log-level=error:bframes=6:b-adapt=0:rc-lookahead=10:keyint=32:min-keyint=32:scenecut=0:open-gop=0' 1280 720
run_case b6_1080p 64 60 6 'log-level=error:bframes=6:b-adapt=0:rc-lookahead=10:keyint=32:min-keyint=32:scenecut=0:open-gop=0' 1920 1080 5.0
run_case b3_4k60 16 60 3 'log-level=error:bframes=3:b-adapt=0:rc-lookahead=10:keyint=32:min-keyint=32:scenecut=0:open-gop=0' 3840 2160 5.1
run_weighted_p_case
run_weighted_b_case

printf 'PASS: HEVC Main decode regression; artifacts: %s\n' "$output_dir"
