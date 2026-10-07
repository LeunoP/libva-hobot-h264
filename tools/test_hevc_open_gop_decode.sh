#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
timeout_bin=${TIMEOUT:-timeout}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
cycles=${HOBOT_HEVC_OPEN_GOP_CYCLES:-5}
timeout_seconds=${HOBOT_HEVC_OPEN_GOP_TIMEOUT:-120}

for bin in "$ffmpeg_bin" "$ffprobe_bin" "$timeout_bin"; do
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
    printf 'HOBOT_HEVC_OPEN_GOP_CYCLES must be a positive integer\n' >&2
    exit 2
}
[[ "$timeout_seconds" =~ ^[1-9][0-9]*$ ]] || {
    printf 'HOBOT_HEVC_OPEN_GOP_TIMEOUT must be a positive integer\n' >&2
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-open-gop.XXXXXX")
fi

require_trace() {
    local trace_log=$1 syntax=$2
    if ! grep -Eq "$syntax" "$trace_log"; then
        printf 'generated stream is missing expected syntax: %s\n' "$syntax" >&2
        exit 1
    fi
}

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi
for pyramid in 0 1; do
    fixture="$output_dir/open-gop-b-pyramid-${pyramid}.hevc"
    trace_log="$output_dir/open-gop-b-pyramid-${pyramid}.trace.log"
    software_hashes="$output_dir/open-gop-b-pyramid-${pyramid}.software.framemd5"
    "$ffmpeg_bin" -hide_banner -loglevel error -f lavfi \
        -i 'testsrc2=size=640x360:rate=30' -frames:v 64 -an \
        -c:v libx265 -preset ultrafast -pix_fmt yuv420p \
        -x265-params \
        "keyint=32:min-keyint=32:scenecut=0:bframes=3:b-adapt=0:b-pyramid=${pyramid}:ref=4:open-gop=1:sao=0:wpp=0:frame-threads=1:pools=none:log-level=error" \
        -f hevc "$fixture"
    "$ffmpeg_bin" -hide_banner -f hevc -i "$fixture" -c:v copy \
        -bsf:v trace_headers -f null - >"$trace_log" 2>&1

    require_trace "$trace_log" 'nal_unit_type[[:space:]]+[^=]*=[[:space:]]*21([[:space:]]|$)'
    require_trace "$trace_log" 'nal_unit_type[[:space:]]+[^=]*=[[:space:]]*8([[:space:]]|$)'
    require_trace "$trace_log" 'num_negative_pics[[:space:]]+[^=]*=[[:space:]]*4([[:space:]]|$)'
    require_trace "$trace_log" 'used_by_curr_pic_s0_flag\[[0-9]+\][[:space:]]+[^=]*=[[:space:]]*0([[:space:]]|$)'
    if ((pyramid == 1)); then
        require_trace "$trace_log" 'nal_unit_type[[:space:]]+[^=]*=[[:space:]]*9([[:space:]]|$)'
    fi

    stream_info=$("$ffprobe_bin" -v error -f hevc -select_streams v:0 -count_frames \
        -show_entries stream=profile,width,height,pix_fmt,nb_read_frames \
        -of csv=p=0 "$fixture")
    if [[ "$stream_info" != 'Main,640,360,yuv420p,64' ]]; then
        printf 'unexpected fixture properties: %s\n' "$stream_info" >&2
        exit 1
    fi
    "$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$fixture" \
        -map 0:v:0 -frames:v 64 -vf format=nv12 -f framemd5 "$software_hashes"

    for ((cycle = 1; cycle <= cycles; cycle++)); do
        printf -v cycle_name 'b-pyramid-%s-cycle-%02d' "$pyramid" "$cycle"
        cycle_dir="$output_dir/$cycle_name"
        mkdir -- "$cycle_dir"
        hardware_hashes="$cycle_dir/hardware.framemd5"
        env "${driver_env[@]}" "$timeout_bin" --foreground --signal=TERM \
            --kill-after=10s "${timeout_seconds}s" "$ffmpeg_bin" \
            -hide_banner -loglevel error -vaapi_device "$drm_device" \
            -hwaccel vaapi -hwaccel_output_format vaapi -f hevc -i "$fixture" \
            -map 0:v:0 -frames:v 64 -vf 'hwdownload,format=nv12' \
            -f framemd5 "$hardware_hashes"
        if ! cmp -s "$software_hashes" "$hardware_hashes"; then
            diff -u "$software_hashes" "$hardware_hashes" || true
            printf 'open-GOP HEVC hardware output differs from software; artifacts: %s\n' \
                "$output_dir" >&2
            exit 1
        fi
        printf 'PASS: HEVC CRA/RASL b-pyramid=%s cycle %02d/%02d, 64 frames bit-exact\n' \
            "$pyramid" "$cycle" "$cycles"
    done
done
printf 'PASS: HEVC CRA/RASL open-GOP regression (%s cycles per B-pyramid variant); artifacts: %s\n' \
    "$cycles" "$output_dir"
