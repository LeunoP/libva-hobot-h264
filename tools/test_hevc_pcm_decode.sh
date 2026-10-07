#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
sha256_bin=${SHA256SUM:-sha256sum}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
cycles=${HOBOT_HEVC_PCM_CYCLES:-5}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fixture_dir="$script_dir/testdata"

for bin in "$ffmpeg_bin" "$ffprobe_bin" "$sha256_bin"; do
    command -v "$bin" >/dev/null 2>&1 || {
        printf 'missing required command: %s\n' "$bin" >&2
        exit 2
    }
done
bsf_list=$("$ffmpeg_bin" -hide_banner -bsfs 2>/dev/null)
if ! grep -qx trace_headers <<<"$bsf_list"; then
    printf 'ffmpeg must include the trace_headers bitstream filter\n' >&2
    exit 2
fi
[[ -e "$drm_device" ]] || {
    printf 'DRM device does not exist: %s\n' "$drm_device" >&2
    exit 2
}
[[ "$cycles" =~ ^[1-9][0-9]*$ ]] || {
    printf 'HOBOT_HEVC_PCM_CYCLES must be a positive integer\n' >&2
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-pcm.XXXXXX")
fi

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

require_trace() {
    local trace_log=$1 pattern=$2
    if ! grep -Eq "$pattern" "$trace_log"; then
        printf 'PCM fixture is missing expected syntax: %s (%s)\n' \
            "$pattern" "$trace_log" >&2
        exit 1
    fi
}

run_case() {
    local name=$1 fixture=$2 loop_filter_disabled=$3
    local pcm_min_log2_minus3=$4 pcm_log2_diff=$5
    local expected_sha256=$6
    local case_dir="$output_dir/$name"
    local trace_log="$case_dir/trace_headers.log"
    local software_hashes="$case_dir/software.framemd5"
    mkdir -- "$case_dir"

    local actual_sha256
    actual_sha256=$("$sha256_bin" "$fixture" | awk '{print $1}')
    if [[ "$actual_sha256" != "$expected_sha256" ]]; then
        printf 'unexpected HEVC PCM fixture hash: %s (%s)\n' \
            "$actual_sha256" "$fixture" >&2
        exit 1
    fi

    local stream_info
    stream_info=$("$ffprobe_bin" -v error -f hevc -count_frames -select_streams v:0 \
        -show_entries stream=profile,width,height,pix_fmt,nb_read_frames \
        -of csv=p=0 "$fixture")
    if [[ "$stream_info" != 'Main,64,64,yuv420p,1' ]]; then
        printf 'unexpected HEVC PCM fixture properties: %s (%s)\n' \
            "$stream_info" "$fixture" >&2
        exit 1
    fi

    "$ffmpeg_bin" -hide_banner -loglevel trace -i "$fixture" -c:v copy \
        -bsf:v trace_headers -f null - >"$trace_log" 2>&1
    require_trace "$trace_log" 'pcm_enabled_flag[[:space:]]+1[[:space:]]*=[[:space:]]*1'
    require_trace "$trace_log" 'pcm_sample_bit_depth_luma_minus1[[:space:]]+0111[[:space:]]*=[[:space:]]*7'
    require_trace "$trace_log" 'pcm_sample_bit_depth_chroma_minus1[[:space:]]+0111[[:space:]]*=[[:space:]]*7'
    require_trace "$trace_log" \
        "log2_min_pcm_luma_coding_block_size_minus3[[:space:]]+[01]+[[:space:]]*=[[:space:]]*$pcm_min_log2_minus3"
    require_trace "$trace_log" \
        "log2_diff_max_min_pcm_luma_coding_block_size[[:space:]]+[01]+[[:space:]]*=[[:space:]]*$pcm_log2_diff"
    require_trace "$trace_log" \
        "pcm_loop_filter_disabled_flag[[:space:]]+[01][[:space:]]*=[[:space:]]*$loop_filter_disabled"

    "$ffmpeg_bin" -hide_banner -loglevel error -y -f hevc -i "$fixture" \
        -map 0:v:0 -frames:v 1 -vf format=nv12 -f framemd5 "$software_hashes"

    local cycle hardware_hashes
    for ((cycle = 1; cycle <= cycles; cycle++)); do
        printf -v hardware_hashes '%s/hardware-cycle-%02d.framemd5' "$case_dir" "$cycle"
        env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error -y \
            -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
            -f hevc -i "$fixture" -map 0:v:0 -frames:v 1 \
            -vf 'hwdownload,format=nv12' -f framemd5 "$hardware_hashes"
        if ! cmp -s "$software_hashes" "$hardware_hashes"; then
            diff -u "$software_hashes" "$hardware_hashes" || true
            printf 'HEVC PCM hardware output differs from software (%s cycle %s); artifacts: %s\n' \
                "$name" "$cycle" "$case_dir" >&2
            exit 1
        fi
        printf 'PASS: HEVC PCM %s cycle %02d/%02d, bit-exact\n' \
            "$name" "$cycle" "$cycles"
    done
}

run_case loop-filter-on-32 \
    "$fixture_dir/hevc_main_64x64_pcm_loop_filter_on.hevc" 0 0 2 \
    7b7c20256eed0a661eee568f594deafc6b26dbdef2877bfd89a5365c41ecce40
run_case loop-filter-off-32 \
    "$fixture_dir/hevc_main_64x64_pcm_loop_filter_off.hevc" 1 0 2 \
    cf1f8f6c217ed577a33f4ece288224ca9f74c34171b62eddee34a0b6b9cc9be4
run_case loop-filter-on-8 \
    "$fixture_dir/hevc_main_64x64_pcm_8x8.hevc" 0 0 0 \
    3dbc94847b3fcf307ffa67881986190f5c720732fedafd94ba3ca53ab38ba4db
run_case loop-filter-off-16 \
    "$fixture_dir/hevc_main_64x64_pcm_16x16.hevc" 1 1 0 \
    9844283462bf11b8563ff83988462b446a78a6b9ba8f797165a31f9d269bf04e

printf 'PASS: HEVC PCM sizes 8/16/32 and loop-filter variants (%s cycles each); artifacts: %s\n' \
    "$cycles" "$output_dir"
