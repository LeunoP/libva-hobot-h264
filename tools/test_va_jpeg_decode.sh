#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
cycles=${HOBOT_JPEG_DECODE_CYCLES:-5}

command -v "$ffmpeg_bin" >/dev/null 2>&1 || {
    printf 'missing ffmpeg: %s\n' "$ffmpeg_bin" >&2
    exit 2
}
"$ffmpeg_bin" -hide_banner -h encoder=mjpeg >/dev/null 2>&1 || {
    printf 'ffmpeg must include the mjpeg encoder\n' >&2
    exit 2
}
[[ -e "$drm_device" ]] || {
    printf 'DRM device does not exist: %s\n' "$drm_device" >&2
    exit 2
}
[[ "$cycles" =~ ^[1-9][0-9]*$ ]] || {
    printf 'HOBOT_JPEG_DECODE_CYCLES must be a positive integer\n' >&2
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-jpeg-decode.XXXXXX")
fi

jpeg="$output_dir/baseline-420.jpg"
hardware_raw="$output_dir/hardware.nv12"
software_raw="$output_dir/software.nv12"
psnr_log="$output_dir/psnr.log"

"$ffmpeg_bin" -hide_banner -loglevel error -y \
    -f lavfi -i 'testsrc2=size=640x480:rate=1' -frames:v 1 \
    -c:v mjpeg -q:v 3 -pix_fmt yuvj420p "$jpeg"

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
    -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
    -loop 1 -framerate 30 -i "$jpeg" -map 0:v:0 \
    -vf 'hwdownload,format=nv12' -frames:v "$cycles" -f rawvideo -y "$hardware_raw"

expected_bytes=$((640 * 480 * 3 / 2 * cycles))
actual_bytes=$(stat -c '%s' "$hardware_raw")
if [[ "$actual_bytes" -ne "$expected_bytes" ]]; then
    printf 'expected %d decoded bytes for %d frames, got %d\n' \
        "$expected_bytes" "$cycles" "$actual_bytes" >&2
    exit 1
fi

"$ffmpeg_bin" -hide_banner -loglevel error -i "$jpeg" -frames:v 1 \
    -vf 'scale=in_range=pc:out_range=pc,format=nv12' \
    -f rawvideo -y "$software_raw"
"$ffmpeg_bin" -hide_banner -loglevel info \
    -f rawvideo -pixel_format nv12 -video_size 640x480 -i "$hardware_raw" \
    -f rawvideo -pixel_format nv12 -video_size 640x480 -i "$software_raw" \
    -lavfi "psnr=stats_file=$psnr_log" -frames:v 1 -f null - >/dev/null 2>&1

average_psnr=$(awk '
    /psnr_avg:/ { for (i = 1; i <= NF; i++) if ($i ~ /^psnr_avg:/) { sub(/^psnr_avg:/, "", $i); print $i } }
' "$psnr_log")
if [[ -z "$average_psnr" ]] || ! awk -v value="$average_psnr" 'BEGIN { exit !(value + 0 >= 45) }'; then
    printf 'JPEG VLD output did not meet the 45 dB PSNR floor (got %s)\n' \
        "${average_psnr:-no measurement}" >&2
    exit 1
fi

printf 'PASS: %d JPEG Baseline 4:2:0 frames decoded; PSNR %.2f dB; artifacts: %s\n' \
    "$cycles" "$average_psnr" "$output_dir"
