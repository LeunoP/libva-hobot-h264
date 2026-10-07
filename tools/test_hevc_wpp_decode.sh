#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
cycles=${HOBOT_HEVC_WPP_CYCLES:-5}

command -v "$ffmpeg_bin" >/dev/null 2>&1 || {
    printf 'missing ffmpeg: %s\n' "$ffmpeg_bin" >&2
    exit 2
}
command -v "$ffprobe_bin" >/dev/null 2>&1 || {
    printf 'missing ffprobe: %s\n' "$ffprobe_bin" >&2
    exit 2
}
"$ffmpeg_bin" -hide_banner -h encoder=libx265 >/dev/null 2>&1 || {
    printf 'ffmpeg must include the libx265 encoder\n' >&2
    exit 2
}
"$ffmpeg_bin" -hide_banner -bsfs 2>/dev/null | grep -qx trace_headers || {
    printf 'ffmpeg must include the trace_headers bitstream filter\n' >&2
    exit 2
}
[[ -e "$drm_device" ]] || {
    printf 'DRM device does not exist: %s\n' "$drm_device" >&2
    exit 2
}
if [[ ! "$cycles" =~ ^[0-9]+$ ]]; then
    printf 'HOBOT_HEVC_WPP_CYCLES must be between 1 and 20\n' >&2
    exit 2
fi
cycles=$((10#$cycles))
if ((cycles < 1 || cycles > 20)); then
    printf 'HOBOT_HEVC_WPP_CYCLES must be between 1 and 20\n' >&2
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-wpp.XXXXXX")
fi

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

clip="$output_dir/hevc-main-wpp.mp4"
"$ffmpeg_bin" -hide_banner -loglevel error -y \
    -f lavfi -i 'testsrc2=size=640x360:rate=30' \
    -frames:v 32 -an -c:v libx265 -profile:v main -preset ultrafast \
    -pix_fmt yuv420p -bf 0 \
    -x265-params 'log-level=error:pools=4:frame-threads=1:wpp=1:bframes=0:ref=1:sao=0:weightp=0:weightb=0:temporal-mvp=0:strong-intra-smoothing=0:keyint=32:min-keyint=32:scenecut=0:open-gop=0' \
    "$clip"

stream_info=$("$ffprobe_bin" -v error -select_streams v:0 -count_frames \
    -show_entries stream=profile,width,height,pix_fmt,nb_read_frames \
    -of csv=p=0 "$clip")
if [[ "$stream_info" != 'Main,640,360,yuv420p,32' ]]; then
    printf 'unexpected HEVC WPP stream properties: %s\n' "$stream_info" >&2
    exit 1
fi

trace_log="$output_dir/trace_headers.log"
"$ffmpeg_bin" -hide_banner -loglevel trace -i "$clip" \
    -c:v copy -bsf:v trace_headers -f null - 2>"$trace_log"
if ! awk '
    /\[trace_headers @/ && /tiles_enabled_flag/ {
        if ($NF != "0") invalid = 1
        tiles++
    }
    /\[trace_headers @/ && /entropy_coding_sync_enabled_flag/ {
        if ($NF != "1") invalid = 1
        wpp++
    }
    /\[trace_headers @/ && /num_entry_point_offsets/ {
        if ($NF + 0 > 0) entries++
    }
    END { exit !(tiles > 0 && wpp > 0 && entries > 0 && !invalid) }
' "$trace_log"; then
    printf 'fixture does not contain WPP entry points with tiles disabled: %s\n' \
        "$trace_log" >&2
    exit 1
fi

software_hashes="$output_dir/software.framemd5"
"$ffmpeg_bin" -hide_banner -loglevel error -i "$clip" -map 0:v:0 \
    -vf format=nv12 -frames:v 32 -f framemd5 "$software_hashes"
frame_count=$(awk '!/^#/ && NF { count++ } END { print count + 0 }' "$software_hashes")
if [[ "$frame_count" -ne 32 ]]; then
    printf 'expected 32 software-decoded pictures, got %s\n' "$frame_count" >&2
    exit 1
fi

for ((cycle = 1; cycle <= cycles; cycle++)); do
    hardware_hashes="$output_dir/hardware-cycle-$cycle.framemd5"
    env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
        -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
        -i "$clip" -map 0:v:0 -frames:v 32 \
        -vf 'hwdownload,format=nv12' -f framemd5 "$hardware_hashes"
    if ! cmp -s "$software_hashes" "$hardware_hashes"; then
        diff -u "$software_hashes" "$hardware_hashes" || true
        printf 'HEVC WPP output differs from software in cycle %s; artifacts: %s\n' \
            "$cycle" "$output_dir" >&2
        exit 1
    fi
done

printf 'PASS: HEVC Main 640x360 WPP, 32 frames, tiles disabled, bit-exact in %s VPU sessions; artifacts: %s\n' \
    "$cycles" "$output_dir"
