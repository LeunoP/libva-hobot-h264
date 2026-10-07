#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
width=${HOBOT_HEVC_ENCODE_WIDTH:-640}
height=${HOBOT_HEVC_ENCODE_HEIGHT:-360}
rate=${HOBOT_HEVC_ENCODE_RATE:-30}
frames=${HOBOT_HEVC_ENCODE_FRAMES:-64}
level=${HOBOT_HEVC_ENCODE_LEVEL:-4.1}
bitrate=${HOBOT_HEVC_ENCODE_BITRATE:-4M}
bufsize=${HOBOT_HEVC_ENCODE_BUFSIZE:-8M}
rate_control=${HOBOT_HEVC_ENCODE_RC_MODE:-CBR}
qp=${HOBOT_HEVC_ENCODE_QP:-26}

command -v "$ffmpeg_bin" >/dev/null 2>&1 || {
    printf 'missing ffmpeg: %s\n' "$ffmpeg_bin" >&2
    exit 2
}
command -v "$ffprobe_bin" >/dev/null 2>&1 || {
    printf 'missing ffprobe: %s\n' "$ffprobe_bin" >&2
    exit 2
}
"$ffmpeg_bin" -hide_banner -h encoder=hevc_vaapi >/dev/null 2>&1 || {
    printf 'ffmpeg must include the hevc_vaapi encoder\n' >&2
    exit 2
}
[[ -e "$drm_device" ]] || {
    printf 'DRM device does not exist: %s\n' "$drm_device" >&2
    exit 2
}
[[ "$width" =~ ^[0-9]+$ && "$height" =~ ^[0-9]+$ &&
   "$rate" =~ ^[0-9]+$ && "$frames" =~ ^[1-9][0-9]*$ ]] || {
    printf 'width, height, rate and frame count must be positive integers\n' >&2
    exit 2
}
if [[ "$rate_control" != CBR && "$rate_control" != VBR && "$rate_control" != CQP ]]; then
    printf 'HOBOT_HEVC_ENCODE_RC_MODE must be CBR, VBR or CQP\n' >&2
    exit 2
fi
if [[ ! "$qp" =~ ^([1-9]|[1-4][0-9]|5[01])$ ]]; then
    printf 'HOBOT_HEVC_ENCODE_QP must be in [1, 51]; use the direct VA test for QP 0\n' >&2
    exit 2
fi
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-encode.XXXXXX")
fi

case "$level" in
    1) expected_level_idc=30 ;;
    2) expected_level_idc=60 ;;
    2.1) expected_level_idc=63 ;;
    3) expected_level_idc=90 ;;
    3.1) expected_level_idc=93 ;;
    4) expected_level_idc=120 ;;
    4.1) expected_level_idc=123 ;;
    5) expected_level_idc=150 ;;
    5.1) expected_level_idc=153 ;;
    *) printf 'unsupported test level: %s\n' "$level" >&2; exit 2 ;;
esac

clip="$output_dir/hevc-main-${width}x${height}.mp4"
software_hashes="$output_dir/software.framemd5"
hardware_hashes="$output_dir/hardware.framemd5"
header_log="$output_dir/headers.log"
driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

if [[ "$rate_control" == CBR ]]; then
    rate_control_args=(-rc_mode CBR -b:v "$bitrate" -maxrate "$bitrate" -bufsize "$bufsize")
elif [[ "$rate_control" == VBR ]]; then
    rate_control_args=(-rc_mode VBR -b:v "$bitrate" -maxrate "${HOBOT_HEVC_ENCODE_MAX_BITRATE:-8M}" -bufsize "$bufsize")
else
    rate_control_args=(-rc_mode CQP -qp "$qp")
fi

env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error -y \
    -vaapi_device "$drm_device" -f lavfi \
    -i "testsrc2=size=${width}x${height}:rate=${rate}" \
    -vf 'format=nv12,hwupload' -c:v hevc_vaapi -profile:v main -level:v "$level" \
    "${rate_control_args[@]}" -g 30 -bf 0 \
    -frames:v "$frames" "$clip"

probe_stream_field() {
    local count_option=()
    [[ "$1" == nb_read_frames ]] && count_option=(-count_frames)
    "$ffprobe_bin" -v error "${count_option[@]}" -select_streams v:0 \
        -show_entries "stream=$1" -of default=noprint_wrappers=1:nokey=1 "$clip"
}
profile=$(probe_stream_field profile)
actual_level=$(probe_stream_field level)
pix_fmt=$(probe_stream_field pix_fmt)
actual_width=$(probe_stream_field width)
actual_height=$(probe_stream_field height)
actual_coded_width=$(probe_stream_field coded_width)
actual_coded_height=$(probe_stream_field coded_height)
actual_frames=$(probe_stream_field nb_read_frames)
stream_info="$profile,$actual_level,$pix_fmt,$actual_width,$actual_height,coded=${actual_coded_width}x${actual_coded_height},$actual_frames"
if [[ "$profile" != Main || "$actual_level" != "$expected_level_idc" ||
      "$pix_fmt" != yuv420p || "$actual_width" != "$width" ||
      "$actual_height" != "$height" || "$actual_frames" != "$frames" ||
      ! "$actual_coded_width" =~ ^[0-9]+$ ||
      ! "$actual_coded_height" =~ ^[0-9]+$ ]]; then
    printf 'unexpected HEVC stream metadata: %s\n' "$stream_info" >&2
    exit 1
