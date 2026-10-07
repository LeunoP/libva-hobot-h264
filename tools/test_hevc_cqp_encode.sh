#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$script_dir/.." && pwd)
ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
test_build_dir=${TEST_BUILD_DIR:-/tmp/libva-hobot-tests}

for tool in "$ffmpeg_bin" "$ffprobe_bin" make; do
    command -v "$tool" >/dev/null 2>&1 || {
        printf 'missing required command: %s\n' "$tool" >&2
        exit 2
    }
done
"$ffmpeg_bin" -hide_banner -bsfs 2>/dev/null | grep -qx trace_headers || {
    printf 'ffmpeg must include the trace_headers bitstream filter\n' >&2
    exit 2
}
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
    [[ ! -e "$output_dir" ]] || {
        printf 'output path already exists: %s\n' "$output_dir" >&2
        exit 2
    }
    mkdir -p -- "$output_dir"
else
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-cqp.XXXXXX")
fi

make -C "$repo_dir" "TEST_BUILD_DIR=$test_build_dir" \
    "$test_build_dir/test_hevc_wpp_encode"

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

for qp_case in 0:0:0:0 26:26:-26:25 10:20:-10:17 51:51:0:0; do
    IFS=: read -r qp_i qp_p delta_i delta_p <<< "$qp_case"
    expected_i=$((qp_i + delta_i))
    expected_p=$((qp_p + delta_p))
    stream="$output_dir/hevc-cqp-i${qp_i}-p${qp_p}.hevc"
    headers="$output_dir/hevc-cqp-i${qp_i}-p${qp_p}.headers.log"
    software_hashes="$output_dir/hevc-cqp-i${qp_i}-p${qp_p}.software.framemd5"
    hardware_hashes="$output_dir/hevc-cqp-i${qp_i}-p${qp_p}.hardware.framemd5"

    env "${driver_env[@]}" "$test_build_dir/test_hevc_wpp_encode" \
        "$stream" 0 CQP "$qp_i" "$qp_p" "$delta_i" "$delta_p"
    "$ffmpeg_bin" -hide_banner -f hevc -i "$stream" -c:v copy \
        -bsf:v trace_headers -f null - >"$headers" 2>&1

    stream_info=$("$ffprobe_bin" -v error -f hevc -select_streams v:0 \
        -count_frames -show_entries stream=profile,width,height,nb_read_frames \
        -of csv=p=0 "$stream")
    if [[ "$stream_info" != 'Main,640,360,2' ]]; then
        printf 'unexpected HEVC CQP stream metadata: %s\n' "$stream_info" >&2
        exit 1
    fi
    frame_types=$("$ffprobe_bin" -v error -f hevc -select_streams v:0 \
        -show_entries frame=pict_type -of csv=p=0 "$stream")
    if [[ "$frame_types" != $'I\nP' ]]; then
        printf 'expected HEVC CQP I/P frames, got: %s\n' "$frame_types" >&2
        exit 1
    fi
    if ! awk -v expected_i="$expected_i" -v expected_p="$expected_p" '
        /init_qp_minus26[[:space:]]/ { base_qp = $NF + 26 }
        /slice_qp_delta[[:space:]]/ {
            expected = slices == 0 ? expected_i : expected_p
            if (base_qp + $NF != expected) invalid = 1
            slices++
        }
        END { exit (invalid || slices != 2) }
    ' "$headers"; then
        printf 'HEVC CQP bitstream QP did not match effective I=%s P=%s; see %s\n' \
            "$expected_i" "$expected_p" "$headers" >&2
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
        printf 'HEVC CQP hardware decode differs from software; artifacts: %s\n' \
            "$output_dir" >&2
        exit 1
    fi
    printf 'PASS: HEVC CQP I=%s+%s P=%s+%s (effective %s/%s), bitstream and hardware/software decode match\n' \
        "$qp_i" "$delta_i" "$qp_p" "$delta_p" "$expected_i" "$expected_p"
done

