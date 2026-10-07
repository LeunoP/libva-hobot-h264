#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
cycles=${HOBOT_HEVC_TWO_RPS_B_TMVP_CYCLES:-5}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fixture="$script_dir/testdata/hevc_main_640x360_two_sps_rps_tmvp_b.hevc"

for bin in "$ffmpeg_bin" "$ffprobe_bin"; do
    command -v "$bin" >/dev/null 2>&1 || {
        printf 'missing required command: %s\n' "$bin" >&2
        exit 2
    }
done
grep -qx trace_headers < <("$ffmpeg_bin" -hide_banner -bsfs 2>/dev/null) || {
    printf 'ffmpeg must include the trace_headers bitstream filter\n' >&2
    exit 2
}
[[ -r "$fixture" && -e "$drm_device" ]] || {
    printf 'missing fixture or DRM device: %s\n' "$drm_device" >&2
    exit 2
}
[[ "$cycles" =~ ^[1-9][0-9]*$ ]] || {
    printf 'HOBOT_HEVC_TWO_RPS_B_TMVP_CYCLES must be a positive integer\n' >&2
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-two-rps-b-tmvp.XXXXXX")
fi

trace="$output_dir/fixture.trace_headers.log"
"$ffmpeg_bin" -hide_banner -f hevc -i "$fixture" -c:v copy \
    -bsf:v trace_headers -f null - >"$trace" 2>&1

count_syntax() {
    local name=$1 value=$2
    grep -Ec "[[:space:]]${name}[[:space:]]+[^=]*=[[:space:]]*${value}([[:space:]]|$)" "$trace" || true
}
expect_count() {
    local name=$1 value=$2 expected=$3 actual
    actual=$(count_syntax "$name" "$value")
    [[ "$actual" -eq "$expected" ]] || {
        printf 'unexpected %s=%s syntax count: expected %s, got %s\n' \
            "$name" "$value" "$expected" "$actual" >&2
        exit 1
    }
}
expect_min_count() {
    local name=$1 value=$2 minimum=$3 actual
    actual=$(count_syntax "$name" "$value")
    ((actual >= minimum)) || {
        printf 'unexpected %s=%s syntax count: expected at least %s, got %s\n' \
            "$name" "$value" "$minimum" "$actual" >&2
        exit 1
    }
}

expect_count slice_type 0 4
expect_count slice_type 2 1
expect_count short_term_ref_pic_set_sps_flag 1 4
expect_count short_term_ref_pic_set_idx 0 4
expect_count slice_temporal_mvp_enabled_flag 1 4
expect_min_count slice_sao_luma_flag 1 4
expect_min_count slice_sao_chroma_flag 1 4
expect_count num_ref_idx_active_override_flag 0 4
expect_count mvd_l1_zero_flag 1 4
expect_count collocated_from_l0_flag 0 4
for syntax in \
    'num_short_term_ref_pic_sets[[:space:]]+[^=]*=[[:space:]]*2([[:space:]]|$)' \
    'num_negative_pics[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'num_positive_pics[[:space:]]+[^=]*=[[:space:]]*0([[:space:]]|$)' \
    'delta_poc_s0_minus1\[0\][[:space:]]+[^=]*=[[:space:]]*0([[:space:]]|$)' \
    'used_by_curr_pic_s0_flag\[0\][[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)'; do
    grep -Eq "$syntax" "$trace" || {
        printf 'fixture is missing expected RPS syntax: %s\n' "$syntax" >&2
        exit 1
    }
done

stream_info=$("$ffprobe_bin" -v error -f hevc -select_streams v:0 \
    -count_frames -show_entries stream=profile,width,height,nb_read_frames \
    -of csv=p=0 "$fixture")
[[ "$stream_info" == 'Main,640,360,5' ]] || {
    printf 'unexpected fixture properties: %s\n' "$stream_info" >&2
    exit 1
}

software_hashes="$output_dir/software.framemd5"
"$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$fixture" \
    -map 0:v:0 -frames:v 5 -vf format=nv12 -f framemd5 "$software_hashes"

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi
for ((cycle = 1; cycle <= cycles; cycle++)); do
    printf -v cycle_name 'cycle-%02d' "$cycle"
    hardware_hashes="$output_dir/$cycle_name-hardware.framemd5"
    env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
        -vaapi_device "$drm_device" -hwaccel vaapi \
        -hwaccel_output_format vaapi -f hevc -i "$fixture" \
        -map 0:v:0 -frames:v 5 -vf 'hwdownload,format=nv12' \
        -f framemd5 "$hardware_hashes"
    if ! cmp -s "$software_hashes" "$hardware_hashes"; then
        diff -u "$software_hashes" "$hardware_hashes" || true
        printf 'B-slice temporal-MVP output differs from software; artifacts: %s\n' \
            "$output_dir" >&2
        exit 1
    fi
    printf 'PASS: two-SPS-RPS B temporal-MVP cycle %02d/%02d, bit-exact to software\n' \
        "$cycle" "$cycles"
done
printf 'PASS: narrow HEVC B temporal-MVP regression (%s fresh VPU sessions); artifacts: %s\n' \
    "$cycles" "$output_dir"
