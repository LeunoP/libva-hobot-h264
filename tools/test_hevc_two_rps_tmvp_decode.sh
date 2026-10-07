#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
cycles=${HOBOT_HEVC_TWO_RPS_TMVP_CYCLES:-5}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
generator="$script_dir/generate_hevc_two_rps_tmvp_fixture.py"

for bin in "$ffmpeg_bin" "$ffprobe_bin" python3; do
    command -v "$bin" >/dev/null 2>&1 || {
        printf 'missing required command: %s\n' "$bin" >&2
        exit 2
    }
done
bsf_list=$("$ffmpeg_bin" -hide_banner -bsfs 2>/dev/null)
encoder_list=$("$ffmpeg_bin" -hide_banner -encoders 2>/dev/null)
grep -qx trace_headers <<<"$bsf_list" || {
    printf 'ffmpeg must include the trace_headers bitstream filter\n' >&2
    exit 2
}
grep -Eq '[[:space:]]libx265[[:space:]]' <<<"$encoder_list" || {
    printf 'ffmpeg must include the libx265 encoder\n' >&2
    exit 2
}
[[ -r "$generator" && -e "$drm_device" ]] || {
    printf 'missing fixture generator or DRM device: %s\n' "$drm_device" >&2
    exit 2
}
[[ "$cycles" =~ ^[1-9][0-9]*$ ]] || {
    printf 'HOBOT_HEVC_TWO_RPS_TMVP_CYCLES must be a positive integer\n' >&2
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-two-rps-tmvp.XXXXXX")
fi

source_fixture="$output_dir/source-inline-rps.hevc"
selected_fixture="$output_dir/two-rps-selected-tmvp.hevc"
inline_fixture="$output_dir/two-rps-inline-tmvp.hevc"
source_hashes="$output_dir/source-software.framemd5"
selected_hashes="$output_dir/selected-software.framemd5"
inline_hashes="$output_dir/inline-software.framemd5"

"$ffmpeg_bin" -hide_banner -loglevel error -y -f lavfi \
    -i 'testsrc2=size=640x360:rate=30:duration=0.1' -frames:v 2 -an \
    -c:v libx265 -preset ultrafast -pix_fmt yuv420p -bf 0 \
    -x265-params \
    'log-level=error:pools=none:frame-threads=1:bframes=0:ref=1:weightp=0:weightb=0:sao=1:temporal-mvp=1:keyint=24:min-keyint=24:scenecut=0:open-gop=0' \
    -f hevc "$source_fixture"

python3 "$generator" "$source_fixture" "$selected_fixture" \
    --inline-output "$inline_fixture"

check_fixture() {
    local fixture=$1
    local expected_flag=$2
    local trace
    trace="$output_dir/$(basename "$fixture").trace_headers.log"
    "$ffmpeg_bin" -hide_banner -f hevc -i "$fixture" -c:v copy \
        -bsf:v trace_headers -f null - >"$trace" 2>&1
    for syntax in \
        'num_short_term_ref_pic_sets[[:space:]]+[^=]*=[[:space:]]*2([[:space:]]|$)' \
        'sps_temporal_mvp_enabled_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
        "short_term_ref_pic_set_sps_flag[[:space:]]+[^=]*=[[:space:]]*$expected_flag([[:space:]]|$)" \
        'slice_temporal_mvp_enabled_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
        'slice_sao_luma_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)' \
        'slice_sao_chroma_flag[[:space:]]+[^=]*=[[:space:]]*1([[:space:]]|$)'; do
        if ! grep -Eq "$syntax" "$trace"; then
            printf 'fixture is missing expected syntax (%s): %s\n' \
                "$fixture" "$syntax" >&2
            exit 1
        fi
    done
    local stream_info
    stream_info=$("$ffprobe_bin" -v error -f hevc -select_streams v:0 \
        -count_frames -show_entries stream=profile,width,height,nb_read_frames \
        -of csv=p=0 "$fixture")
    [[ "$stream_info" == 'Main,640,360,2' ]] || {
        printf 'unexpected fixture properties: %s: %s\n' \
            "$fixture" "$stream_info" >&2
        exit 1
    }
}

check_fixture "$selected_fixture" 1
check_fixture "$inline_fixture" 0
"$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$source_fixture" \
    -map 0:v:0 -frames:v 2 -vf format=nv12 -f framemd5 "$source_hashes"
"$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$selected_fixture" \
    -map 0:v:0 -frames:v 2 -vf format=nv12 -f framemd5 "$selected_hashes"
"$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$inline_fixture" \
    -map 0:v:0 -frames:v 2 -vf format=nv12 -f framemd5 "$inline_hashes"
cmp -s "$source_hashes" "$selected_hashes" || {
    diff -u "$source_hashes" "$selected_hashes" || true
    printf 'selected-RPS transformation changed software output\n' >&2
    exit 1
}
cmp -s "$source_hashes" "$inline_hashes" || {
    diff -u "$source_hashes" "$inline_hashes" || true
    printf 'inline-RPS transformation changed software output\n' >&2
    exit 1
}

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
    for variant in selected inline; do
        if [[ "$variant" == selected ]]; then
            fixture=$selected_fixture
            expected_hashes=$selected_hashes
        else
            fixture=$inline_fixture
            expected_hashes=$inline_hashes
        fi
        hardware_hashes="$cycle_dir/$variant-hardware.framemd5"
        env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
            -vaapi_device "$drm_device" -hwaccel vaapi \
            -hwaccel_output_format vaapi -f hevc -i "$fixture" \
            -map 0:v:0 -frames:v 2 -vf 'hwdownload,format=nv12' \
            -f framemd5 "$hardware_hashes"
        if ! cmp -s "$expected_hashes" "$hardware_hashes"; then
            diff -u "$expected_hashes" "$hardware_hashes" || true
            printf '%s-RPS temporal-MVP hardware output differs; artifacts: %s\n' \
                "$variant" "$output_dir" >&2
            exit 1
        fi
        printf 'PASS: two-SPS-RPS %s temporal-MVP cycle %02d/%02d, bit-exact to software\n' \
            "$variant" "$cycle" "$cycles"
    done
done
printf 'PASS: HEVC two-SPS-RPS temporal-MVP selected/inline regressions (%s cycles each); artifacts: %s\n' \
    "$cycles" "$output_dir"
