#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
cycles=${HOBOT_HEVC_TWO_RPS_WEIGHTED_CYCLES:-5}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
generator="$script_dir/generate_hevc_neutral_weight_fixture.py"
fixture_dir="$script_dir/testdata"

command -v "$ffmpeg_bin" >/dev/null 2>&1 || {
    printf 'missing ffmpeg: %s\n' "$ffmpeg_bin" >&2
    exit 2
}
command -v python3 >/dev/null 2>&1 || {
    printf 'missing python3\n' >&2
    exit 2
}
"$ffmpeg_bin" -hide_banner -bsfs 2>/dev/null | grep -qx trace_headers || {
    printf 'ffmpeg must include the trace_headers bitstream filter\n' >&2
    exit 2
}
[[ -e "$drm_device" ]] || {
    printf 'DRM device does not exist: %s\n' "$drm_device" >&2
    exit 2
}
[[ "$cycles" =~ ^[1-9][0-9]*$ ]] || {
    printf 'HOBOT_HEVC_TWO_RPS_WEIGHTED_CYCLES must be a positive integer\n' >&2
    exit 2
}
[[ -r "$fixture_dir/hevc_main_640x360_two_slices_idr_p_p.hevc" &&
   -r "$fixture_dir/hevc_main_640x360_two_slices_idr_bbb.hevc" ]] || {
    printf 'missing two-SPS-RPS HM fixtures in %s\n' "$fixture_dir" >&2
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-two-rps-weighted.XXXXXX")
fi

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi
output_root=$output_dir

run_case() {
    local profile=$1 mode=$2 source=$3 frame_count=$4
    local output="$output_dir/${profile}-weighted-${mode}.hevc"
    local source_hashes="$output_dir/${profile}-${mode}-source-software.framemd5"
    local transformed_hashes="$output_dir/${profile}-${mode}-software.framemd5"
    local hardware_hashes="$output_dir/${profile}-${mode}-hardware.framemd5"

    python3 "$generator" --mode "$mode" --weights "$profile" "$source" "$output"
    "$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$source" \
        -map 0:v:0 -frames:v "$frame_count" -vf format=nv12 -f framemd5 "$source_hashes"
    "$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$output" \
        -map 0:v:0 -frames:v "$frame_count" -vf format=nv12 -f framemd5 "$transformed_hashes"
    if [[ "$profile" == neutral ]]; then
        if ! cmp -s "$source_hashes" "$transformed_hashes"; then
            printf 'neutral weighted-%s software output changed; artifacts: %s\n' \
                "$mode" "$output_dir" >&2
            exit 1
        fi
    elif cmp -s "$source_hashes" "$transformed_hashes"; then
        printf 'non-default weighted-%s did not change software output; artifacts: %s\n' \
            "$mode" "$output_dir" >&2
        exit 1
    fi

    env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
        -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
        -f hevc -i "$output" -map 0:v:0 -frames:v "$frame_count" \
        -vf 'hwdownload,format=nv12' -f framemd5 "$hardware_hashes"
    if ! cmp -s "$transformed_hashes" "$hardware_hashes"; then
        printf 'two-RPS %s weighted-%s hardware output differs; artifacts: %s\n' \
            "$profile" "$mode" "$output_dir" >&2
        exit 1
    fi
    printf 'PASS: two-SPS-RPS %s weighted-%s (%s frames), bit-exact to software\n' \
        "$profile" "$mode" "$frame_count"
}

for ((cycle = 1; cycle <= cycles; cycle++)); do
    printf -v cycle_name 'cycle-%02d' "$cycle"
    output_dir="$output_root/$cycle_name"
    mkdir -- "$output_dir"
    printf 'cycle %02d/%02d\n' "$cycle" "$cycles"
    run_case neutral p "$fixture_dir/hevc_main_640x360_two_slices_idr_p_p.hevc" 3
    run_case offset p "$fixture_dir/hevc_main_640x360_two_slices_idr_p_p.hevc" 3
    run_case chroma p "$fixture_dir/hevc_main_640x360_two_slices_idr_p_p.hevc" 3
    run_case neutral b "$fixture_dir/hevc_main_640x360_two_slices_idr_bbb.hevc" 4
    run_case offset b "$fixture_dir/hevc_main_640x360_two_slices_idr_bbb.hevc" 4
    run_case chroma b "$fixture_dir/hevc_main_640x360_two_slices_idr_bbb.hevc" 4
done
printf 'PASS: two-SPS-RPS weighted P/B (neutral, luma offsets, chroma), %s cycles; artifacts: %s\n' \
    "$cycles" "$output_root"
