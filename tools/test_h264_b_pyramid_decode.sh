#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
timeout_bin=${TIMEOUT:-timeout}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}

for bin in "$ffmpeg_bin" "$ffprobe_bin" "$timeout_bin"; do
    command -v "$bin" >/dev/null 2>&1 || {
        printf 'missing required command: %s\n' "$bin" >&2
        exit 2
    }
done
"$ffmpeg_bin" -hide_banner -h encoder=libx264 >/dev/null 2>&1 || {
    printf 'ffmpeg must include the libx264 encoder\n' >&2
    exit 2
}
grep -qx trace_headers < <("$ffmpeg_bin" -hide_banner -bsfs 2>/dev/null) || {
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
    if [[ -e "$output_dir" ]]; then
        printf 'output path already exists: %s\n' "$output_dir" >&2
        exit 2
    fi
    mkdir -p -- "$output_dir"
else
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-h264-b-pyramid.XXXXXX")
fi

clip="$output_dir/h264-high-b-pyramid.mp4"
trace="$output_dir/trace_headers.log"
software_hashes="$output_dir/software.framemd5"
hardware_hashes="$output_dir/hardware.framemd5"

"$ffmpeg_bin" -hide_banner -loglevel error -y \
    -f lavfi -i 'testsrc2=size=640x360:rate=60' -frames:v 64 -an \
    -c:v libx264 -profile:v high -level:v 4.1 -preset veryfast \
    -pix_fmt yuv420p -bf 3 -g 32 \
    -x264-params 'b-adapt=0:scenecut=0' "$clip"

frame_stats=$("$ffprobe_bin" -v error -select_streams v:0 \
    -show_entries frame=pict_type -of csv=p=0 "$clip" |
    awk 'NF { frames++; if ($1 == "B") { run++; if (run > max) max = run } else run = 0 }
         END { printf "%d %d\n", frames, max }')
read -r frame_count max_b_run <<< "$frame_stats"
if [[ "$frame_count" -ne 64 || "$max_b_run" -lt 2 ]]; then
    printf 'generated input did not meet the 64-frame / B-frame test contract: frames=%s max_b_run=%s\n' \
        "$frame_count" "$max_b_run" >&2
    exit 1
fi

"$ffmpeg_bin" -hide_banner -i "$clip" -c:v copy \
    -bsf:v trace_headers -f null - >"$trace" 2>&1
grep -Eq 'num_ref_idx_l0_default_active_minus1[[:space:]]+1[[:space:]]*=[[:space:]]*0([[:space:]]|$)' "$trace" || {
    printf 'test stream does not declare the expected zero L0 PPS default\n' >&2
    exit 1
}
grep -Eq 'num_ref_idx_l1_default_active_minus1[[:space:]]+1[[:space:]]*=[[:space:]]*0([[:space:]]|$)' "$trace" || {
    printf 'test stream does not declare the expected zero L1 PPS default\n' >&2
    exit 1
}
awk '
    /Slice Header/ { in_slice = 1; reference = 0; b_slice = 0 }
    in_slice && /nal_ref_idc/ && ($NF + 0) > 0 { reference = 1 }
    in_slice && /slice_type/ && (($NF + 0) % 5) == 1 { b_slice = 1 }
    in_slice && /slice_qp_delta/ {
        if (reference && b_slice) found = 1
        in_slice = 0
    }
    END { exit !found }
' "$trace" || {
    printf 'test stream does not contain a reference B slice\n' >&2
    exit 1
}

"$ffmpeg_bin" -hide_banner -loglevel error -i "$clip" -map 0:v:0 \
    -vf format=nv12 -f framemd5 "$software_hashes"
driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi
env "${driver_env[@]}" "$timeout_bin" --foreground --signal=TERM --kill-after=10s \
    120s "$ffmpeg_bin" -hide_banner -loglevel error \
    -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
    -i "$clip" -map 0:v:0 -vf 'hwdownload,format=nv12' \
    -f framemd5 "$hardware_hashes"

if ! cmp -s "$software_hashes" "$hardware_hashes"; then
    diff -u "$software_hashes" "$hardware_hashes" || true
    printf 'B-pyramid hardware output differs from software; artifacts: %s\n' \
        "$output_dir" >&2
    exit 1
fi

printf 'PASS: 64 H.264 High B-pyramid frames (including reference B slices) are bit-exact; artifacts: %s\n' \
    "$output_dir"
