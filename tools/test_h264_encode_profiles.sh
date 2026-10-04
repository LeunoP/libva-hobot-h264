#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/card0}
driver_name=${LIBVA_DRIVER_NAME:-hobot}

command -v "$ffmpeg_bin" >/dev/null 2>&1 || {
    printf 'missing ffmpeg: %s\n' "$ffmpeg_bin" >&2
    exit 2
}
command -v "$ffprobe_bin" >/dev/null 2>&1 || {
    printf 'missing ffprobe: %s\n' "$ffprobe_bin" >&2
    exit 2
}
"$ffmpeg_bin" -hide_banner -h encoder=h264_vaapi >/dev/null 2>&1 || {
    printf 'ffmpeg must include the h264_vaapi encoder\n' >&2
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
    if [[ -e "$output_dir" ]]; then
        printf 'output path already exists: %s\n' "$output_dir" >&2
        exit 2
    fi
    mkdir -p -- "$output_dir"
else
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-h264-encode-profiles.XXXXXX")
fi

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

for profile in constrained_baseline main high; do
    clip="$output_dir/h264-$profile.mp4"
    software_hashes="$output_dir/$profile-software.framemd5"
    hardware_hashes="$output_dir/$profile-hardware.framemd5"
    case "$profile" in
        constrained_baseline) expected_profile='Constrained Baseline' ;;
        main) expected_profile=Main ;;
        high) expected_profile=High ;;
    esac

    env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error -y \
        -vaapi_device "$drm_device" -f lavfi \
        -i 'testsrc2=size=640x360:rate=30' -vf 'format=nv12,hwupload' \
        -c:v h264_vaapi -profile:v "$profile" -level:v 4.1 \
        -b:v 1M -maxrate 1M -bufsize 2M -g 30 -bf 0 -frames:v 64 "$clip"

    stats=$("$ffprobe_bin" -v error -count_frames -select_streams v:0 \
        -show_entries stream=profile,level,pix_fmt,nb_read_frames \
        -of csv=p=0 "$clip")
    IFS=, read -r actual_profile pixel_format level frame_count <<< "$stats"
    if [[ "$actual_profile" != "$expected_profile" || "$level" != 41 ||
          "$pixel_format" != yuv420p || "$frame_count" != 64 ]]; then
        printf 'unexpected encoded stream metadata for %s: %s\n' "$profile" "$stats" >&2
        exit 1
    fi

    if [[ "$profile" == constrained_baseline ]]; then
        headers="$output_dir/constrained-baseline-trace-headers.log"
        "$ffmpeg_bin" -hide_banner -loglevel trace -i "$clip" -c:v copy \
            -bsf:v trace_headers -f null - 2>"$headers"
        if ! grep -Eq 'constraint_set1_flag[[:space:]]+1 = 1' "$headers" ||
           ! grep -Eq 'frame_mbs_only_flag[[:space:]]+1 = 1' "$headers" ||
           grep -Eq 'entropy_coding_mode_flag[[:space:]]+1 = 1|transform_8x8_mode_flag|pic_scaling_matrix_present_flag|second_chroma_qp_index_offset|weighted_pred_flag[[:space:]]+1 = 1|weighted_bipred_idc[[:space:]]+[1-3] =' \
               "$headers"; then
            printf 'Constrained Baseline syntax constraints failed; see %s\n' \
                "$headers" >&2
            exit 1
        fi
    fi

    "$ffmpeg_bin" -hide_banner -loglevel error -i "$clip" -map 0:v:0 \
        -vf format=nv12 -f framemd5 "$software_hashes"
    env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
        -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
        -i "$clip" -map 0:v:0 -vf 'hwdownload,format=nv12' \
        -f framemd5 "$hardware_hashes"
    if ! diff -u "$software_hashes" "$hardware_hashes"; then
        printf '%s encoded profile hardware decode differs from software; artifacts: %s\n' \
            "$profile" "$output_dir" >&2
        exit 1
    fi
    printf 'PASS: H.264 %s encode metadata and 64 hardware-decoded frame hashes\n' "$profile"
done

printf 'PASS: H.264 Constrained Baseline/Main/High encoders and 64-frame hardware decode comparisons; artifacts: %s\n' \
    "$output_dir"
