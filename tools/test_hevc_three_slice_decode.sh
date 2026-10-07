#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
cycles=${HOBOT_HEVC_THREE_SLICE_CYCLES:-5}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fixture="$script_dir/testdata/hevc_main_640x360_three_slices_idr_p_p_p.hevc"

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
    printf 'missing HEVC three-slice fixture: %s\n' "$fixture" >&2
    exit 2
}
[[ -e "$drm_device" ]] || {
    printf 'DRM device does not exist: %s\n' "$drm_device" >&2
    exit 2
}
if [[ ! "$cycles" =~ ^[0-9]+$ ]]; then
    printf 'HOBOT_HEVC_THREE_SLICE_CYCLES must be between 1 and 20\n' >&2
    exit 2
fi
cycles=$((10#$cycles))
if ((cycles < 1 || cycles > 20)); then
    printf 'HOBOT_HEVC_THREE_SLICE_CYCLES must be between 1 and 20\n' >&2
    exit 2
fi
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-three-slice.XXXXXX")
fi

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

stream_info=$("$ffprobe_bin" -v error -f hevc -select_streams v:0 -count_frames \
    -show_entries stream=profile,width,height,pix_fmt,nb_read_frames -of csv=p=0 "$fixture")
if [[ "$stream_info" != 'Main,640,360,yuv420p,4' ]]; then
    printf 'unexpected HEVC fixture properties: %s\n' "$stream_info" >&2
    exit 1
fi

trace_log="$output_dir/trace_headers.log"
"$ffmpeg_bin" -hide_banner -loglevel trace -f hevc -i "$fixture" \
    -c:v copy -bsf:v trace_headers -f null - 2>"$trace_log"
slice_flags=$(awk '/\[trace_headers @/ && /first_slice_segment_in_pic_flag/ { print $NF }' \
    "$trace_log" | paste -sd, -)
slice_addresses=$(awk '/\[trace_headers @/ && /slice_segment_address/ { print $NF }' \
    "$trace_log" | paste -sd, -)
if [[ "$slice_flags" != '1,0,0,1,0,0,1,0,0,1,0,0' ||
      "$slice_addresses" != '20,40,20,40,20,40,20,40' ]] ||
   ! awk '/\[trace_headers @/ && /entropy_coding_sync_enabled_flag/ {
       if ($NF != "0") invalid = 1
       seen++
   } END { exit !(seen > 0 && !invalid) }' "$trace_log"; then
    printf 'fixture is not the expected 4-picture, three-slice, WPP-disabled layout\n' >&2
    printf 'first-slice flags: %s\nslice addresses: %s\n' \
        "$slice_flags" "$slice_addresses" >&2
    exit 1
fi

software_hashes="$output_dir/software.framemd5"
"$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$fixture" \
    -map 0:v:0 -vf format=nv12 -frames:v 4 -f framemd5 "$software_hashes"
frame_count=$(awk '!/^#/ && NF { count++ } END { print count + 0 }' "$software_hashes")
if [[ "$frame_count" -ne 4 ]]; then
    printf 'expected 4 software-decoded pictures, got %s\n' "$frame_count" >&2
    exit 1
fi

for ((cycle = 1; cycle <= cycles; cycle++)); do
    printf -v cycle_tag '%02d' "$cycle"
    hardware_hashes="$output_dir/hardware-cycle-$cycle_tag.framemd5"
    env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
        -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
        -f hevc -i "$fixture" -map 0:v:0 -frames:v 4 \
        -vf 'hwdownload,format=nv12' -f framemd5 "$hardware_hashes"
    if ! cmp -s "$software_hashes" "$hardware_hashes"; then
        diff -u "$software_hashes" "$hardware_hashes" || true
        printf 'HEVC three-slice output differs from software in cycle %s; artifacts: %s\n' \
            "$cycle" "$output_dir" >&2
        exit 1
    fi
done

printf 'PASS: HEVC Main 640x360 IDR+3P, three independent slices per picture, bit-exact in %s VPU sessions; artifacts: %s\n' \
    "$cycles" "$output_dir"