fi
if (( actual_coded_width < width || actual_coded_height < height )); then
    printf 'coded HEVC dimensions are smaller than the requested frame: %s\n' \
        "$stream_info" >&2
    exit 1
fi
if [[ "$width" == 638 ]] && (( actual_coded_width != 640 )); then
    printf '638-pixel crop test expected a 640-pixel coded width: %s\n' \
        "$stream_info" >&2
    exit 1
fi
if (( height % 16 != 0 )) &&
   (( actual_coded_height <= actual_height ||
      actual_coded_height - actual_height >= 16 )); then
    printf 'non-aligned HEVC height lacks a bounded coded/display crop: %s\n' \
        "$stream_info" >&2
    exit 1
fi
"$ffmpeg_bin" -hide_banner -loglevel verbose -i "$clip" -map 0:v:0 \
    -c:v copy -bsf:v trace_headers -frames:v 1 -f null - \
    >"$header_log" 2>&1
if ! awk '/sample_adaptive_offset_enabled_flag[[:space:]]/ {
    found = 1
    if ($NF != "0") invalid = 1
} END { exit (!found || invalid) }' "$header_log"; then
    printf 'HEVC encoder output did not disable unsupported decoder SAO; see %s\n' \
        "$header_log" >&2
    exit 1
fi
if [[ "$rate_control" == CQP ]] &&
   ! awk -v expected="$qp" '
        /init_qp_minus26[[:space:]]/ { base_qp = $NF + 26 }
        /slice_qp_delta[[:space:]]/ {
            if (base_qp + $NF != expected) invalid = 1
            slices++
        }
        END { exit (invalid || slices == 0) }
    ' "$header_log"; then
    printf 'HEVC CQP output did not preserve requested QP %s; see %s\n' \
        "$qp" "$header_log" >&2
    exit 1
fi
if (( actual_coded_width != actual_width || actual_coded_height != actual_height )); then
    expected_right_offset=$(((actual_coded_width - actual_width) / 2))
    expected_bottom_offset=$(((actual_coded_height - actual_height) / 2))
    for expected in \
        'conformance_window_flag[[:space:]].*= 1$' \
        'conf_win_left_offset[[:space:]].*= 0$' \
        "conf_win_right_offset[[:space:]].*= ${expected_right_offset}$" \
        'conf_win_top_offset[[:space:]].*= 0$' \
        "conf_win_bottom_offset[[:space:]].*= ${expected_bottom_offset}$"; do
        if ! grep -Eq "$expected" "$header_log"; then
            printf 'expected HEVC SPS crop field missing: %s; see %s\n' \
                "$expected" "$header_log" >&2
            exit 1
        fi
    done
fi

b_frame_count=$("$ffprobe_bin" -v error -select_streams v:0 \
    -show_entries frame=pict_type -of csv=p=0 "$clip" |
    awk '$1 == "B" { count++ } END { print count + 0 }')
if [[ "$b_frame_count" != 0 ]]; then
    printf 'HEVC no-B stream unexpectedly contains %s B-frames\n' "$b_frame_count" >&2
    exit 1
fi

"$ffmpeg_bin" -hide_banner -loglevel error -i "$clip" -map 0:v:0 \
    -vf format=nv12 -f framemd5 "$software_hashes"
decoded_frames=$(awk 'NF && $1 !~ /^#/ { count++ } END { print count + 0 }' "$software_hashes")
if [[ "$decoded_frames" != "$frames" ]]; then
    printf 'software decoder produced %s frames, expected %s; artifacts: %s\n' \
        "$decoded_frames" "$frames" "$output_dir" >&2
    exit 1
fi
env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
    -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
    -i "$clip" -map 0:v:0 -vf 'hwdownload,format=nv12' \
    -f framemd5 "$hardware_hashes"
if ! diff -u "$software_hashes" "$hardware_hashes"; then
    printf 'hardware HEVC re-decode differs from software; artifacts: %s\n' \
        "$output_dir" >&2
    exit 1
fi

printf 'PASS: HEVC Main L%s %sx%s %sfps, %s %s frames QP=%s; hardware re-decode bit-exact to software; artifacts: %s\n' \
    "$level" "$width" "$height" "$rate" "$frames" "$rate_control" \
    "${qp:--}" "$output_dir"
