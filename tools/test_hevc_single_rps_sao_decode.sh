#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/card0}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
cycles=${HOBOT_HEVC_SAO_RPS_CYCLES:-5}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fixture="$script_dir/testdata/hevc_main_640x360_single_sps_rps_sao_p.hevc"

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
    printf 'missing HEVC single-RPS SAO fixture: %s\n' "$fixture" >&2
    exit 2
}
[[ -e "$drm_device" ]] || {
    printf 'DRM device does not exist: %s\n' "$drm_device" >&2
    exit 2
}
[[ "$cycles" =~ ^[1-9][0-9]*$ ]] || {
    printf 'HOBOT_HEVC_SAO_RPS_CYCLES must be a positive integer\n' >&2
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-single-rps-sao.XXXXXX")
fi

trace_log="$output_dir/trace_headers.log"
software_hashes="$output_dir/software.framemd5"
driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

"$ffmpeg_bin" -hide_banner -f hevc -i "$fixture" -c:v copy \
    -bsf:v trace_headers -f null - >"$trace_log" 2>&1
for syntax in \
    'sample_adaptive_offset_enabled_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'num_short_term_ref_pic_sets[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'num_negative_pics[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'num_positive_pics[[:space:]]+[^=]*=[[:space:]]*0([[:space:]]|$)' \
    'short_term_ref_pic_set_sps_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'slice_sao_luma_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'slice_sao_chroma_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)'; do
    if ! grep -Eq "$syntax" "$trace_log"; then
        printf 'fixture does not contain expected single-RPS SAO syntax: %s\n' \
            "$syntax" >&2
        exit 1
    fi
done

stream_info=$("$ffprobe_bin" -v error -f hevc -select_streams v:0 \
    -count_frames -show_entries stream=width,height,nb_read_frames \
    -of csv=p=0 "$fixture")
if [[ "$stream_info" != '640,360,2' ]]; then
    printf 'unexpected fixture properties: %s\n' "$stream_info" >&2
    exit 1
fi

"$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$fixture" \
    -map 0:v:0 -frames:v 2 -vf format=nv12 -f framemd5 "$software_hashes"
for ((cycle = 1; cycle <= cycles; cycle++)); do
    printf -v cycle_name 'cycle-%02d' "$cycle"
    cycle_dir="$output_dir/$cycle_name"
    mkdir -- "$cycle_dir"
    hardware_hashes="$cycle_dir/hardware.framemd5"
    env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
        -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
        -f hevc -i "$fixture" -map 0:v:0 -frames:v 2 \
        -vf 'hwdownload,format=nv12' -f framemd5 "$hardware_hashes"
    if ! cmp -s "$software_hashes" "$hardware_hashes"; then
        diff -u "$software_hashes" "$hardware_hashes" || true
        printf 'single-SPS-RPS SAO output differs from software; artifacts: %s\n' \
            "$output_dir" >&2
        exit 1
    fi
    printf 'PASS: single-SPS-RPS SAO cycle %02d/%02d, bit-exact to software\n' \
        "$cycle" "$cycles"
done
printf 'PASS: HEVC single-SPS-RPS SAO regression (%s cycles); artifacts: %s\n' \
    "$cycles" "$output_dir"
