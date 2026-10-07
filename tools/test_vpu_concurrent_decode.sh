#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
timeout_bin=${TIMEOUT:-timeout}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
timeout_seconds=${HOBOT_CONCURRENT_DECODE_TIMEOUT:-120}

if [[ $# -lt 2 || $# -gt 3 ]]; then
    printf 'usage: %s h264-input.mp4 hevc-input.mp4 [frame-count]\n' "$0" >&2
    exit 2
fi
h264_input=$1
hevc_input=$2
frames=${3:-64}

for bin in "$ffmpeg_bin" "$ffprobe_bin" "$timeout_bin"; do
    command -v "$bin" >/dev/null 2>&1 || {
        printf 'missing required command: %s\n' "$bin" >&2
        exit 2
    }
done
[[ -e "$drm_device" ]] || {
    printf 'DRM device does not exist: %s\n' "$drm_device" >&2
    exit 2
}
[[ "$frames" =~ ^[1-9][0-9]*$ ]] || {
    printf 'frame-count must be a positive integer\n' >&2
    exit 2
}
[[ "$timeout_seconds" =~ ^[1-9][0-9]*$ ]] || {
    printf 'HOBOT_CONCURRENT_DECODE_TIMEOUT must be a positive integer\n' >&2
    exit 2
}

validate_input() {
    local label=$1 expected_codec=$2 input=$3 codec pixel_format frame_count
    [[ -r "$input" ]] || {
        printf '%s input is not readable: %s\n' "$label" "$input" >&2
        exit 2
    }
    codec=$("$ffprobe_bin" -v error -select_streams v:0 \
        -show_entries stream=codec_name -of default=noprint_wrappers=1:nokey=1 "$input")
    pixel_format=$("$ffprobe_bin" -v error -select_streams v:0 \
        -show_entries stream=pix_fmt -of default=noprint_wrappers=1:nokey=1 "$input")
    frame_count=$("$ffprobe_bin" -v error -select_streams v:0 -count_frames \
        -show_entries stream=nb_read_frames -of default=noprint_wrappers=1:nokey=1 "$input")
    if [[ "$codec" != "$expected_codec" || "$pixel_format" != yuv420p ||
          ! "$frame_count" =~ ^[0-9]+$ ]] || ((frame_count < frames)); then
        printf '%s input must contain at least %s decoded %s yuv420p frames; got codec=%s format=%s frames=%s\n' \
            "$label" "$frames" "$expected_codec" "$codec" "$pixel_format" "$frame_count" >&2
        exit 2
    fi
}

validate_input H.264 h264 "$h264_input"
validate_input HEVC hevc "$hevc_input"

output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-vpu-concurrent.XXXXXX")
h264_software="$output_dir/h264-software.framemd5"
hevc_software="$output_dir/hevc-software.framemd5"
h264_hardware="$output_dir/h264-hardware.framemd5"
hevc_hardware="$output_dir/hevc-hardware.framemd5"

"$ffmpeg_bin" -hide_banner -loglevel error -i "$h264_input" \
    -map 0:v:0 -frames:v "$frames" -vf format=nv12 -f framemd5 "$h264_software"
"$ffmpeg_bin" -hide_banner -loglevel error -i "$hevc_input" \
    -map 0:v:0 -frames:v "$frames" -vf format=nv12 -f framemd5 "$hevc_software"

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

env "${driver_env[@]}" "$timeout_bin" --foreground --signal=TERM --kill-after=10s \
    "${timeout_seconds}s" "$ffmpeg_bin" -hide_banner -loglevel error \
    -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
    -i "$h264_input" -map 0:v:0 -frames:v "$frames" \
    -vf 'hwdownload,format=nv12' -f framemd5 "$h264_hardware" \
    >"$output_dir/h264.log" 2>&1 &
h264_pid=$!
env "${driver_env[@]}" "$timeout_bin" --foreground --signal=TERM --kill-after=10s \
    "${timeout_seconds}s" "$ffmpeg_bin" -hide_banner -loglevel error \
    -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
    -i "$hevc_input" -map 0:v:0 -frames:v "$frames" \
    -vf 'hwdownload,format=nv12' -f framemd5 "$hevc_hardware" \
    >"$output_dir/hevc.log" 2>&1 &
hevc_pid=$!

h264_status=0
wait "$h264_pid" || h264_status=$?
hevc_status=0
wait "$hevc_pid" || hevc_status=$?
if ((h264_status != 0 || hevc_status != 0)); then
    printf 'concurrent hardware decode failed: h264=%s hevc=%s; logs: %s\n' \
        "$h264_status" "$hevc_status" "$output_dir" >&2
    cat "$output_dir/h264.log" "$output_dir/hevc.log" >&2
    exit 1
fi

if ! cmp -s "$h264_software" "$h264_hardware" ||
   ! cmp -s "$hevc_software" "$hevc_hardware"; then
    diff -u "$h264_software" "$h264_hardware" || true
    diff -u "$hevc_software" "$hevc_hardware" || true
    printf 'concurrent hardware output differs from software; artifacts: %s\n' \
        "$output_dir" >&2
    exit 1
fi

printf 'PASS: concurrent H.264/HEVC hardware decode, %s frames each, bit-exact; artifacts: %s\n' \
    "$frames" "$output_dir"
