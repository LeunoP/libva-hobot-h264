#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/card0}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
cycles=${HOBOT_H264_ENCODE_CYCLES:-3}

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
[[ "$cycles" =~ ^[1-9][0-9]*$ ]] || {
    printf 'HOBOT_H264_ENCODE_CYCLES must be a positive integer\n' >&2
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-h264-encode.XXXXXX")
fi

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

for ((cycle = 1; cycle <= cycles; cycle++)); do
    printf -v suffix '%02d' "$cycle"
    for scenario in aligned cropped fractional; do
        if [[ "$scenario" == aligned ]]; then
            width=1280
            height=720
            frames=60
            bitrate=5M
            bufsize=10M
            source_rate=60
            expected_fps=60/1
            expected_num_units=1
            expected_time_scale=120
            crop_flag=0
            crop_bottom=0
        elif [[ "$scenario" == cropped ]]; then
            width=640
            height=360
            frames=30
            bitrate=2M
            bufsize=4M
            source_rate=60
            expected_fps=60/1
            expected_num_units=1
            expected_time_scale=120
            crop_flag=1
            crop_bottom=4
        else
            width=1280
            height=720
            frames=30
            bitrate=5M
            bufsize=10M
            source_rate=30000/1001
            expected_fps=30000/1001
            expected_num_units=1001
            expected_time_scale=60000
            crop_flag=0
            crop_bottom=0
        fi
        clip="$output_dir/h264-${width}x${height}-${scenario}-${suffix}.mp4"
        software_hashes="$output_dir/software-${width}x${height}-${scenario}-${suffix}.framemd5"
        hardware_hashes="$output_dir/hardware-${width}x${height}-${scenario}-${suffix}.framemd5"
        header_log="$output_dir/headers-${width}x${height}-${scenario}-${suffix}.log"

        env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error -y \
            -vaapi_device "$drm_device" -f lavfi \
            -i "testsrc2=size=${width}x${height}:rate=${source_rate}" \
            -vf 'format=nv12,hwupload' -c:v h264_vaapi -profile:v high -level:v 4.1 \
            -b:v "$bitrate" -maxrate "$bitrate" -bufsize "$bufsize" \
            -g 60 -bf 0 -frames:v "$frames" "$clip"

        stats=$("$ffprobe_bin" -v error -count_frames -select_streams v:0 \
            -show_entries stream=profile,level,avg_frame_rate,color_range,nb_read_frames \
            -of csv=p=0 "$clip")
        IFS=, read -r profile level color_range fps actual_frames <<< "$stats"
        if [[ "$profile" != High || "$level" != 41 || "$fps" != "$expected_fps" ||
              "$color_range" != tv || "$actual_frames" != "$frames" ]]; then
            printf 'unexpected stream metadata in %s cycle %s: %s\n' \
                "$scenario" "$suffix" "$stats" >&2
            exit 1
        fi

        "$ffmpeg_bin" -hide_banner -loglevel verbose -i "$clip" -map 0:v:0 \
            -c:v copy -bsf:v trace_headers -frames:v 1 -f null - >"$header_log" 2>&1
        for expected in \
            'level_idc[[:space:]].*= 41$' \
            "num_units_in_tick[[:space:]].*= ${expected_num_units}$" \
            "time_scale[[:space:]].*= ${expected_time_scale}$" \
            'video_full_range_flag[[:space:]].*= 0$' \
            "frame_cropping_flag[[:space:]].*= ${crop_flag}$"; do
            if ! grep -Eq "$expected" "$header_log"; then
                printf 'expected SPS field missing in %s cycle %s: %s\n' \
                    "$scenario" "$suffix" "$expected" >&2
                exit 1
            fi
        done
        if [[ "$crop_bottom" -gt 0 ]] &&
           ! grep -Eq "frame_crop_bottom_offset[[:space:]].*= ${crop_bottom}$" "$header_log"; then
            printf 'unexpected bottom crop in %s cycle %s\n' "$scenario" "$suffix" >&2
            exit 1
        fi

        "$ffmpeg_bin" -hide_banner -loglevel error -i "$clip" -map 0:v:0 \
            -vf format=nv12 -f framemd5 "$software_hashes"
        env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
            -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
            -i "$clip" -map 0:v:0 -vf 'hwdownload,format=nv12' -f framemd5 "$hardware_hashes"

        if ! diff -u "$software_hashes" "$hardware_hashes"; then
            printf 'software/hardware frame hashes differ in %s cycle %s\n' \
                "$scenario" "$suffix" >&2
            exit 1
        fi
        printf 'PASS cycle %s %s: H.264 High L4.1 %s, crop/range VUI, %s matching decode frames\n' \
            "$suffix" "$scenario" "$expected_fps" "$frames"
    done
done

printf 'PASS: %s cycles each of aligned, cropped and fractional-FPS H.264; artifacts: %s\n' \
    "$cycles" "$output_dir"
