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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-h264-profiles.XXXXXX")
fi

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

for profile in baseline main high; do
    clip="$output_dir/h264-$profile.mp4"
    software_hashes="$output_dir/$profile-software.framemd5"
    hardware_hashes="$output_dir/$profile-hardware.framemd5"
    bframes=0
    x264_params='b-adapt=0:scenecut=0'
    [[ "$profile" == main ]] && bframes=3
    [[ "$profile" == high ]] && bframes=6
    [[ "$profile" == high ]] && x264_params+=':8x8dct=1'

    "$ffmpeg_bin" -hide_banner -loglevel error -y \
        -f lavfi -i 'testsrc2=size=640x360:rate=60' -frames:v 64 -an \
        -c:v libx264 -profile:v "$profile" -level:v 4.1 -preset ultrafast \
        -pix_fmt yuv420p -bf "$bframes" -b-pyramid none -g 32 \
        -x264-params "$x264_params" "$clip"

    stream_profile=$("$ffprobe_bin" -v error -select_streams v:0 \
        -show_entries stream=profile -of default=noprint_wrappers=1:nokey=1 "$clip")
    case "$profile:$stream_profile" in
        baseline:'Constrained Baseline'|main:Main|high:High) ;;
        *)
            printf 'unexpected H264 stream profile: requested=%s actual=%s\n' \
                "$profile" "$stream_profile" >&2
            exit 1
            ;;
    esac

    "$ffmpeg_bin" -hide_banner -loglevel error -i "$clip" -map 0:v:0 \
        -vf format=nv12 -f framemd5 "$software_hashes"
    env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
        -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
        -i "$clip" -map 0:v:0 -vf 'hwdownload,format=nv12' \
        -f framemd5 "$hardware_hashes"

    if ! diff -u "$software_hashes" "$hardware_hashes"; then
        printf '%s profile decode differs from software; artifacts: %s\n' \
            "$profile" "$output_dir" >&2
        exit 1
    fi
    printf 'PASS: %s (%s), 64 hardware frames bit-exact to software\n' \
        "$profile" "$stream_profile"
done

printf 'PASS: all H264 VA profiles; artifacts: %s\n' "$output_dir"
