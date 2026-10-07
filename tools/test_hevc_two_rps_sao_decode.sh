#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
cycles=${HOBOT_HEVC_TWO_RPS_SAO_CYCLES:-5}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source_fixture="$script_dir/testdata/hevc_main_640x360_single_sps_rps_sao_p.hevc"
b_fixture="$script_dir/testdata/hevc_main_640x360_two_sps_rps_sao_b.hevc"
generator="$script_dir/generate_hevc_two_rps_sao_fixture.py"
weighted_generator="$script_dir/generate_hevc_neutral_weight_fixture.py"

for bin in "$ffmpeg_bin" "$ffprobe_bin" python3; do
    command -v "$bin" >/dev/null 2>&1 || {
        printf 'missing required command: %s\n' "$bin" >&2
        exit 2
    }
done
"$ffmpeg_bin" -hide_banner -bsfs 2>/dev/null | grep -qx trace_headers || {
    printf 'ffmpeg must include the trace_headers bitstream filter\n' >&2
    exit 2
}
[[ -r "$source_fixture" && -r "$b_fixture" && -r "$generator" &&
   -r "$weighted_generator" ]] || {
    printf 'missing two-RPS SAO fixture source, B fixture, or generator\n' >&2
    exit 2
}
[[ -e "$drm_device" ]] || {
    printf 'DRM device does not exist: %s\n' "$drm_device" >&2
    exit 2
}
[[ "$cycles" =~ ^[1-9][0-9]*$ ]] || {
    printf 'HOBOT_HEVC_TWO_RPS_SAO_CYCLES must be a positive integer\n' >&2
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-two-rps-sao.XXXXXX")
fi

fixture="$output_dir/two-rps-sao.hevc"
trace_log="$output_dir/trace_headers.log"
b_trace_log="$output_dir/b-trace_headers.log"
weighted_b_fixture="$output_dir/two-rps-weighted-sao-b.hevc"
weighted_b_trace_log="$output_dir/weighted-b-trace_headers.log"
weighted_b_chroma_fixture="$output_dir/two-rps-chroma-weighted-sao-b.hevc"
weighted_b_chroma_trace_log="$output_dir/chroma-weighted-b-trace_headers.log"
source_hashes="$output_dir/source-software.framemd5"
software_hashes="$output_dir/software.framemd5"
b_software_hashes="$output_dir/b-software.framemd5"
weighted_b_software_hashes="$output_dir/weighted-b-software.framemd5"
weighted_b_chroma_software_hashes="$output_dir/chroma-weighted-b-software.framemd5"
python3 "$generator" "$source_fixture" "$fixture"
"$ffmpeg_bin" -hide_banner -f hevc -i "$fixture" -c:v copy \
    -bsf:v trace_headers -f null - >"$trace_log" 2>&1
for syntax in \
    'num_short_term_ref_pic_sets[[:space:]]+[^=]*=[[:space:]]*2([[:space:]]|$)' \
    'sample_adaptive_offset_enabled_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'short_term_ref_pic_set_idx[[:space:]]+[^=]*=[[:space:]]*0([[:space:]]|$)' \
    'slice_sao_luma_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'slice_sao_chroma_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)'; do
    if ! grep -Eq "$syntax" "$trace_log"; then
        printf 'transformed stream is missing expected syntax: %s\n' "$syntax" >&2
        exit 1
    fi
done

"$ffmpeg_bin" -hide_banner -f hevc -i "$b_fixture" -c:v copy \
    -bsf:v trace_headers -f null - >"$b_trace_log" 2>&1
for syntax in \
    'num_short_term_ref_pic_sets[[:space:]]+[^=]*=[[:space:]]*2([[:space:]]|$)' \
    'sps_temporal_mvp_enabled_flag[[:space:]]+[^=]*=[[:space:]]*0([[:space:]]|$)' \
    'sample_adaptive_offset_enabled_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'short_term_ref_pic_set_sps_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'short_term_ref_pic_set_idx[[:space:]]+[^=]*=[[:space:]]*0([[:space:]]|$)' \
    'slice_sao_luma_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'slice_sao_chroma_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)'; do
    if ! grep -Eq "$syntax" "$b_trace_log"; then
        printf 'B fixture is missing expected syntax: %s\n' "$syntax" >&2
        exit 1
    fi
