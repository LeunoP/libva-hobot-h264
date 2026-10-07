#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$script_dir/.." && pwd)
ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
test_build_dir=${TEST_BUILD_DIR:-/tmp/libva-hobot-tests}

for tool in "$ffmpeg_bin" "$ffprobe_bin" make cmp; do
    command -v "$tool" >/dev/null 2>&1 || {
        printf 'missing required command: %s\n' "$tool" >&2
        exit 2
    }
done
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
    [[ ! -e "$output_dir" ]] || {
        printf 'output path already exists: %s\n' "$output_dir" >&2
        exit 2
    }
    mkdir -p -- "$output_dir"
else
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-vbr.XXXXXX")
fi

driver_path=${LIBVA_DRIVERS_PATH:-$repo_dir}
driver_env=("LIBVA_DRIVER_NAME=$driver_name" "LIBVA_DRIVERS_PATH=$driver_path")
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

make -C "$repo_dir" "TEST_BUILD_DIR=$test_build_dir" \
    "$test_build_dir/test_hevc_wpp_encode"

dynamic_stream="$output_dir/hevc-vbr-dynamic.hevc"
env "${driver_env[@]}" "$test_build_dir/test_hevc_wpp_encode" \
    "$dynamic_stream" 0 VBR
stream_info=$("$ffprobe_bin" -v error -f hevc -count_frames \
    -select_streams v:0 -show_entries stream=profile,width,height,nb_read_frames \
    -of csv=p=0 "$dynamic_stream")
if [[ "$stream_info" != 'Main,640,360,2' ]]; then
    printf 'unexpected dynamic VBR stream metadata: %s\n' "$stream_info" >&2
    exit 1
fi

software_hashes="$output_dir/hevc-vbr-dynamic.software.framemd5"
hardware_hashes="$output_dir/hevc-vbr-dynamic.hardware.framemd5"
"$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$dynamic_stream" \
    -map 0:v:0 -vf format=nv12 -f framemd5 "$software_hashes" -y
env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
    -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
    -f hevc -i "$dynamic_stream" -map 0:v:0 \
    -vf 'hwdownload,format=nv12' -f framemd5 "$hardware_hashes" -y
cmp "$software_hashes" "$hardware_hashes"

env "${driver_env[@]}" HOBOT_HEVC_ENCODE_RC_MODE=VBR \
    "$script_dir/test_hevc_main_encode.sh" "$output_dir/hevc-ffmpeg"
env "${driver_env[@]}" HOBOT_H264_ENCODE_RC_MODE=VBR \
    HOBOT_H264_ENCODE_CYCLES=1 \
    "$script_dir/test_h264_encode.sh" "$output_dir/h264-ffmpeg"

printf 'PASS: H.264/HEVC VA VBR and dynamic HEVC max/target bitrate updates; artifacts: %s\n' \
    "$output_dir"
