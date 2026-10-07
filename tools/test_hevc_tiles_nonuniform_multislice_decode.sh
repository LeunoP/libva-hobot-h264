#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ffmpeg_bin=${FFMPEG:-ffmpeg}
ffprobe_bin=${FFPROBE:-ffprobe}
drm_device=${HOBOT_DRM_DEVICE:-/dev/dri/renderD128}
driver_name=${LIBVA_DRIVER_NAME:-hobot}
cycles=${HOBOT_HEVC_TILES_NONUNIFORM_MULTISLICE_CYCLES:-5}
fixture=${HOBOT_HEVC_TILES_NONUNIFORM_MULTISLICE_FIXTURE:-"$script_dir/testdata/hevc_main_640x360_nonuniform_tiles_2x2_multislice_b.hevc"}

command -v "$ffmpeg_bin" >/dev/null 2>&1 || {
    printf 'missing ffmpeg: %s\n' "$ffmpeg_bin" >&2
    exit 2
}
command -v "$ffprobe_bin" >/dev/null 2>&1 || {
    printf 'missing ffprobe: %s\n' "$ffprobe_bin" >&2
    exit 2
}
"$ffmpeg_bin" -hide_banner -bsfs 2>/dev/null | grep -qx trace_headers || {
    printf 'ffmpeg must include the trace_headers bitstream filter\n' >&2
    exit 2
}
[[ -r "$fixture" ]] || {
    printf 'fixture is not readable: %s\n' "$fixture" >&2
    exit 2
}
[[ -e "$drm_device" ]] || {
    printf 'DRM device does not exist: %s\n' "$drm_device" >&2
    exit 2
}
if [[ ! "$cycles" =~ ^[0-9]+$ ]]; then
    printf 'HOBOT_HEVC_TILES_NONUNIFORM_MULTISLICE_CYCLES must be between 1 and 20\n' >&2
    exit 2
fi
cycles=$((10#$cycles))
if ((cycles < 1 || cycles > 20)); then
    printf 'HOBOT_HEVC_TILES_NONUNIFORM_MULTISLICE_CYCLES must be between 1 and 20\n' >&2
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
    output_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobot-hevc-tiles-nonuniform-multislice.XXXXXX")
fi

driver_env=("LIBVA_DRIVER_NAME=$driver_name")
if [[ -n "${LIBVA_DRIVERS_PATH:-}" ]]; then
    driver_env+=("LIBVA_DRIVERS_PATH=$LIBVA_DRIVERS_PATH")
fi
if [[ -n "${LIBVA_TRACE:-}" ]]; then
    driver_env+=("LIBVA_TRACE=$LIBVA_TRACE")
fi

stream_info=$("$ffprobe_bin" -v error -select_streams v:0 -count_frames \
    -show_entries stream=profile,width,height,pix_fmt,nb_read_frames \
    -of csv=p=0 "$fixture")
if [[ "$stream_info" != 'Main,640,360,yuv420p,8' ]]; then
    printf 'unexpected HEVC tile fixture properties: %s\n' "$stream_info" >&2
    exit 1
fi

trace_log="$output_dir/trace_headers.log"
"$ffmpeg_bin" -hide_banner -loglevel trace -i "$fixture" \
    -c:v copy -bsf:v trace_headers -f null - 2>"$trace_log"
if ! awk '
    /\[trace_headers @/ && /tiles_enabled_flag/ {
        if ($NF != "1") invalid = 1
        tiles++
    }
    /\[trace_headers @/ && /entropy_coding_sync_enabled_flag/ {
        if ($NF != "0") invalid = 1
        wpp++
    }
    /\[trace_headers @/ && /num_tile_columns_minus1/ {
        if ($NF + 0 != 1) invalid = 1
        columns++
    }
    /\[trace_headers @/ && /num_tile_rows_minus1/ {
        if ($NF + 0 != 1) invalid = 1
        rows++
    }
    /\[trace_headers @/ && /uniform_spacing_flag/ {
        if ($NF != "0") invalid = 1
        nonuniform++
    }
    /\[trace_headers @/ && /column_width_minus1\[0\]/ {
        if ($NF + 0 != 5) invalid = 1
        column_width++
    }
    /\[trace_headers @/ && /row_height_minus1\[0\]/ {
        if ($NF + 0 != 3) invalid = 1
        row_height++
    }
    /\[trace_headers @/ && /slice_segment_address/ {
        if ($NF != "6" && $NF != "40" && $NF != "46") invalid = 1
        starts[$NF]++
        addresses++
    }
    /\[trace_headers @/ && /num_entry_point_offsets/ {
        if ($NF != "0") invalid = 1
        entry_points++
    }
    END {
        exit !(tiles >= 1 && wpp >= 1 && columns >= 1 && rows >= 1 &&
               nonuniform >= 1 && column_width >= 1 && row_height >= 1 &&
               starts["6"] == 8 && starts["40"] == 8 && starts["46"] == 8 &&
               addresses == 24 && entry_points == 32 && !invalid)
    }
' "$trace_log"; then
    printf 'fixture does not match the verified 4-slice non-uniform 2x2 tile topology: %s\n' \
        "$trace_log" >&2
    exit 1
fi

software_hashes="$output_dir/software.framemd5"
"$ffmpeg_bin" -hide_banner -loglevel error -i "$fixture" \
    -vf format=nv12 -f framemd5 "$software_hashes"

for ((cycle = 1; cycle <= cycles; cycle++)); do
    hardware_hashes="$output_dir/hardware-cycle-$cycle.framemd5"
    env "${driver_env[@]}" "$ffmpeg_bin" -hide_banner -loglevel error \
        -vaapi_device "$drm_device" -hwaccel vaapi -hwaccel_output_format vaapi \
        -i "$fixture" -vf 'hwdownload,format=nv12' -f framemd5 "$hardware_hashes"
    if ! cmp -s "$software_hashes" "$hardware_hashes"; then
        diff -u "$software_hashes" "$hardware_hashes" || true
        printf 'HEVC non-uniform tiled multislice output differs from software in cycle %s; artifacts: %s\n' \
            "$cycle" "$output_dir" >&2
        exit 1
    fi
done

printf 'PASS: HEVC Main 640x360 explicit non-uniform 2x2 tiles, 4 independent slices/picture, 8 frames, bit-exact in %s VPU sessions; artifacts: %s\n' \
    "$cycles" "$output_dir"