done
trace_count() {
    local syntax=$1
    local count
    count=$(grep -Ec "$syntax" "$b_trace_log" || true)
    printf '%s' "$count"
}
b_slices=$(trace_count 'slice_type[[:space:]]+[^=]*=[[:space:]]*0([[:space:]]|$)')
i_slices=$(trace_count 'slice_type[[:space:]]+[^=]*=[[:space:]]*2([[:space:]]|$)')
luma_sao_slices=$(trace_count 'slice_sao_luma_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)')
chroma_sao_slices=$(trace_count 'slice_sao_chroma_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)')
if ((b_slices != 1 || i_slices != 1 ||
     luma_sao_slices != 2 || chroma_sao_slices != 2)); then
    printf 'unexpected B fixture slice/SAO coverage: I/B=%s/%s luma/chroma=%s/%s\n' \
        "$i_slices" "$b_slices" "$luma_sao_slices" "$chroma_sao_slices" >&2
    exit 1
fi

stream_info=$("$ffprobe_bin" -v error -f hevc -select_streams v:0 \
    -count_frames -show_entries stream=width,height,nb_read_frames \
    -of csv=p=0 "$fixture")
if [[ "$stream_info" != '640,360,2' ]]; then
    printf 'unexpected fixture properties: %s\n' "$stream_info" >&2
    exit 1
fi
b_stream_info=$("$ffprobe_bin" -v error -f hevc -select_streams v:0 \
    -count_frames -show_entries stream=profile,width,height,nb_read_frames \
    -of csv=p=0 "$b_fixture")
if [[ "$b_stream_info" != 'Main,640,360,2' ]]; then
    printf 'unexpected two-RPS B fixture properties: %s\n' "$b_stream_info" >&2
    exit 1
fi
"$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$source_fixture" \
    -map 0:v:0 -frames:v 2 -vf format=nv12 -f framemd5 "$source_hashes"
"$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$fixture" \
    -map 0:v:0 -frames:v 2 -vf format=nv12 -f framemd5 "$software_hashes"
if ! cmp -s "$source_hashes" "$software_hashes"; then
    diff -u "$source_hashes" "$software_hashes" || true
    printf 'RPS transformation changed software output; artifacts: %s\n' \
        "$output_dir" >&2
    exit 1
fi
"$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$b_fixture" \
    -map 0:v:0 -frames:v 2 -vf format=nv12 -f framemd5 "$b_software_hashes"
python3 "$weighted_generator" --mode b --weights offset \
    "$b_fixture" "$weighted_b_fixture"
"$ffmpeg_bin" -hide_banner -f hevc -i "$weighted_b_fixture" -c:v copy \
    -bsf:v trace_headers -f null - >"$weighted_b_trace_log" 2>&1
for syntax in \
    'weighted_bipred_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'slice_sao_luma_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'slice_sao_chroma_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'luma_offset_l0\[0\][[:space:]]+[^=]*=[[:space:]]*8([[:space:]]|$)' \
    'luma_offset_l1\[0\][[:space:]]+[^=]*=[[:space:]]*8([[:space:]]|$)'; do
    if ! grep -Eq "$syntax" "$weighted_b_trace_log"; then
        printf 'weighted B-SAO stream is missing expected syntax: %s\n' "$syntax" >&2
        exit 1
    fi
done
"$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$weighted_b_fixture" \
    -map 0:v:0 -frames:v 2 -vf format=nv12 -f framemd5 \
    "$weighted_b_software_hashes"
if cmp -s "$b_software_hashes" "$weighted_b_software_hashes"; then
    printf 'non-default weighted B-SAO table did not change software output\n' >&2
    exit 1
fi
python3 "$weighted_generator" --mode b --weights chroma \
    "$b_fixture" "$weighted_b_chroma_fixture"
"$ffmpeg_bin" -hide_banner -f hevc -i "$weighted_b_chroma_fixture" -c:v copy \
    -bsf:v trace_headers -f null - >"$weighted_b_chroma_trace_log" 2>&1
