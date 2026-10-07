#!/usr/bin/env bash
set -euo pipefail

ffmpeg_bin=${FFMPEG:-ffmpeg}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}

command -v "$ffmpeg_bin" >/dev/null 2>&1 || {
    printf 'missing ffmpeg: %s\n' "$ffmpeg_bin" >&2
    exit 2
}
"$ffmpeg_bin" -hide_banner -h encoder=libx265 >/dev/null 2>&1 || {
    printf 'ffmpeg must include the libx265 encoder\n' >&2
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-scaling.XXXXXX")
fi

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

write_matrix() {
    local path=$1 name=$2 count=$3
    printf '%s\n' "$name" >> "$path"
    for ((i = 0; i < count; i++)); do
        if ((i > 0)); then
            printf ' ' >> "$path"
        fi
        printf '16' >> "$path"
    done
    printf '\n' >> "$path"
}

write_flat_scaling_list() {
    local path=$1 size class component count name
    : > "$path"
    for size in 4 8 16; do
        count=64
        if [[ "$size" == 4 ]]; then
            count=16
        fi
        for class in INTRA INTER; do
            for component in LUMA CHROMAU CHROMAV; do
                name="${class}${size}X${size}_${component}"
                write_matrix "$path" "$name" "$count"
                if [[ "$size" == 16 ]]; then
                    write_matrix "$path" "${name}_DC" 1
                fi
            done
        done
    done
    for class in INTRA INTER; do
        name="${class}32X32_LUMA"
        write_matrix "$path" "$name" 64
        write_matrix "$path" "${name}_DC" 1
    done
}

check_scaling_flag() {
    local clip=$1 expected_data_flag=$2 trace
    trace=$("$ffmpeg_bin" -hide_banner -loglevel trace -i "$clip" \
        -c:v copy -bsf:v trace_headers -f null - 2>&1)
    grep -Eq 'scaling_list_enabled_flag[[:space:]]+1 = 1' <<<"$trace" || {
        printf 'scaling-list fixture did not enable scaling lists: %s\n' "$clip" >&2
        exit 1
    }
    grep -Eq "sps_scaling_list_data_present_flag[[:space:]]+[0-9]+ = ${expected_data_flag}" \
        <<<"$trace" || {
        printf 'unexpected SPS scaling-list data flag in %s\n' "$clip" >&2
        exit 1
    }
}

encode_fixture() {
    local clip=$1 scaling_arg=$2
    "$ffmpeg_bin" -hide_banner -loglevel error \
        -f lavfi -i 'testsrc2=size=640x360:rate=30' \
        -frames:v 8 -an -c:v libx265 -profile:v main -preset ultrafast \
        -pix_fmt yuv420p -bf 0 \
        -x265-params "${scaling_arg}:log-level=error:pools=none:frame-threads=1:wpp=0:bframes=0:ref=1:weightp=0:weightb=0:sao=0:keyint=8:min-keyint=8:scenecut=0:open-gop=0" \
        -f hevc "$clip"
}

default_clip="$output_dir/hevc-scaling-default.hevc"
default_software="$output_dir/default-software.framemd5"
default_hardware="$output_dir/default-hardware.framemd5"
custom_matrix="$output_dir/hevc-scaling-flat16.txt"
custom_clip="$output_dir/hevc-scaling-custom.hevc"
custom_log="$output_dir/custom-rejection.log"

encode_fixture "$default_clip" 'scaling-list=default'
check_scaling_flag "$default_clip" 0
"$ffmpeg_bin" -hide_banner -loglevel error -i "$default_clip" \
    -map 0:v:0 -vf format=nv12 -f framemd5 "$default_software"
env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
    -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
    -i "$default_clip" -map 0:v:0 -vf 'hwdownload,format=nv12' \
    -f framemd5 "$default_hardware"
if ! diff -u "$default_software" "$default_hardware"; then
    printf 'HEVC default scaling-list output differs from software; artifacts: %s\n' \
        "$output_dir" >&2
    exit 1
fi

write_flat_scaling_list "$custom_matrix"
encode_fixture "$custom_clip" "scaling-list=$custom_matrix"
check_scaling_flag "$custom_clip" 1
if env "${driver_env[@]}" LIBVA_DEBUG=1 "$ffmpeg_bin" -hide_banner \
    -loglevel error -vaapi_device "$drm_device" -hwaccel vaapi \
    -hwaccel_output_format vaapi -i "$custom_clip" -map 0:v:0 \
    -frames:v 1 -vf 'hwdownload,format=nv12' -f null - >"$custom_log" 2>&1; then
    printf 'non-default HEVC scaling matrices were unexpectedly accepted; artifacts: %s\n' \
        "$output_dir" >&2
    exit 1
fi
grep -Fq 'non-default HEVC scaling matrices are unsupported' "$custom_log" || {
    printf 'custom scaling-list test failed for an unexpected reason; see %s\n' \
        "$custom_log" >&2
    exit 1
}

printf 'PASS: HEVC default scaling matrices are bit-exact; non-default matrices fail fast; artifacts: %s\n' \
    "$output_dir"
