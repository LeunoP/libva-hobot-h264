#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
cycles=${HOBOT_HEVC_THREE_RPS_CYCLES:-5}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
generator="$script_dir/generate_hevc_three_rps_fixture.py"

for bin in "$ffmpeg_bin" "$ffprobe_bin" python3; do
    command -v "$bin" >/dev/null 2>&1 || {
        printf 'missing required command: %s\n' "$bin" >&2
        exit 2
    }
done
grep -qx trace_headers < <("$ffmpeg_bin" -hide_banner -bsfs 2>/dev/null) || {
    printf 'ffmpeg must include the trace_headers bitstream filter\n' >&2
    exit 2
}
grep -Eq '[[:space:]]libx265[[:space:]]' < <("$ffmpeg_bin" -hide_banner -encoders 2>/dev/null) || {
    printf 'ffmpeg must include the libx265 encoder\n' >&2
    exit 2
}
[[ -r "$generator" && -e "$drm_device" ]] || {
    printf 'missing fixture generator or DRM device: %s\n' "$drm_device" >&2
    exit 2
}
[[ "$cycles" =~ ^[1-9][0-9]*$ ]] || {
    printf 'HOBOT_HEVC_THREE_RPS_CYCLES must be a positive integer\n' >&2
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-three-rps.XXXXXX")
fi

source_fixture="$output_dir/source-inline-rps.hevc"
source_hashes="$output_dir/source-software.framemd5"
set_counts=(3 64)
rps_indices=(2 63)
case_names=(three-rps-index-2 max-rps-index-63)

"$ffmpeg_bin" -hide_banner -loglevel error -y -f lavfi \
    -i 'testsrc2=size=640x360:rate=30:duration=0.1' -frames:v 2 -an \
    -c:v libx265 -preset ultrafast -pix_fmt yuv420p -bf 0 \
    -x265-params \
    'log-level=error:pools=none:frame-threads=1:bframes=0:ref=1:weightp=0:weightb=0:sao=1:temporal-mvp=1:keyint=24:min-keyint=24:scenecut=0:open-gop=0' \
    -f hevc "$source_fixture"
"$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$source_fixture" \
    -map 0:v:0 -frames:v 2 -vf format=nv12 -f framemd5 "$source_hashes"

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
for case_index in "${!set_counts[@]}"; do
    set_count=${set_counts[$case_index]}
    rps_index=${rps_indices[$case_index]}
    case_name=${case_names[$case_index]}
    fixture="$output_dir/$case_name.hevc"
    expected_hashes="$output_dir/$case_name-software.framemd5"
    trace_log="$output_dir/$case_name.trace_headers.log"
    python3 "$generator" "$source_fixture" "$fixture" \
        --set-count "$set_count" --rps-index "$rps_index"

    "$ffmpeg_bin" -hide_banner -f hevc -i "$fixture" -c:v copy \
        -bsf:v trace_headers -f null - >"$trace_log" 2>&1
    for syntax in \
        "num_short_term_ref_pic_sets[[:space:]]+[^=]*=[[:space:]]*$set_count([[:space:]]|$)" \
        'short_term_ref_pic_set_sps_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
        "short_term_ref_pic_set_idx[[:space:]]+[^=]*=[[:space:]]*$rps_index([[:space:]]|$)" \
        'slice_temporal_mvp_enabled_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)'; do
        grep -Eq "$syntax" "$trace_log" || {
            printf 'generated stream is missing expected syntax: %s\n' "$syntax" >&2
            exit 1
        }
    done
    stream_info=$("$ffprobe_bin" -v error -f hevc -select_streams v:0 \
        -count_frames -show_entries stream=profile,width,height,nb_read_frames \
        -of csv=p=0 "$fixture")
    [[ "$stream_info" == 'Main,640,360,2' ]] || {
        printf 'unexpected fixture properties: %s\n' "$stream_info" >&2
        exit 1
    }
    "$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$fixture" \
        -map 0:v:0 -frames:v 2 -vf format=nv12 -f framemd5 "$expected_hashes"
    if ! cmp -s "$source_hashes" "$expected_hashes"; then
        diff -u "$source_hashes" "$expected_hashes" || true
        printf '%s transformation changed software output\n' "$case_name" >&2
        exit 1
    fi

    for ((cycle = 1; cycle <= cycles; cycle++)); do
        printf -v cycle_name '%s-cycle-%02d' "$case_name" "$cycle"
        cycle_dir="$output_dir/$cycle_name"
        mkdir -- "$cycle_dir"
        hardware_hashes="$cycle_dir/hardware.framemd5"
        env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
            -vaapi_device "$drm_device" -hwaccel vaapi \
            -hwaccel_output_format vaapi -f hevc -i "$fixture" \
            -map 0:v:0 -frames:v 2 -vf 'hwdownload,format=nv12' \
            -f framemd5 "$hardware_hashes"
        if ! cmp -s "$expected_hashes" "$hardware_hashes"; then
            diff -u "$expected_hashes" "$hardware_hashes" || true
            printf '%s hardware output differs; artifacts: %s\n' \
                "$case_name" "$output_dir" >&2
            exit 1
        fi
        printf 'PASS: %s cycle %02d/%02d, bit-exact to software\n' \
            "$case_name" "$cycle" "$cycles"
    done
done
printf 'PASS: HEVC SPS-RPS count/index regressions (%s cycles each); artifacts: %s\n' \
    "$cycles" "$output_dir"
