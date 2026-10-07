#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$script_dir/.." && pwd)
ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
test_build_dir=${TEST_BUILD_DIR:-/tmp/libva-hobot-tests}
output_dir=${1:-$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-wpp-encode.XXXXXX")}

if [[ $# -gt 1 ]]; then
    printf 'usage: %s [new-output-directory]\n' "$0" >&2
    exit 2
fi
if [[ $# -eq 1 ]]; then
    [[ ! -e "$output_dir" ]] || {
        printf 'output path already exists: %s\n' "$output_dir" >&2
        exit 2
    }
    mkdir -p -- "$output_dir"
fi
for tool in "$ffmpeg_bin" "$ffprobe_bin" make; do
    command -v "$tool" >/dev/null 2>&1 || {
        printf 'missing required command: %s\n' "$tool" >&2
        exit 2
    }
done
[[ -e "$drm_device" ]] || {
    printf 'DRM device does not exist: %s\n' "$drm_device" >&2
    exit 2
}
"$ffmpeg_bin" -hide_banner -bsfs 2>/dev/null | grep -qx trace_headers || {
    printf 'ffmpeg must include the trace_headers bitstream filter\n' >&2
    exit 2
}

stream="$output_dir/hevc-wpp-ip.hevc"
trace="$output_dir/trace_headers.log"
software_hashes="$output_dir/software.framemd5"
hardware_hashes="$output_dir/hardware.framemd5"
driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

make -C "$repo_dir" "TEST_BUILD_DIR=$test_build_dir" \
    "$test_build_dir/test_hevc_wpp_encode"
env "${driver_env[@]}" "$test_build_dir/test_hevc_wpp_encode" "$stream" 1
"$ffmpeg_bin" -hide_banner -f hevc -i "$stream" -c:v copy \
    -bsf:v trace_headers -f null - >"$trace" 2>&1
grep -Eq 'entropy_coding_sync_enabled_flag[^=]*=[[:space:]]*1([[:space:]]|$)' "$trace"
grep -Eq 'tiles_enabled_flag[^=]*=[[:space:]]*0([[:space:]]|$)' "$trace"
if ! awk '/\[trace_headers @/ && /dependent_slice_segment_flag/ {
        count++
        if ($NF != 1) bad = 1
    }
    END { exit !(count == 10 && !bad) }' "$trace"; then
    printf 'WPP I/P output did not contain five dependent row segments per frame: %s\n' \
        "$trace" >&2
    exit 1
fi
for address in 10 20 30 40 50; do
    if ! awk -v expected="$address" \
        '/\[trace_headers @/ && /slice_segment_address/ && $NF == expected {
            count++
        }
        END { exit !(count == 2) }' "$trace"; then
        printf 'WPP row address %s was not present once per frame: %s\n' \
            "$address" "$trace" >&2
        exit 1
    fi
done
if ! awk '/\[trace_headers @/ && /num_entry_point_offsets/ {
        count++
        if ($NF != 0) bad = 1
    }
    END { exit !(count == 12 && !bad) }' "$trace"; then
    printf 'unexpected WPP slice entry-point layout: %s\n' "$trace" >&2
    exit 1
fi
stream_info=$("$ffprobe_bin" -v error -f hevc -select_streams v:0 \
    -count_frames -show_entries stream=profile,width,height,nb_read_frames \
    -of csv=p=0 "$stream")
[[ "$stream_info" == 'Main,640,360,2' ]] || {
    printf 'unexpected WPP stream properties: %s\n' "$stream_info" >&2
    exit 1
}
frame_types=$("$ffprobe_bin" -v error -f hevc -select_streams v:0 \
    -show_entries frame=pict_type -of csv=p=0 "$stream")
[[ "$frame_types" == $'I\nP' ]] || {
    printf 'expected an I/P stream, got: %s\n' "$frame_types" >&2
    exit 1
}
if ! awk '/\[trace_headers @/ && /nal_unit_type/ {
    nal_type = $NF + 0
    if (nal_type <= 31) {
        count++
        expected = count <= 6 ? 19 : 1
        if (nal_type != expected)
            bad = 1
    }
}
END { exit !(count == 12 && !bad) }' "$trace"; then
    printf 'expected six IDR VCL segments followed by six P VCL segments: %s\n' \
        "$trace" >&2
    exit 1
fi
"$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$stream" \
    -map 0:v:0 -vf format=nv12 -f framemd5 "$software_hashes"
env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
    -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
    -f hevc -i "$stream" -map 0:v:0 \
    -vf 'hwdownload,format=nv12' -f framemd5 "$hardware_hashes"
if ! cmp -s "$software_hashes" "$hardware_hashes"; then
    diff -u "$software_hashes" "$hardware_hashes" || true
    printf 'WPP hardware decode differs from software; artifacts: %s\n' \
        "$output_dir" >&2
    exit 1
fi
printf 'PASS: HEVC WPP I/P row segments and hardware/software decode match; artifacts: %s\n' \
    "$output_dir"
