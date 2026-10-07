#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
cycles=${HOBOT_JPEG_DECODE_CYCLES:-5}
decode_timeout=${HOBOT_JPEG_DECODE_TIMEOUT:-20}
timeout_bin=${TIMEOUT_BIN:-timeout}

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
[[ "$decode_timeout" =~ ^[1-9][0-9]*$ ]] || {
    printf 'HOBOT_JPEG_DECODE_TIMEOUT must be a positive number of seconds\n' >&2
    exit 2
}
command -v "$timeout_bin" >/dev/null 2>&1 || {
    printf 'missing timeout command: %s\n' "$timeout_bin" >&2
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

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

run_case() {
    local sampling=$1
    local pixel_format=$2
    local jpeg="$output_dir/baseline-$sampling.jpg"
    local hardware_raw="$output_dir/hardware-$sampling.nv12"
    local software_raw="$output_dir/software-$sampling.nv12"
    local psnr_log="$output_dir/psnr-$sampling.log"
    local decode_log="$output_dir/decode-$sampling.log"

    "$ffmpeg_bin" -hide_banner -loglevel error -y \
        -f lavfi -i 'testsrc2=size=640x480:rate=1' -frames:v 1 \
        -c:v mjpeg -q:v 3 -pix_fmt "$pixel_format" "$jpeg"

    if ! "$timeout_bin" --foreground "${decode_timeout}s" \
        env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
        -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
        -loop 1 -framerate 30 -i "$jpeg" -map 0:v:0 \
        -vf 'hwdownload,format=nv12' -frames:v "$cycles" -f rawvideo -y "$hardware_raw" \
        > /dev/null 2>"$decode_log"; then
        printf '%s JPEG VLD decode failed or exceeded %ss; recent log:\n' \
            "$sampling" "$decode_timeout" >&2
        tail -n 40 "$decode_log" >&2
        return 1
    fi

    local expected_bytes=$((640 * 480 * cycles * 3 / 2))
    local actual_bytes
    actual_bytes=$(stat -c '%s' "$hardware_raw")
    if [[ "$actual_bytes" -ne "$expected_bytes" ]]; then
        printf '%s: expected %d decoded bytes for %d frames, got %d\n' \
            "$sampling" "$expected_bytes" "$cycles" "$actual_bytes" >&2
        return 1
    fi

    "$ffmpeg_bin" -hide_banner -loglevel error -i "$jpeg" -frames:v 1 \
        -vf 'scale=in_range=pc:out_range=pc,format=nv12' \
        -f rawvideo -y "$software_raw"
    "$ffmpeg_bin" -hide_banner -loglevel info \
        -f rawvideo -pixel_format nv12 -video_size 640x480 -i "$hardware_raw" \
        -f rawvideo -pixel_format nv12 -video_size 640x480 -i "$software_raw" \
        -lavfi "psnr=stats_file=$psnr_log" -frames:v 1 -f null - >/dev/null 2>&1

    local average_psnr
    average_psnr=$(awk '
        /psnr_avg:/ { for (i = 1; i <= NF; i++) if ($i ~ /^psnr_avg:/) { sub(/^psnr_avg:/, "", $i); print $i } }
    ' "$psnr_log")
    if [[ -z "$average_psnr" ]] ||
        ! awk -v value="$average_psnr" 'BEGIN { exit !(value + 0 >= 45) }'; then
        printf '%s JPEG VLD output did not meet the 45 dB PSNR floor (got %s)\n' \
            "$sampling" "${average_psnr:-no measurement}" >&2
        return 1
    fi

    printf 'PASS: %d JPEG Baseline yuvj%s (4:2:0) frames decoded; PSNR %.2f dB\n' \
        "$cycles" "$sampling" "$average_psnr"
}

run_case 420 yuvj420p
printf 'Artifacts: %s\n' "$output_dir"
