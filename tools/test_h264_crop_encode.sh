#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cc=${CC:-cc}
ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/card0}

command -v "$cc" >/dev/null 2>&1 || { printf 'missing compiler: %s\n' "$cc" >&2; exit 2; }
command -v "$ffmpeg_bin" >/dev/null 2>&1 || { printf 'missing ffmpeg: %s\n' "$ffmpeg_bin" >&2; exit 2; }
command -v "$ffprobe_bin" >/dev/null 2>&1 || { printf 'missing ffprobe: %s\n' "$ffprobe_bin" >&2; exit 2; }
[[ -e "$drm_device" ]] || { printf 'DRM device does not exist: %s\n' "$drm_device" >&2; exit 2; }
"$ffmpeg_bin" -hide_banner -bsfs 2>/dev/null | grep -qx 'trace_headers' || {
    printf 'ffmpeg must include the trace_headers bitstream filter\n' >&2
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-h264-crop-encode.XXXXXX")
fi

binary="$output_dir/test_h264_crop_encode"
clip="$output_dir/nonzero-left-top-crop.h264"
reference="$output_dir/reference-638x360.nv12"
decoded="$output_dir/decoded-638x360.nv12"
headers="$output_dir/headers.log"
psnr_log="$output_dir/psnr.log"

"$cc" -O2 -Wall -Wextra -Werror -I/usr/include \
    "$repo_dir/tools/test_h264_crop_encode.c" \
    -L/usr/hobot/lib -lva-drm -lva -Wl,-rpath=/usr/hobot/lib \
    -o "$binary"

env LIBVA_DRIVER_NAME="${LIBVA_DRIVER_NAME:-hobot}" \
    "$binary" "$clip" "$reference"

stream_field() {
    "$ffprobe_bin" -v error -select_streams v:0 \
        -show_entries "stream=$1" -of default=noprint_wrappers=1:nokey=1 "$clip"
}
width=$(stream_field width)
height=$(stream_field height)
profile=$(stream_field profile)
level=$(stream_field level)
fps=$(stream_field r_frame_rate)
color_range=$(stream_field color_range)
frames=$("$ffprobe_bin" -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames -of default=noprint_wrappers=1:nokey=1 "$clip")
if [[ "$width" != 638 || "$height" != 360 || "$profile" != High ||
      "$level" != 41 || "$fps" != 60/1 || "$color_range" != tv || "$frames" != 1 ]]; then
    printf 'unexpected encoded stream metadata: %sx%s profile=%s level=%s fps=%s range=%s frames=%s\n' \
        "$width" "$height" "$profile" "$level" "$fps" "$color_range" "$frames" >&2
    exit 1
fi

"$ffmpeg_bin" -hide_banner -loglevel verbose -i "$clip" -map 0:v:0 \
    -c:v copy -bsf:v trace_headers -frames:v 1 -f null - >"$headers" 2>&1
for expected in \
    'level_idc[[:space:]].*= 41$' \
    'num_units_in_tick[[:space:]].*= 1$' \
    'time_scale[[:space:]].*= 120$' \
    'frame_crop_left_offset[[:space:]].*= 1$' \
    'frame_crop_right_offset[[:space:]].*= 0$' \
    'frame_crop_top_offset[[:space:]].*= 1$' \
    'frame_crop_bottom_offset[[:space:]].*= 3$' \
    'video_full_range_flag[[:space:]].*= 0$'; do
    if ! grep -Eq "$expected" "$headers"; then
        printf 'expected SPS field missing: %s\n' "$expected" >&2
        exit 1
    fi
done

"$ffmpeg_bin" -hide_banner -loglevel error -i "$clip" -map 0:v:0 \
    -frames:v 1 -vf 'crop=638:360:2:0,format=nv12' \
    -f rawvideo "$decoded"
if [[ $(stat -c %s "$decoded") -ne $(stat -c %s "$reference") ]]; then
    printf 'decoded/reference NV12 frame sizes differ\n' >&2
    exit 1
fi

"$ffmpeg_bin" -hide_banner -loglevel info \
    -f rawvideo -pixel_format nv12 -video_size 638x360 -framerate 60 -i "$reference" \
    -f rawvideo -pixel_format nv12 -video_size 638x360 -framerate 60 -i "$decoded" \
    -lavfi '[0:v][1:v]psnr' -frames:v 1 -f null - > /dev/null 2>"$psnr_log"
psnr=$(sed -n 's/.*average:\([^ ]*\).*/\1/p' "$psnr_log" | tail -n 1)
if [[ -z "$psnr" ]] || ! awk -v value="$psnr" 'BEGIN { exit !(value + 0 >= 24) }'; then
    printf 'decoded visible frame is not sufficiently close to input (PSNR=%s)\n' \
        "${psnr:-missing}" >&2
    exit 1
fi

printf 'PASS: nonzero left/top crop visible=%sx%s coded=%sx%s, PSNR=%s dB; artifacts: %s\n' \
    "$width" "$height" 640 368 "$psnr" "$output_dir"