wpp_stream="$output_dir/hevc-cqp-wpp-i26-p37.hevc"
wpp_headers="$output_dir/hevc-cqp-wpp.headers.log"
wpp_software_hashes="$output_dir/hevc-cqp-wpp.software.framemd5"
wpp_hardware_hashes="$output_dir/hevc-cqp-wpp.hardware.framemd5"
env "${driver_env[@]}" "$test_build_dir/test_hevc_wpp_encode" \
    "$wpp_stream" 1 CQP 26 37
"$ffmpeg_bin" -hide_banner -f hevc -i "$wpp_stream" -c:v copy \
    -bsf:v trace_headers -f null - >"$wpp_headers" 2>&1
if ! awk '
    /init_qp_minus26[[:space:]]/ { base_qp = $NF + 26 }
    /slice_qp_delta[[:space:]]/ {
        expected = slices == 0 ? 26 : 37
        if (base_qp + $NF != expected) invalid = 1
        slices++
    }
    END { exit (invalid || slices != 2) }
' "$wpp_headers"; then
    printf 'HEVC WPP CQP bitstream did not preserve I/P QP; see %s\n' \
        "$wpp_headers" >&2
    exit 1
fi
if ! awk '/\[trace_headers @/ && /dependent_slice_segment_flag/ {
        count++
        if ($NF != 1) bad = 1
    }
    END { exit !(count == 10 && !bad) }' "$wpp_headers"; then
    printf 'HEVC WPP CQP output did not contain five dependent row segments per frame\n' >&2
    exit 1
fi
if ! awk '/\[trace_headers @/ && /num_entry_point_offsets/ {
        count++
        if ($NF != 0) bad = 1
    }
    END { exit !(count == 12 && !bad) }' "$wpp_headers"; then
    printf 'unexpected HEVC WPP CQP entry-point layout\n' >&2
    exit 1
fi
"$ffmpeg_bin" -hide_banner -loglevel error -f hevc -i "$wpp_stream" \
    -map 0:v:0 -vf format=nv12 -f framemd5 "$wpp_software_hashes"
env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
    -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
    -f hevc -i "$wpp_stream" -map 0:v:0 \
    -vf 'hwdownload,format=nv12' -f framemd5 "$wpp_hardware_hashes"
if ! cmp -s "$wpp_software_hashes" "$wpp_hardware_hashes"; then
    diff -u "$wpp_software_hashes" "$wpp_hardware_hashes" || true
    printf 'HEVC WPP CQP hardware decode differs from software\n' >&2
    exit 1
fi
printf 'PASS: HEVC WPP CQP I=26 P=37 and hardware/software decode match\n'

invalid_log="$output_dir/hevc-cqp-invalid-effective-qp.log"
if env "${driver_env[@]}" "$test_build_dir/test_hevc_wpp_encode" \
    "$output_dir/invalid-effective-qp.hevc" 0 CQP 0 26 -1 0 \
    >"$invalid_log" 2>&1; then
    printf 'HEVC CQP accepted an effective QP below zero\n' >&2
    exit 1
fi
if ! grep -qi 'vaRenderPicture failed: invalid parameter' "$invalid_log"; then
    printf 'HEVC CQP invalid-QP rejection returned an unexpected error; see %s\n' \
        "$invalid_log" >&2
    exit 1
fi
printf 'PASS: HEVC CQP rejects effective QP below zero\n'

invalid_high_log="$output_dir/hevc-cqp-invalid-effective-qp-high.log"
if env "${driver_env[@]}" "$test_build_dir/test_hevc_wpp_encode" \
    "$output_dir/invalid-effective-qp-high.hevc" 0 CQP 26 51 0 1 \
    >"$invalid_high_log" 2>&1; then
    printf 'HEVC CQP accepted an effective QP above 51\n' >&2
    exit 1
fi
if ! grep -qi 'vaRenderPicture failed: invalid parameter' "$invalid_high_log"; then
    printf 'HEVC CQP upper-bound rejection returned an unexpected error; see %s\n' \
        "$invalid_high_log" >&2
    exit 1
fi
printf 'PASS: HEVC CQP rejects effective QP above 51\n'

printf 'PASS: HEVC CQP direct-VA range and per-picture QP tests; artifacts: %s\n' \
    "$output_dir"
