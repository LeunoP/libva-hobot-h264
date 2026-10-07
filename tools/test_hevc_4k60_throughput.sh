#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
min_fps=${HOBOT_MIN_DECODE_FPS:-60}

if [[ $# -lt 1 || $# -gt 2 ]]; then
    printf 'usage: %s 4k60-hevc-main.mp4 [loop-count]\n' "$0" >&2
    exit 2
fi
clip=$1
loops=${2:-20}
[[ -r "$clip" ]] || {
    printf 'input is not readable: %s\n' "$clip" >&2
    exit 2
}
[[ -e "$drm_device" ]] || {
    printf 'DRM device does not exist: %s\n' "$drm_device" >&2
    exit 2
}
command -v "$ffmpeg_bin" >/dev/null 2>&1 || {
    printf 'missing ffmpeg: %s\n' "$ffmpeg_bin" >&2
    exit 2
}
command -v "$ffprobe_bin" >/dev/null 2>&1 || {
    printf 'missing ffprobe: %s\n' "$ffprobe_bin" >&2
    exit 2
}
if [[ ! "$loops" =~ ^[0-9]+$ ]] || (( loops < 1 || loops > 100 )); then
    printf 'loop-count must be between 1 and 100\n' >&2
    exit 2
fi
[[ "$min_fps" =~ ^[0-9]+([.][0-9]+)?$ ]] || {
    printf 'HOBOT_MIN_DECODE_FPS must be numeric\n' >&2
    exit 2
}

stream_info=$("$ffprobe_bin" -v error -select_streams v:0 -count_frames \
    -show_entries stream=profile,width,height,pix_fmt,avg_frame_rate,nb_read_frames \
    -of csv=p=0 "$clip")
IFS=, read -r profile width height pixel_format frame_rate frames <<< "$stream_info"
if [[ "$profile" != Main || "$width" != 3840 || "$height" != 2160 ||
      "$pixel_format" != yuv420p || "$frame_rate" != 60/1 ||
      ! "$frames" =~ ^[1-9][0-9]*$ ]]; then
    printf 'input must be HEVC Main 8-bit 4:2:0, 3840x2160, 60fps; got: %s\n' \
        "$stream_info" >&2
    exit 2
fi

expected_frames=$((frames * loops))
driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi

[[ -r /proc/uptime ]] || {
    printf '/proc/uptime is required for monotonic throughput measurement\n' >&2
    exit 2
}
read -r start_time _ </proc/uptime
progress=$(env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
    -nostats -progress pipe:1 -stream_loop "$((loops - 1))" \
    -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
    -i "$clip" -map 0:v:0 -frames:v "$expected_frames" \
    -vf 'hwdownload,format=nv12' -f null -)
read -r end_time _ </proc/uptime
if [[ ! "$start_time" =~ ^[0-9]+([.][0-9]+)?$ ||
      ! "$end_time" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    printf 'invalid monotonic timestamps in /proc/uptime\n' >&2
    exit 2
fi
output_frames=$(awk -F= '$1 == "frame" { frames = $2 }
    END { print frames + 0 }' <<< "$progress")
elapsed_seconds=$(awk -v start="$start_time" -v end="$end_time" \
    'BEGIN { printf "%.9f", end - start }')
measured_fps=$(awk -v frames="$output_frames" -v elapsed="$elapsed_seconds" \
    'BEGIN { if (elapsed > 0) printf "%.2f", frames / elapsed; else print "0.00" }')
if [[ "$output_frames" != "$expected_frames" ||
      ! "$measured_fps" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    printf 'incomplete throughput run: expected_frames=%s output_frames=%s fps=%s\n' \
        "$expected_frames" "$output_frames" "$measured_fps" >&2
    exit 1
fi
if ! awk -v fps="$measured_fps" -v minimum="$min_fps" \
    'BEGIN { exit !(fps >= minimum) }'; then
    printf 'FAIL: %s frames decoded at %s fps; required at least %s fps\n' \
        "$output_frames" "$measured_fps" "$min_fps" >&2
    exit 1
fi

printf 'PASS: %s 4K60 HEVC frames decoded at %s fps (minimum %s fps)\n' \
    "$output_frames" "$measured_fps" "$min_fps"
