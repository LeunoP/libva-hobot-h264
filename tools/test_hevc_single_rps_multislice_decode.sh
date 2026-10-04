#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/card0}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fixture="$script_dir/testdata/hevc_main_640x360_two_slices_single_sps_rps_p3.hevc"

command -v "$ffmpeg_bin" >/dev/null 2>&1 || {
    printf 'missing ffmpeg: %s\n' "$ffmpeg_bin" >&2
    exit 2
}
command -v "$ffprobe_bin" >/dev/null 2>&1 || {
    printf 'missing ffprobe: %s\n' "$ffprobe_bin" >&2
    exit 2
}
"$ffmpeg_bin" -hide_banner -bsfs 2>/dev/null | grep -qx trace_headers || {
    printf 'ffmpeg must include the trace_headers bitstream filter\n' >&2
    exit 2
}
[[ -r "$fixture" ]] || {
    printf 'missing HEVC single-RPS multislice fixture: %s\n' "$fixture" >&2
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-single-rps-multislice.XXXXXX")
fi

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

trace_log="$output_dir/trace_headers.log"
"$ffmpeg_bin" -hide_banner -loglevel trace -f hevc -i "$fixture" \
    -c:v copy -bsf:v trace_headers -f null - 2>"$trace_log"

rps_count_check=$(awk '/\[trace_headers @/ && /num_short_term_ref_pic_sets/ {
    if ($NF != "1") invalid = 1
    seen++
} END { print seen + 0; exit !(seen > 0 && !invalid) }' "$trace_log") || {
    printf 'fixture must signal exactly one SPS short-term RPS\n' >&2
    exit 1
}
slice_flags=$(awk '/\[trace_headers @/ && /first_slice_segment_in_pic_flag/ { print $NF }' \
    "$trace_log" | paste -sd, -)
slice_addresses=$(awk '/\[trace_headers @/ && /slice_segment_address/ { print $NF }' \
    "$trace_log" | paste -sd, -)
dependent_flags=$(awk '/\[trace_headers @/ && /dependent_slice_segment_flag/ { print $NF }' \
    "$trace_log" | paste -sd, -)
rps_flags=$(awk '/\[trace_headers @/ && /short_term_ref_pic_set_sps_flag/ { print $NF }' \
    "$trace_log" | paste -sd, -)
if [[ "$slice_flags" != '1,0,1,0,1,0,1,0' ||
      "$slice_addresses" != '30,30,30,30' ||
      "$dependent_flags" != '0,0,0,0' ||
      "$rps_flags" != '1,1,1,1,1,1' ]]; then
    printf 'fixture is not the expected four-picture single-RPS multislice layout\n' >&2
    printf 'first-slice flags: %s\nslice addresses: %s\ndependent flags: %s\nRPS flags: %s\n' \
        "$slice_flags" "$slice_addresses" "$dependent_flags" "$rps_flags" >&2
    exit 1
fi

stream_info=$("$ffprobe_bin" -v error -f hevc -select_streams v:0 \
    -count_frames -show_entries stream=profile,width,height,pix_fmt,nb_read_frames \
    -of csv=p=0 "$fixture")
if [[ "$stream_info" != 'Main,640,360,yuv420p,4' ]]; then
    printf 'unexpected fixture properties: %s\n' "$stream_info" >&2
    exit 1
fi

software_hashes="$output_dir/software.framemd5"
hardware_hashes="$output_dir/hardware.framemd5"
"$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$fixture" \
    -map 0:v:0 -vf format=nv12 -f framemd5 "$software_hashes"
env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
    -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
    -f hevc -i "$fixture" -map 0:v:0 -frames:v 4 \
    -vf 'hwdownload,format=nv12' -f framemd5 "$hardware_hashes"

if ! cmp -s "$software_hashes" "$hardware_hashes"; then
    diff -u "$software_hashes" "$hardware_hashes" || true
    printf 'single-RPS multislice hardware output differs from software; artifacts: %s\n' \
        "$output_dir" >&2
    exit 1
fi
printf 'PASS: HEVC Main 640x360 IDR+3P, independent slices at CTUs 0/30, single SPS RPS; bit-exact to software; artifacts: %s\n' \
    "$output_dir"