for syntax in \
    'weighted_bipred_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'slice_sao_luma_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'slice_sao_chroma_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'chroma_weight_l0_flag\[0\][[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'chroma_weight_l1_flag\[0\][[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
    'chroma_offset_l0\[0\]\[0\][[:space:]]+[^=]*=[[:space:]]*8([[:space:]]|$)' \
    'chroma_offset_l0\[0\]\[1\][[:space:]]+[^=]*=[[:space:]]*8([[:space:]]|$)' \
    'chroma_offset_l1\[0\]\[0\][[:space:]]+[^=]*=[[:space:]]*8([[:space:]]|$)' \
    'chroma_offset_l1\[0\]\[1\][[:space:]]+[^=]*=[[:space:]]*8([[:space:]]|$)'; do
    if ! grep -Eq "$syntax" "$weighted_b_chroma_trace_log"; then
        printf 'chroma-weighted B-SAO stream is missing expected syntax: %s\n' \
            "$syntax" >&2
        exit 1
    fi
done
"$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$weighted_b_chroma_fixture" \
    -map 0:v:0 -frames:v 2 -vf format=nv12 -f framemd5 \
    "$weighted_b_chroma_software_hashes"
if cmp -s "$b_software_hashes" "$weighted_b_chroma_software_hashes"; then
    printf 'non-default chroma-weighted B-SAO table did not change software output\n' >&2
    exit 1
fi
driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi
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
        printf 'two-RPS SAO hardware output differs from software; artifacts: %s\n' \
            "$output_dir" >&2
        exit 1
    fi
    printf 'PASS: two-SPS-RPS P-SAO cycle %02d/%02d, bit-exact to software\n' \
        "$cycle" "$cycles"
    b_hardware_hashes="$cycle_dir/b-hardware.framemd5"
    env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
        -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
        -f hevc -i "$b_fixture" -map 0:v:0 -frames:v 2 \
        -vf 'hwdownload,format=nv12' -f framemd5 "$b_hardware_hashes"
    if ! cmp -s "$b_software_hashes" "$b_hardware_hashes"; then
        diff -u "$b_software_hashes" "$b_hardware_hashes" || true
        printf 'two-RPS B-SAO hardware output differs from software; artifacts: %s\n' \
            "$output_dir" >&2
        exit 1
    fi
    printf 'PASS: two-SPS-RPS B-SAO cycle %02d/%02d, bit-exact to software\n' \
        "$cycle" "$cycles"
    weighted_b_hardware_hashes="$cycle_dir/weighted-b-hardware.framemd5"
    env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
        -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
        -f hevc -i "$weighted_b_fixture" -map 0:v:0 -frames:v 2 \
        -vf 'hwdownload,format=nv12' -f framemd5 "$weighted_b_hardware_hashes"
    if ! cmp -s "$weighted_b_software_hashes" "$weighted_b_hardware_hashes"; then
        diff -u "$weighted_b_software_hashes" "$weighted_b_hardware_hashes" || true
        printf 'two-RPS weighted B-SAO hardware output differs from software; artifacts: %s\n' \
            "$output_dir" >&2
        exit 1
    fi
    printf 'PASS: two-SPS-RPS weighted B-SAO cycle %02d/%02d, bit-exact to software\n' \
        "$cycle" "$cycles"
    weighted_b_chroma_hardware_hashes="$cycle_dir/chroma-weighted-b-hardware.framemd5"
    env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
        -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
        -f hevc -i "$weighted_b_chroma_fixture" -map 0:v:0 -frames:v 2 \
        -vf 'hwdownload,format=nv12' -f framemd5 "$weighted_b_chroma_hardware_hashes"
    if ! cmp -s "$weighted_b_chroma_software_hashes" \
        "$weighted_b_chroma_hardware_hashes"; then
        diff -u "$weighted_b_chroma_software_hashes" \
            "$weighted_b_chroma_hardware_hashes" || true
        printf 'two-RPS chroma-weighted B-SAO hardware output differs from software; artifacts: %s\n' \
            "$output_dir" >&2
        exit 1
    fi
    printf 'PASS: two-SPS-RPS chroma-weighted B-SAO cycle %02d/%02d, bit-exact to software\n' \
        "$cycle" "$cycles"
done
printf 'PASS: HEVC two-SPS-RPS P/B and luma/chroma-weighted B SAO regressions (%s cycles each); artifacts: %s\n' \
    "$cycles" "$output_dir"
