#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
cycles=${HOBOT_HEVC_INLINE_REFMOD_CYCLES:-5}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
p_fixture="$script_dir/testdata/hevc_main_640x360_inline_rps_refmod_p.hevc"
b_fixture="$script_dir/testdata/hevc_main_640x360_inline_rps_refmod_b.hevc"

command -v "$ffmpeg_bin" >/dev/null 2>&1 || {
    printf 'missing ffmpeg: %s\n' "$ffmpeg_bin" >&2
    exit 2
}
command -v "$ffprobe_bin" >/dev/null 2>&1 || {
    printf 'missing ffprobe: %s\n' "$ffprobe_bin" >&2
    exit 2
}
[[ "$cycles" =~ ^[1-9][0-9]*$ ]] || {
    printf 'HOBOT_HEVC_INLINE_REFMOD_CYCLES must be a positive integer\n' >&2
    exit 2
}
for fixture in "$p_fixture" "$b_fixture"; do
    [[ -r "$fixture" ]] || {
        printf 'missing HEVC test fixture: %s\n' "$fixture" >&2
        exit 2
    }
done
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-inline-refmod.XXXXXX")
fi

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

validate_trace() {
    local fixture=$1
    local mode=$2
    local trace_log=$3

    "$ffmpeg_bin" -hide_banner -loglevel trace -f hevc -i "$fixture" \
        -c:v copy -bsf:v trace_headers -f null - 2>"$trace_log"
    if ! grep -Eq 'num_short_term_ref_pic_sets[[:space:]]+1 = 0' "$trace_log" || \
       ! grep -Eq 'lists_modification_present_flag[[:space:]]+1 = 1' "$trace_log"; then
        printf 'fixture lacks zero-SPS-RPS/list-modification syntax: %s\n' "$fixture" >&2
        return 1
    fi

    if [[ "$mode" == p ]]; then
        awk '
            /Slice Segment Header/ { is_b = 0; l0_modified = 0; entry0 = ""; entry1 = "" }
            /slice_type[[:space:]]/ { is_b = ($NF == 0) }
            /ref_pic_list_modification_flag_l0/ && $NF == 1 { l0_modified = 1 }
            /list_entry_l0\[0\]/ { entry0 = $NF }
            /list_entry_l0\[1\]/ { entry1 = $NF }
            /five_minus_max_num_merge_cand/ && !is_b && l0_modified &&
                entry0 == 1 && entry1 == 0 { found = 1 }
            END { exit !found }
        ' "$trace_log" || {
            printf 'fixture lacks P-slice L0 reordering [1,0]\n' >&2
            return 1
        }
    else
        awk '
            /Slice Segment Header/ {
                is_b = 0; l0_modified = 0; l1_modified = 0
                l00 = ""; l01 = ""; l02 = ""; l10 = ""; l11 = ""; l12 = ""
            }
            /slice_type[[:space:]]/ { is_b = ($NF == 0) }
            /ref_pic_list_modification_flag_l0/ && $NF == 1 { l0_modified = 1 }
            /ref_pic_list_modification_flag_l1/ && $NF == 1 { l1_modified = 1 }
            /list_entry_l0\[0\]/ { l00 = $NF }
            /list_entry_l0\[1\]/ { l01 = $NF }
            /list_entry_l0\[2\]/ { l02 = $NF }
            /list_entry_l1\[0\]/ { l10 = $NF }
            /list_entry_l1\[1\]/ { l11 = $NF }
            /list_entry_l1\[2\]/ { l12 = $NF }
            /five_minus_max_num_merge_cand/ && is_b && l0_modified && l1_modified &&
                l00 == 1 && l01 == 2 && l02 == 0 &&
                l10 == 1 && l11 == 2 && l12 == 0 { found = 1 }
            END { exit !found }
        ' "$trace_log" || {
            printf 'fixture lacks B-slice L0/L1 reordering [1,2,0] with three references\n' >&2
            return 1
        }
    fi
}

run_case() {
    local label=$1
    local mode=$2
    local fixture=$3
    local case_dir="$output_dir/$label"
    mkdir -p -- "$case_dir"

    local stream_info
    stream_info=$("$ffprobe_bin" -v error -f hevc -select_streams v:0 \
        -show_entries stream=profile,width,height,pix_fmt -of csv=p=0 "$fixture")
    if [[ "$stream_info" != 'Main,640,360,yuv420p' ]]; then
        printf 'unexpected test fixture properties: %s\n' "$stream_info" >&2
        return 1
    fi

    validate_trace "$fixture" "$mode" "$case_dir/trace_headers.log"
    local software_hashes="$case_dir/software.framemd5"
    "$ffmpeg_bin" -xerror -hide_banner -loglevel error -f hevc -i "$fixture" \
        -map 0:v:0 -frames:v 8 -vf format=nv12 -f framemd5 "$software_hashes"
    local frame_count
    frame_count=$(awk '!/^#/ && NF { count++ } END { print count + 0 }' \
        "$software_hashes")
    if [[ "$frame_count" -ne 8 ]]; then
        printf 'expected 8 software-decoded pictures in %s, got %s\n' \
            "$label" "$frame_count" >&2
        return 1
    fi

    for ((cycle = 1; cycle <= cycles; cycle++)); do
        local cycle_name hardware_hashes hardware_count
        cycle_name=$(printf '%02d' "$cycle")
        hardware_hashes="$case_dir/cycle_${cycle_name}.framemd5"
        env "${driver_env[@]}" "$ffmpeg_bin" -xerror -hide_banner -loglevel error \
            -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
            -f hevc -i "$fixture" -map 0:v:0 -frames:v 8 \
            -vf 'hwdownload,format=nv12' -f framemd5 "$hardware_hashes" \
            2>"$case_dir/cycle_${cycle_name}.log"
        hardware_count=$(awk '!/^#/ && NF { count++ } END { print count + 0 }' \
            "$hardware_hashes")
        if [[ "$hardware_count" -ne 8 ]] || \
           ! diff -u \
               <(awk -F, '!/^#/ { print $NF }' "$software_hashes") \
               <(awk -F, '!/^#/ { print $NF }' "$hardware_hashes"); then
            printf '%s inline-RPS output differs from software at cycle %s; artifacts: %s\n' \
                "$label" "$cycle_name" "$case_dir" >&2
            return 1
        fi
        printf '%s cycle %s/%s PASS\n' "$label" "$cycle_name" "$cycles"
    done
}

run_case p-l0 p "$p_fixture"
run_case b-l0-l1 b "$b_fixture"

printf 'PASS: HEVC Main inline-RPS P L0 [1,0] and B L0/L1 [1,2,0] (3 refs), 8 pictures each, %s fresh VPU sessions per fixture; artifacts: %s\n' \
    "$cycles" "$output_dir"
