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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-h264-multislice.XXXXXX")
fi

clip="$output_dir/h264-high-b6-2slices.mp4"
trace_log="$output_dir/trace_headers.log"
software_hashes="$output_dir/software.framemd5"
hardware_hashes="$output_dir/hardware.framemd5"

"$ffmpeg_bin" -hide_banner -loglevel error -y \
    -f lavfi -i 'testsrc2=size=640x360:rate=60' -frames:v 64 -an \
    -c:v libx264 -profile:v high -level:v 4.1 -preset ultrafast \
    -pix_fmt yuv420p -bf 6 -b-pyramid none -g 32 \
    -x264-params 'slices=2:b-adapt=0:scenecut=0:8x8dct=1' "$clip"

stream_info=$("$ffprobe_bin" -v error -select_streams v:0 \
    -show_entries stream=profile,width,height,pix_fmt \
    -of csv=p=0 "$clip")
if [[ "$stream_info" != 'High,640,360,yuv420p' ]]; then
    printf 'unexpected H.264 stream properties: %s\n' "$stream_info" >&2
    exit 1
fi

frame_stats=$("$ffprobe_bin" -v error -select_streams v:0 \
    -show_entries frame=pict_type -of csv=p=0 "$clip" |
    awk 'NF { frames++; if ($1 == "B") { run++; if (run > max) max = run } else run = 0 }
         END { printf "%d %d\n", frames, max }')
read -r frame_count max_b_run <<< "$frame_stats"
if [[ "$frame_count" -ne 64 || "$max_b_run" -lt 6 ]]; then
    printf 'generated input failed frame/B-frame contract: frames=%s max-B-run=%s\n' \
        "$frame_count" "$max_b_run" >&2
    exit 1
fi

"$ffmpeg_bin" -hide_banner -loglevel trace -i "$clip" -c:v copy \
    -bsf:v trace_headers -f null - 2>"$trace_log"
read -r slice_count invalid_slice_start_count < <(
    awk '/\[trace_headers @/ && /first_mb_in_slice/ {
        expected = count % 2 == 0 ? 0 : 480
        if ($NF != expected)
            invalid++
        count++
    }
    END { printf "%d %d\n", count, invalid + 0 }' "$trace_log"
)
if [[ "$slice_count" -ne 128 || "$invalid_slice_start_count" -ne 0 ]]; then
    printf 'expected 64 pairs of slices at first_mb_in_slice 0/480; got %s slices, %s invalid starts\n' \
        "$slice_count" "$invalid_slice_start_count" >&2
    exit 1
fi

"$ffmpeg_bin" -hide_banner -loglevel error -i "$clip" -map 0:v:0 \
    -vf format=nv12 -f framemd5 "$software_hashes"

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
    -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
    -i "$clip" -map 0:v:0 -vf 'hwdownload,format=nv12' \
    -f framemd5 "$hardware_hashes"

if ! diff -u "$software_hashes" "$hardware_hashes"; then
    printf 'H.264 multi-slice output differs from software; artifacts: %s\n' \
        "$output_dir" >&2
    exit 1
fi

printf 'PASS: H.264 High 640x360, 64 frames, 2 slices/frame, B6; bit-exact to software; artifacts: %s\n' \
    "$output_dir"
