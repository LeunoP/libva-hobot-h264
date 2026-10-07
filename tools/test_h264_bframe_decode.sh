#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}

command -v "$ffmpeg_bin" >/dev/null 2>&1 || {
    printf 'missing ffmpeg: %s\n' "$ffmpeg_bin" >&2
    exit 2
}
command -v "$ffprobe_bin" >/dev/null 2>&1 || {
    printf 'missing ffprobe: %s\n' "$ffprobe_bin" >&2
    exit 2
}
"$ffmpeg_bin" -hide_banner -h encoder=libx264 >/dev/null 2>&1 || {
    printf 'ffmpeg must include the libx264 encoder\n' >&2
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-bframe-regression.XXXXXX")
fi

clip="$output_dir/h264-high-b6.mp4"
software_hashes="$output_dir/software.framemd5"
hardware_hashes="$output_dir/hardware.framemd5"

"$ffmpeg_bin" -hide_banner -loglevel error -y \
    -f lavfi -i 'testsrc2=size=640x360:rate=60' -frames:v 64 -an \
    -c:v libx264 -profile:v high -level:v 4.1 -preset ultrafast \
    -pix_fmt yuv420p -bf 6 -b-pyramid none -g 32 \
    -x264-params 'b-adapt=0:scenecut=0' "$clip"

frame_stats=$("$ffprobe_bin" -v error -select_streams v:0 \
    -show_entries frame=pict_type -of csv=p=0 "$clip" |
    awk 'NF { frames++; if ($1 == "B") { run++; if (run > max) max = run } else run = 0 }
         END { printf "%d %d\n", frames, max }')
read -r frame_count max_b_run <<< "$frame_stats"
printf 'input frames=%s max_consecutive_b_frames=%s\n' "$frame_count" "$max_b_run"
if [[ "$frame_count" -ne 64 || "$max_b_run" -lt 6 ]]; then
    printf 'generated input did not meet the 64-frame / 6-B-frame test contract\n' >&2
    exit 1
fi

"$ffmpeg_bin" -hide_banner -loglevel error -i "$clip" -map 0:v:0 \
    -vf format=nv12 -f framemd5 "$software_hashes"

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi
env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
    -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
    -i "$clip" -map 0:v:0 -vf 'hwdownload,format=nv12' \
    -f framemd5 "$hardware_hashes"

if ! diff -u "$software_hashes" "$hardware_hashes"; then
    printf 'hardware and software NV12 frame hashes differ; artifacts: %s\n' "$output_dir" >&2
    exit 1
fi

printf 'PASS: 64 hardware-decoded frames are bit-exact to software; artifacts: %s\n' "$output_dir"

run_high_b_case() {
    local name=$1 width=$2 height=$3 rate=$4 frame_total=$5 level=$6 refs=$7 gop=$8
    local clip="$output_dir/h264-high-${name}-b6.mp4"
    local software_hashes="$output_dir/high-${name}-software.framemd5"
    local hardware_hashes="$output_dir/high-${name}-hardware.framemd5"
    local level_idc=${level//./}
    local profile stream_level stream_geometry frame_stats frame_count max_b_run

    "$ffmpeg_bin" -hide_banner -loglevel error -y \
        -f lavfi -i "testsrc2=size=${width}x${height}:rate=${rate}" \
        -frames:v "$frame_total" -an -c:v libx264 -profile:v high \
        -level:v "$level" -preset ultrafast -pix_fmt yuv420p -bf 6 \
        -b-pyramid none -refs "$refs" -g "$gop" \
        -x264-params 'b-adapt=0:scenecut=0:8x8dct=1' "$clip"

    profile=$("$ffprobe_bin" -v error -select_streams v:0 \
        -show_entries stream=profile -of default=noprint_wrappers=1:nokey=1 "$clip")
    stream_level=$("$ffprobe_bin" -v error -select_streams v:0 \
        -show_entries stream=level -of default=noprint_wrappers=1:nokey=1 "$clip")
    stream_geometry=$("$ffprobe_bin" -v error -select_streams v:0 \
        -show_entries stream=width,height,pix_fmt,nb_frames -of csv=p=0 "$clip")
    if [[ "$profile" != High || "$stream_level" != "$level_idc" ||
          "$stream_geometry" != "$width,$height,yuv420p,$frame_total" ]]; then
        printf 'unexpected H.264 stream for %s: profile=%s level=%s geometry=%s\n' \
            "$name" "$profile" "$stream_level" "$stream_geometry" >&2
        exit 1
    fi

    frame_stats=$("$ffprobe_bin" -v error -select_streams v:0 \
        -show_entries frame=pict_type -of csv=p=0 "$clip" |
        awk 'NF { frames++; if ($1 == "B") { run++; if (run > max) max = run } else run = 0 }
             END { printf "%d %d\n", frames, max }')
    read -r frame_count max_b_run <<< "$frame_stats"
    if [[ "$frame_count" -ne "$frame_total" || "$max_b_run" -lt 6 ]]; then
        printf 'generated H.264 input did not meet the %s-frame / 6-B-frame contract\n' \
            "$name" >&2
        exit 1
    fi

    "$ffmpeg_bin" -hide_banner -loglevel error -i "$clip" -map 0:v:0 \
        -vf format=nv12 -f framemd5 "$software_hashes"
    env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
        -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
        -i "$clip" -map 0:v:0 -vf 'hwdownload,format=nv12' \
        -f framemd5 "$hardware_hashes"

    if ! cmp -s "$software_hashes" "$hardware_hashes"; then
        diff -u "$software_hashes" "$hardware_hashes" || true
        printf 'H.264 hardware output differs for %s; artifacts: %s\n' \
            "$name" "$output_dir" >&2
        exit 1
    fi

    printf 'PASS: %s H.264 High %s B6 frames are bit-exact to software; artifacts: %s\n' \
        "$frame_total" "$name" "$output_dir"
}

run_high_b_case 1080p60 1920 1080 60 64 5.1 4 32
run_high_b_case 4k30 3840 2160 30 16 5.1 4 16
