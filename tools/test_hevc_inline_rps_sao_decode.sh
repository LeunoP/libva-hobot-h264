#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
cycles=${HOBOT_HEVC_INLINE_SAO_CYCLES:-5}

for bin in "$ffmpeg_bin" "$ffprobe_bin"; do
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
encoder_list=$("$ffmpeg_bin" -hide_banner -encoders 2>/dev/null)
if ! grep -Eq '[[:space:]]libx265[[:space:]]' <<<"$encoder_list"; then
    printf 'ffmpeg must include the libx265 encoder\n' >&2
    exit 2
fi
[[ -e "$drm_device" ]] || {
    printf 'DRM device does not exist: %s\n' "$drm_device" >&2
    exit 2
}
[[ "$cycles" =~ ^[1-9][0-9]*$ ]] || {
    printf 'HOBOT_HEVC_INLINE_SAO_CYCLES must be a positive integer\n' >&2
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-inline-sao.XXXXXX")
fi

fixture="$output_dir/inline-rps-sao.hevc"
trace_log="$output_dir/trace_headers.log"
software_hashes="$output_dir/software.framemd5"
"$ffmpeg_bin" -hide_banner -loglevel error -f lavfi \
    -i 'testsrc2=size=640x360:rate=30:duration=0.2' -frames:v 6 -an \
    -c:v libx265 -preset ultrafast -pix_fmt yuv420p \
    -x265-params \
    'keyint=12:min-keyint=12:scenecut=0:bframes=2:b-adapt=0:b-pyramid=0:ref=1:sao=1:strong-intra-smoothing=0:wpp=0:frame-threads=1:pools=none:log-level=error' \
    -f hevc "$fixture"
"$ffmpeg_bin" -hide_banner -f hevc -i "$fixture" -c:v copy \
    -bsf:v trace_headers -f null - >"$trace_log" 2>&1

require_trace() {
    local syntax=$1
    if ! grep -Eq "$syntax" "$trace_log"; then
        printf 'generated stream is missing expected syntax: %s\n' "$syntax" >&2
        exit 1
    fi
}
trace_count() {
    local syntax=$1
    local count
    count=$(grep -Ec "$syntax" "$trace_log" || true)
    printf '%s' "$count"
}
require_trace 'num_short_term_ref_pic_sets[[:space:]]+[^=]*=[[:space:]]*0([[:space:]]|$)'
require_trace 'sample_adaptive_offset_enabled_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)'
require_trace 'short_term_ref_pic_set_sps_flag[[:space:]]+[^=]*=[[:space:]]*0([[:space:]]|$)'
b_slices=$(trace_count 'slice_type[[:space:]]+[^=]*=[[:space:]]*0([[:space:]]|$)')
p_slices=$(trace_count 'slice_type[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)')
i_slices=$(trace_count 'slice_type[[:space:]]+[^=]*=[[:space:]]*2([[:space:]]|$)')
luma_sao_slices=$(trace_count 'slice_sao_luma_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)')
chroma_sao_slices=$(trace_count 'slice_sao_chroma_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)')
if ((b_slices == 0 || p_slices == 0 || i_slices == 0 ||
     b_slices + p_slices + i_slices != 6 ||
     luma_sao_slices != 6 || chroma_sao_slices != 6)); then
    printf 'unexpected slice/SAO coverage: I/P/B=%s/%s/%s luma/chroma=%s/%s\n' \
        "$i_slices" "$p_slices" "$b_slices" \
        "$luma_sao_slices" "$chroma_sao_slices" >&2
    exit 1
fi

stream_info=$("$ffprobe_bin" -v error -f hevc -select_streams v:0 \
    -count_frames -show_entries stream=profile,width,height,nb_read_frames \
    -of csv=p=0 "$fixture")
if [[ "$stream_info" != 'Main,640,360,6' ]]; then
    printf 'unexpected fixture properties: %s\n' "$stream_info" >&2
    exit 1
fi
"$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$fixture" \
    -map 0:v:0 -frames:v 6 -vf format=nv12 -f framemd5 "$software_hashes"

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
        -f hevc -i "$fixture" -map 0:v:0 -frames:v 6 \
        -vf 'hwdownload,format=nv12' -f framemd5 "$hardware_hashes"
    if ! cmp -s "$software_hashes" "$hardware_hashes"; then
        diff -u "$software_hashes" "$hardware_hashes" || true
        printf 'inline-RPS SAO hardware output differs from software; artifacts: %s\n' \
            "$output_dir" >&2
        exit 1
    fi
    printf 'PASS: inline-RPS I/P/B SAO cycle %02d/%02d, bit-exact to software\n' \
        "$cycle" "$cycles"
done
printf 'PASS: HEVC inline-RPS I/P/B SAO regression (%s cycles); artifacts: %s\n' \
    "$cycles" "$output_dir"
