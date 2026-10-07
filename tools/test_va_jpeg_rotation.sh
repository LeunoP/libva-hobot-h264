#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || ! -f "$1/hobot_drv_video.so" ]]; then
    printf 'usage: %s <directory-containing-hobot_drv_video.so>\n' "$0" >&2
    exit 2
fi

driver_dir=$1
test_bin=${TEST_BIN:-/tmp/libva-hobot-tests/test_va_jpeg_rotation}
ffmpeg_bin=${FFMPEG:-ffmpeg}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
cycles=${HOBOT_JPEG_ROTATION_CYCLES:-5}
timeout_bin=${TIMEOUT_BIN:-timeout}
output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-jpeg-rotation.XXXXXX")

[[ -x "$test_bin" ]] || {
    printf 'missing test binary: %s (build with make build-va-tests)\n' "$test_bin" >&2
    exit 2
}
command -v "$ffmpeg_bin" >/dev/null 2>&1 || {
    printf 'missing ffmpeg: %s\n' "$ffmpeg_bin" >&2
    exit 2
}
[[ -e "$drm_device" ]] || {
    printf 'DRM device does not exist: %s\n' "$drm_device" >&2
    exit 2
}
[[ "$cycles" =~ ^[1-9][0-9]*$ ]] || {
    printf 'HOBOT_JPEG_ROTATION_CYCLES must be a positive integer\n' >&2
    exit 2
}

jpeg="$output_dir/asymmetric-640x480.jpg"
"$ffmpeg_bin" -hide_banner -loglevel error -y \
    -f lavfi -i 'testsrc2=size=640x480:rate=1' -frames:v 1 \
    -c:v mjpeg -q:v 3 -pix_fmt yuvj420p "$jpeg"

rotation_env=(
    "LIBVA_DRIVER_NAME=hobot"
    "LIBVA_DRIVERS_PATH=$driver_dir"
    "HOBOT_DRM_DEVICE=$drm_device"
)

for rotation in 0 90 180 270; do
    case "$rotation" in
        0)
            output_width=640
            output_height=480
            filter='scale=in_range=pc:out_range=pc,format=nv12'
            ;;
        90)
            output_width=480
            output_height=640
            filter='scale=in_range=pc:out_range=pc,transpose=clock,format=nv12'
            ;;
        180)
            output_width=640
            output_height=480
            filter='scale=in_range=pc:out_range=pc,hflip,vflip,format=nv12'
            ;;
        270)
            output_width=480
            output_height=640
            filter='scale=in_range=pc:out_range=pc,transpose=cclock,format=nv12'
            ;;
    esac

    hardware="$output_dir/hardware-$rotation.nv12"
    reference="$output_dir/reference-$rotation.nv12"
    stats="$output_dir/psnr-$rotation.log"
    log="$output_dir/decode-$rotation.log"

    if ! "$timeout_bin" --foreground 30s env "${rotation_env[@]}" \
        "$test_bin" "$jpeg" "$rotation" "$cycles" "$hardware" \
        >"$log" 2>&1; then
        printf 'hardware JPEG rotation %s failed; log:\n' "$rotation" >&2
        tail -n 60 "$log" >&2
        exit 1
    fi

    "$ffmpeg_bin" -hide_banner -loglevel error -i "$jpeg" -frames:v 1 \
        -vf "$filter" -f rawvideo -pix_fmt nv12 -y "$reference"
    expected_size=$((output_width * output_height * 3 / 2))
    actual_size=$(stat -c '%s' "$hardware")
    if [[ "$actual_size" -ne "$expected_size" ]]; then
        printf 'rotation %s output size mismatch: expected=%s actual=%s\n' \
            "$rotation" "$expected_size" "$actual_size" >&2
        exit 1
    fi

    "$ffmpeg_bin" -hide_banner -loglevel info \
        -f rawvideo -pixel_format nv12 -video_size "${output_width}x${output_height}" \
        -i "$hardware" \
        -f rawvideo -pixel_format nv12 -video_size "${output_width}x${output_height}" \
        -i "$reference" -lavfi "psnr=stats_file=$stats" -frames:v 1 -f null - \
        >/dev/null 2>&1
    average_psnr=$(awk '
        /psnr_avg:/ {
            for (i = 1; i <= NF; i++)
                if ($i ~ /^psnr_avg:/) {
                    sub(/^psnr_avg:/, "", $i)
                    print $i
                }
        }
    ' "$stats")
    if [[ -z "$average_psnr" ]] ||
        ! awk -v value="$average_psnr" 'BEGIN { exit !(value + 0 >= 45) }'; then
        printf 'rotation %s PSNR below 45 dB: %s\n' \
            "$rotation" "${average_psnr:-no measurement}" >&2
        exit 1
    fi
    cat "$log"
    printf 'PASS: rotation=%s PSNR=%s dB\n' "$rotation" "$average_psnr"
done

printf 'Artifacts: %s\n' "$output_dir"
