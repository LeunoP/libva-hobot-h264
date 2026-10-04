# libva-hobot-h264 — VA-API Driver for D-Robotics RDK-X5

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-D--Robotics%20RDK--X5-green.svg)]()
[![Architecture](https://img.shields.io/badge/Arch-ARM64%20(aarch64)-orange.svg)]()
[![Codec](https://img.shields.io/badge/Codec-H.264%20%7C%20JPEG-red.svg)]()
[![Branch: dev](https://img.shields.io/badge/Branch-dev-brightgreen.svg)]()

[English](README.md) | [한국어](README.ko.md)

`libva-hobot-h264` is an open-source, high-performance VA-API (Video Acceleration API) backend driver for the **D-Robotics RDK-X5** single-board computer (powered by Chips&Media Wave521 VPU and ARM Cortex-A55).

It provides a VA-API backend for the D-Robotics VPU. Actual resolution and frame rate depend on the stream, firmware, renderer, and board configuration; this document does not promise a specific performance level.

> [!NOTE]
> **Notice**: This codebase and driver architecture were researched, developed, and optimized by AI (Google DeepMind Antigravity / Gemini and OpenAI Codex) in collaboration with LeunoP.
> *(해당 코드는 AI가 작성 및 최적화했습니다.)*

---

## Key Features

- **VA-API subset**: See the supported scope and limitations below. This does not imply complete VA-API coverage or compatibility with every libva client.
- **H.264 B-frame handling**: Uses the VPU reorder setting and a 128-entry FIFO (`submitted_surfaces`) to associate submitted surfaces with decoded output. A 64-frame H.264 High sample with six consecutive B-frames was hardware-decoded bit-exactly against software NV12 output; run `tools/test_h264_bframe_decode.sh` to repeat this regression.
- **H.264 encoder sequence setup**: Defers VPU startup until the first encoded picture so the requested H.264 level and sequence VUI timing/SAR are applied before initialization. VA-API does not expose input full-range metadata here, so NV12 output is explicitly signaled as limited-range.
- **H.264 visible-size surfaces**: Accepts exact visible-size NV12 surfaces when H.264 coded dimensions are macroblock padded. The input is placed at the SPS crop offset in the coded frame and edge pixels are replicated; full coded-size surfaces retain their original coordinates.
- **H.264 Parameter-Set Refresh**: Detects changed synthesized SPS/PPS data and re-injects the parameter sets before the next picture, keeping decoder state aligned across stream renegotiation.
- **VPU Output Integrity & Corrupted Frame Drop**: Inspects `err_mb_in_frame_display` directly at the driver boundary. Automatically drops severely corrupted frames (`err_mb > 0`) back to the VPU buffer pool, eliminating visual screen tearing on non-compliant or high-level video streams.
- **VPU Hollow-Buffer Guard**: Rejects and recycles empty output buffers emitted during decoder startup or resolution renegotiation before they can reach image mapping or DRM export paths.
- **Hobot-VA WatchDog Real-Time IPC**: Publishes anomaly telemetry to a PID-scoped `/dev/shm/hobot_va_watchdog.<pid>` file. Unique temporary files and a monotonic event sequence keep concurrent decoder threads and independent player processes from overwriting or deleting one another's events.
- **Multi-Slice Frame Assembly**: Aggregates multi-slice pictures (e.g. from broadcast encoders or streaming servers) into single VPU frames per the `MC_FEEDING_MODE_FRAME_SIZE` specification.
- **DMA-BUF & DRM PRIME 2 Export**: Exports decoded VPU DMA-BUFs with `dup(fd)` lifecycle safety. A Hobot graphics buffer is allocated lazily only for the vendor surface-info fallback path when no decoded output is available.
- **NV12 Plane Layout Validation**: Validates Y and UV byte pitches and bounds independently. The codec SDK's `vstride` is the chroma byte pitch; the public surface-info `vstride` reports aligned luma rows only when that layout is verified.
- **Two-Stage MPV Playback Automation (`misc/mpv_scripts/`)**: Includes production-ready companion scripts for automated 5-second probing and seamless in-place software fallback.

---

## Codec Support Matrix

| Codec | Profile | Decode (VLD) | Encode (EncSlice / EncPicture) |
|---|---|:---:|:---:|
| **H.264 (AVC)** | Constrained Baseline | ✅ | ✅ Limited |
| **H.264 (AVC)** | Main | ✅ | ✅ |
| **H.264 (AVC)** | High (up to Level 5.1) | ✅ | ✅ |
| **H.264 (AVC)** | High (Level 5.2 / Multi-Ref DPB Overrun) | 🛡️ Auto-Dropped → SW Fallback | ❌ |
| **JPEG** | Baseline 8-bit YUV 4:2:0 | ✅ Limited | ✅ |
| **HEVC (H.265)** | Main, 8-bit YUV 4:2:0 (restricted) | ✅ Limited | ✅ Limited |
| **HEVC (H.265)** | Main 10 / other formats | ❌ Unsupported | ❌ Unsupported |

> **Note on Level 5.2 / High DPB Streams**: When an H.264 bitstream exceeds the hardware VPU's DPB (Decoded Picture Buffer) or Level specifications (e.g. certain high-profile YouTube 1080p60 streams with 16 reference frames), the VPU produces corrupt macroblocks (`err_mb > 0`). The driver safely drops these frames and signals the companion script to fall back to CPU software decoding.

> **Note on HEVC**: The earlier corrupted output was traced to synthesized parameter sets using defaults that did not match the VA picture parameters. The driver now synthesizes VPS/SPS/PPS from the submitted values and exposes only the tested Main 8-bit 4:2:0 subset. Encoding is limited to CBR, I/P frames, and one full-frame slice. Main 10 and unsupported bitstream features remain unavailable.

## VA-API Scope & Limitations

For HEVC SPS data declaring zero or one short-term RPS set, the VLD path validates both inline and SPS-selected reference sets. Inline syntax is parsed for negative/positive picture counts and per-entry delta/usage flags, checked against VA-API `st_rps_bits`, and validated against active-reference counts and optional list modifications. When the sole SPS RPS is selected, current references are reconstructed from the VA DPB flags and POCs; SAO flags, default P/B list order, and VA `RefPicList` indices are checked. Long-term references remain unsupported, and other slice tools are limited to the explicitly validated patterns below.

A checked-in two-picture, SAO-enabled single-SPS-RPS P fixture (640×360) decoded bit-exactly against software in five fresh candidate-driver VPU sessions (`test_hevc_single_rps_sao_decode.sh`).

- Surface and image paths support 8-bit NV12 / YUV420 with even width and height. Generic surface dimensions are limited to 64–4096 pixels; HEVC Main contexts are capped at 3840×2160.
- H.264 VLD and EncSlice advertise Constrained Baseline, Main, and High. Constrained Baseline encoding selects the SDK Baseline profile, forces CAVLC and disables 8x8 transform, sets `constraint_set1_flag` on verified Baseline SPS NAL units, and removes only the verified zero-valued high-profile PPS extension while preserving chroma-QP semantics. It is restricted to the tested 8-bit 4:2:0, CBR, I/P, single-full-frame-slice subset; syntax and hardware decode are checked by `test_h264_encode_profiles.sh`.
- The H.264 decoder synthesizes profile-matched SPS data for Constrained Baseline, Main, and High; it supports 8-bit 4:2:0 and POC types 0 and 2. Coded macroblock dimensions are limited to the VA context dimensions rounded up to macroblock boundaries; reference-frame counts above the VA DPB capacity of 16 are rejected. POC type 1 is rejected because VA picture parameters do not expose its offset-cycle fields. FMO and redundant-picture-count PPS modes are rejected because the synthesized PPS does not represent them. Constrained Baseline additionally rejects interlaced, CABAC, weighted-prediction, and 8x8-transform picture parameters. High-profile monochrome and 4:2:2/4:4:4 formats are unsupported.
> HEVC list-modification scope: the zero-SPS-RPS inline path parses and validates signaled reordering against VA `RefPicList`. Checked-in 640×360 Main fixtures cover P L0 reordering `[1,0]` and B L0/L1 reordering `[1,2,0]` with three current references; both decoded all 8 pictures bit-exactly against software in 5/5 fresh candidate-driver VPU sessions (`test_hevc_inline_refmod_decode.sh`). The separate two-SPS-RPS inter-picture path still rejects active list modifications; other inline RPS/list layouts are not implied by these fixtures.
- HEVC VLD advertises Main Level 5.1 and is limited to 8-bit 4:2:0, context-matching dimensions up to 3840×2160, and ordered long-format slice sequences whose CTU addresses start at zero and increase within the picture. Main 10, tiles, WPP, scaling lists, PCM, separate colour planes, long-term references, dependent slices, partial slice-data fragments, and entry-point offsets are rejected. Slice-local RPS bit counts are accepted. With two SPS RPS sets, each slice is checked against a narrow supported pattern: IDR pictures require an empty DPB; P pictures select RPS index 0 with exactly the immediately previous POC as their sole reference; B pictures select RPS index 0 and use that same immediately previous POC as the sole reference in both L0 and L1. Within this two-set inter-picture subset, B-frame support is limited to a single reference with no reordering. Luma and chroma weighted P/B tables are parsed and range/boundary checked for the supported 8-bit 4:2:0 format; active reference-list modifications, temporal MVP, SAO, and other unvalidated slice-header tools are rejected. Unit tests cover weighted P/B syntax, malformed values, chroma limits, and header boundaries. Candidate-driver tests exercise the two-SPS-RPS weighted path using 640×360 HM fixtures with two slices per picture. Neutral tables, non-default luma tables (denominator 0, delta weight 0, offset +8), and chroma tables (denominator 0, delta weight 0, Cb/Cr offset +8) decode bit-exactly against software: three P-pattern frames and four B-pattern frames per case, all passing in five repeated runs. Other weight values and RPS/weight-table layouts remain unverified. The PPS list-modification capability flag is accepted for this subset because the one-reference RPS makes list-modification syntax inapplicable. Both single- and multi-slice pictures are accepted only when every slice independently matches the supported pattern. Candidate-driver 640×360 HM streams with two slices per picture decoded bit-exactly against software in 5/5 fresh VPU sessions for IDR+P+P and IDR+BBB patterns. Nonzero SPS RPS counts other than the validated two-set layout are accepted only for independent IDR pictures, whose unused RPS sets are omitted from the driver's synthesized SPS; a 640×360 IDR picture with two slices and 14 SPS RPS sets also decoded bit-exactly in 5/5 fresh VPU sessions. Separately, single-slice HEVC Main inline-RPS streams with non-default weighting decoded bit-exactly on the candidate driver: a no-B P stream (`weighted_pred_flag=1`, non-default L0 luma offset) at 24/24 frames, and a B3 stream (`weighted_bipred_flag=1`, non-default L0 offset in P slices and L1 offset in B slices) at 32/32 frames. Those tests do not cover other RPS/weight-table layouts. Arbitrary multi-slice inter streams remain unvalidated. Other nonzero SPS RPS layouts fail closed for inter pictures. The established single-slice hardware suite remains bit-exact for 640×360 P (30 frames, Level 4.1), 1280×720p60 B3/B6 (64 frames each, Level 4.1), 1920×1080p60 B6 (64 frames, Level 5.0), and 3840×2160p60 B3 (16 frames, Level 5.1). The 60 fps values describe stream timestamps, not a sustained decode-throughput measurement; arbitrary HEVC tools or streams are not implied.
- HEVC EncSlice is limited to Main 8-bit 4:2:0, CBR, I/P frames, and one full-frame slice per picture. SAO is explicitly disabled because the installed decoder SDK does not support HEVC SAO; initialization fails if the encoder cannot apply this setting. The SDK's HEVC crop setting did not change the emitted SPS, so the driver adds standard SPS conformance-window syntax for right/bottom crops while preserving coded dimensions; each crop must be smaller than 16 pixels, align to the 4:2:0 crop unit, retain the same CTU grid, and fit the coded-buffer capacity. 64-frame Level 4.1 streams were hardware re-decoded bit-exactly to software at 640×360 (right/bottom offsets 0/4), 638×360 (1/4), 638×362 (1/3), and 626×354 (7/7) in the SPS. These cases cover crops up to 14 pixels per dimension, not every geometry combination.
- H.264 VLD slice parameters honor `slice_data_offset`, `slice_data_size`, and the `ALL/BEGIN/MIDDLE/END` flags. The driver accepts one parameter buffer with multiple elements paired to one data buffer, or equal-count ordered parameter/data buffer pairs with one element each; ambiguous pairings and out-of-range data are rejected. A 64-frame 640×360 High stream with two slices per picture and six consecutive B-frames decoded bit-exactly against software in 5/5 fresh VPU sessions.
- H.264 encoding is limited to CBR and one full-frame I/P slice per picture (maximum slice count: 1). The SDK's fixed GOP uses a single-reference I/P pattern, so B/SP/SI slices, multiple/partial slices, macroblock maps, and unsupported encoder buffer or misc parameter types are rejected. VA HRD/VBV buffer size is converted to the SDK's 10–3000 ms VBV window; the installed SDK has no control for VA's initial CPB fullness, so that value is not applied. H.264 level and supported sequence VUI fields are fixed before VPU startup; changing them requires a new VA context. JPEG VLD supports baseline sequential, 8-bit, three-component YUV 4:2:0 with one full-frame scan and no rotation; other sampling modes, progressive/multi-scan input, and unsupported VA fields are rejected. JPEG encoding supports baseline sequential 8-bit, three-component pictures, per-picture quality (1–100), supplied quantization tables, and restart intervals. The JPU path was validated against software NV12 output and repeated hardware decode. Attributes reported as `VA_ATTRIB_NOT_SUPPORTED` are not implemented.
- DMA-BUF export supports DRM PRIME 2. Preallocated NV12 surfaces accept only bounds-validated single- or separate-FD layouts; decoded VPU frames with unverified multi-FD layouts are rejected. Legacy DRM PRIME and external-memory import are unsupported.
- Subpictures, display-attribute operations, image palettes, and `vaPutSurface` are not implemented and return errors rather than false success. `vaPutImage` uses a CPU NV12 staging path; it does not provide a hardware blit.
- Unit tests and a successful build do not establish playback or encoding performance on hardware. Run stream regression tests on the target board with `/dev/dri` and the Wave521 VPU available.

---

## Hardware & System Requirements

- **Target Board**: D-Robotics RDK-X5 (8-core Cortex-A55; memory varies by SKU)
- **VPU**: Chips&Media Wave521
- **Operating System**: Ubuntu 22.04 LTS (Jammy) / Debian Linux 6.1 (aarch64)
- **Driver Name**: `hobot` (`LIBVA_DRIVER_NAME=hobot`)
- **Installation Path**: `/usr/lib/aarch64-linux-gnu/dri/hobot_drv_video.so`
- **Dependencies**: `libva-dev`, `libdrm-dev`, D-Robotics multimedia libraries (`/usr/hobot/lib/libmultimedia.so`)

---

## Building and Installation

### 1. Install Prerequisites
```bash
sudo apt-get update
sudo apt-get install -y build-essential libva-dev libdrm-dev vainfo
```

### 2. Build the Driver
```bash
cd libva-hobot
make clean
make -j$(nproc)
```

### 3. Install
```bash
sudo cp -f hobot_drv_video.so /usr/lib/aarch64-linux-gnu/dri/hobot_drv_video.so
```

### 4. Configure Environment
Set `LIBVA_DRIVER_NAME=hobot` so `libva` loads this driver by default:
```bash
echo "export LIBVA_DRIVER_NAME=hobot" >> ~/.bashrc
sudo sh -c 'echo "LIBVA_DRIVER_NAME=hobot" >> /etc/environment'
source ~/.bashrc
```

### 5. Verify Installation
Run `vainfo` to ensure the driver initializes correctly:
```bash
vainfo
```

---

## Zero-Copy Hardware Pipeline: DRM/GBM + Vivante DirectVIV

On the D-Robotics RDK-X5, the most performant, zero-copy video playback architecture bypasses X11 entirely:

```text
H.264 Bitstream
       │
       ▼
 mpv (demuxer)
       │
       ▼
    VA-API
       │
       ▼
libva-hobot-h264
       │
       ▼
 Chips&Media Wave521 VPU
       │
       ▼ (Hardware NV12 Frame in Contiguous Physical Memory)
 Hobot Graphics Buffer
       │
       ▼ (Physical / Virtual Address via vaExportSurfaceHandle)
 Vivante DirectVIV (`glTexDirectVIVMap`)
       │
       ▼ (Zero-Copy Texture Sampling in Silicon)
 Vivante GPU (GLES)
       │
       ▼
    DRM / GBM
       │
       ▼
   HDMI Display (refresh and presented frame rate are system-dependent)
```

### Why Standalone DRM/GBM instead of X11?
- **DRM/GBM (Hardware DirectVIV)**: On the validated path, `glTexDirectVIVMap` samples the VPU's NV12 buffer without a CPU frame copy. This does not guarantee the displayed frame rate: a Moonlight run on the current kiosk path measured about 30 presented FPS from a 60-fps stream. Vsync, compositor policy, and display mode affect presentation independently of VPU decode throughput.
- **X11**: Rendering and zero-copy support depend on the installed vendor graphics stack and session configuration. The `vaapi-copy` path adds CPU memory-copy work; validate it on the target image instead of assuming a fixed CPU cost or universal X11 limitation.

---

## VPU Output Integrity & WatchDog Subsystem

### The Problem: Vendor Error Masking
When playing certain out-of-spec H.264 streams (e.g., Level 5.2 with DPB > 16), the vendor multimedia library (`libmultimedia.so`) reports `error_reason = 0x00000000` (success), masking severe internal VPU failures:
```text
warn_info = 0x01200000 (Level / DPB requirement exceeded)
err_mb = 7800 ~ 8160 / 8160 (95% ~ 100% macroblock corruption)
```
Passing these corrupted surfaces directly to the display results in violent rainbow pixel tearing.

### The Solution: Output-Integrity Drop & Real-Time Telemetry
1. **Corrupted Frame Drop**:
   `libva-hobot` inspects `out_info.video_frame_info.err_mb_in_frame_display`. If `err_mb > 0`, the surface is immediately recycled back to the VPU queue:
   ```c
   if (err_mb > 0 || (err_reason & 0x00020000)) {
       fprintf(stderr, "[HOBOT-VA][DROP] Dropping corrupted frame (err_mb=%d/%d)\n", err_mb, total_mb);
       hb_mm_mc_queue_output_buffer(mctx, &out_buf, 50);
       continue;
   }
   ```
2. **WatchDog IPC Telemetry**:
   The driver records anomalies and publishes real-time telemetry to `/dev/shm/hobot_va_watchdog.<pid>`:
   ```text
   <pid> <anomaly_count> <epoch_timestamp> <err_mb> <total_mb> <event_sequence>
   ```
   A 10-second clean playback window resets each context's anomaly count; the process-wide event sequence stays monotonic so a later anomaly remains observable. Each MPV instance reads only its own PID file, uses the sequence to avoid duplicate handling, and removes that file at shutdown. It does not unlink telemetry during active playback, avoiding a race with a concurrent publisher. Publication uses a unique temporary file and atomic rename.

---

## MPV Companion Scripts (`misc/mpv_scripts/`)

The repository includes a suite of helper scripts in [`misc/mpv_scripts/`](misc/mpv_scripts/) designed to orchestrate seamless hardware verification and software fallback.

### 1. `fallback-restart.lua` (Automated Probing & Fallback)

| Playback Stage | Condition | Player Action | OSD Notification |
| :--- | :--- | :--- | :--- |
| **Initial Probing (First 5s)** | 1 anomaly OR hardware decode error | Rewinds to `00:00:00`, switches to SW decoder (`hwdec=no`), reveals screen & unmutes | **Top-left small notice (1.5s)**:<br>`SW 디코더` |
| **Initial Probing (First 5s)** | Clean playback (0 anomalies) | Rewinds to `00:00:00`, reveals screen & unmutes, keeps HW DirectVIV | *(None — seamless reveal)* |
| **Post-Probing (Mid-Playback)** | 1 anomaly OR decode failure | **Does NOT rewind** (keeps current playback position), switches to SW in-place | **Top-left small notice (1.5s)**:<br>`SW 디코더` |

### 2. `mpv.conf` (Optimized RDK-X5 Profile)
Pre-configured for DRM/GBM DirectVIV zero-copy (`vo=gpu`, `gpu-context=drm`, `hwdec=vaapi`, `vd-lavc-software-fallback=1`), YouTube 1080p AVC preference, and automatic HLS/live stream CPU decoding.

### 3. `mpv-launcher-wrapper.sh` (Production Launcher)
Handles environment variables (`LIBVA_DRIVER_NAME=hobot`, `LD_LIBRARY_PATH=/usr/hobot/lib`) and automatically manages LightDM / DRM Master release and restoration.

### Quick Deployment
```bash
# 1. Install Lua script
mkdir -p ~/.config/mpv/scripts
cp misc/mpv_scripts/fallback-restart.lua ~/.config/mpv/scripts/

# 2. Install MPV configuration
cp misc/mpv_scripts/mpv.conf ~/.config/mpv/mpv.conf

# 3. Install production launcher
sudo cp misc/mpv_scripts/mpv-launcher-wrapper.sh /usr/local/bin/mpv
sudo chmod +x /usr/local/bin/mpv
```

---

## DirectVIV Surface Extension API (`va/va_hobot.h`)

Applications can query low-level hardware memory descriptors (physical addresses, virtual addresses, strides) through standard libva dispatch:

```c
#include <va/va.h>
#include <va/va_hobot.h>

struct hobot_surface_info info = {0};
VAStatus status = vaGetHobotSurfaceInfo(va_dpy, surface_id, &info);
if (status == VA_STATUS_SUCCESS) {
    // info.phys_addr[0]: Y plane physical address
    // info.phys_addr[1]: UV plane physical address
    // info.virt_addr[0]: Y plane virtual address
    // info.virt_addr[1]: UV plane virtual address
    // info.stride, info.vstride, info.width, info.height, info.dma_fd
}
```

This uses the standard `vaExportSurfaceHandle` entry point with vendor memory type `VA_SURFACE_ATTRIB_MEM_TYPE_HOBOT_GRAPH_BUF` (`0x80484F10`), eliminating any need for dynamic symbol lookups or linker hacks.

---

## Verification & Benchmark Suite (`tools/`)

The repository includes comprehensive standalone verification tools in `tools/`:

- **`test_directviv.c`**: Validates Vivante `GL_VIV_direct_texture` and `glTexDirectVIVMap` symbol resolution on the target GPU.
- **`test_render_directviv.c`**: Validates off-screen GLES shader rendering with hardware NV12 texture sampling.
- **`test_nv12_overlay.c`**: Tests hardware DRM KMS plane overlay capabilities.
- **`test_va_directviv_bench.c`**: Measures raw H.264 VPU decode plus DirectVIV GLES pipeline throughput over thousands of frames. The recorded >127 FPS result is an unthrottled benchmark measurement, not end-to-end display presentation or a 1080p60 playback guarantee.
- **`test_va_config.c`**: Exercises VA configuration creation/query/destruction, verifies the driver mutex lifecycle, and checks NV12 CPU mapping through `vaLockSurface`/`vaUnlockSurface`.
- **`test_va_surface_export.c`**: Decodes the entire input with VA-API, synchronizes every VPU frame, and validates both NV12 and separate-layer DRM PRIME 2 exports with plane/object bounds checks.
- **`test_va_jpeg_encode.c`**: Hardware JPEG Baseline encode smoke test; accepts a per-picture quality value and writes one encoded JPEG for independent decode checks.
- **`test_va_jpeg_decode.sh`**: Generates baseline 4:2:0 JPEG input, checks repeated JPEG VLD output in one VA context, and compares NV12 with a full-range software reference using PSNR.
- **`test_hevc_main_decode.sh`**: Generates HEVC Main 8-bit 4:2:0 P/B streams at 640×360, 720p60, 1080p60 Level 5.0, and 4K60 Level 5.1 (including B3/B6 cases), plus no-B weighted-P and B3 weighted-bipred streams with verified non-default luma offsets in P/B slice tables. It verifies the encoded level and frame pattern, then compares every hardware-decoded NV12 frame against software output. Set `HOBOT_HEVC_WEIGHTED_ONLY=1` to run both weighted regressions, or `HOBOT_HEVC_WEIGHTED_P_ONLY=1` for only the P case.
- **`test_hevc_multislice_b_decode.sh`**: Checks a 640×360 HM fixture with two slices per picture and an IDR+BBB low-delay B pattern. Both reference lists use the same immediately previous POC as their sole reference. It compares candidate-driver NV12 output with software across five fresh VPU sessions by default.
- **`test_hevc_two_rps_weighted_decode.sh`**: Adds syntax-valid neutral, non-default luma, and chroma weighted P/B tables to checked-in two-SPS-RPS HM fixtures. It verifies table fields, confirms neutral tables preserve software output and non-default tables change it, then compares candidate-driver output against software (3 P-pattern frames and 4 B-pattern frames per profile) for five cycles by default. Set `HOBOT_HEVC_TWO_RPS_WEIGHTED_CYCLES` to change the repetition count. Other RPS/weight-table layouts remain unverified.
- **`test_hevc_main_encode.sh`**: Encodes a 64-frame HEVC Main no-B CBR stream with VAAPI, checks profile/level/visible and coded dimensions/frame count, verifies SAO is disabled and checks SPS crop offsets, then compares every hardware-decoded frame with software. The tested sizes are 640×360, 638×360, 638×362, and 626×354; other geometry combinations are not exhaustively verified.
- **`test_h264_encode.sh`**: Repeats aligned 720p60, visible 640x360 in a 640x368 coded frame, and 30000/1001 fps VAAPI H.264 encoding. It checks SPS level/timing/cropping and limited-range signaling, then compares every hardware-decoded NV12 frame with software decode. The mocked surface-state test also checks nonzero left/top crop placement and Y/UV edge replication.
- **`test_h264_crop_encode.sh`**: Directly submits a 638x360 VA surface to a 640x368 H.264 context with nonzero SPS left/top offsets, verifies the emitted crop syntax, decodes the visible region, and checks it against the source NV12 frame with a PSNR threshold.
- **`test_va_surface_state.c`**: Mocked regression checks for buffer ownership/teardown, PID-scoped watchdog publication across threads/processes and sequence restart, requested coded-buffer capacity, API array bounds, encoder-control and JPEG quality/quantization/restart/Huffman validation, H.264 HRD-to-VBV conversion and single-slice limits, H.264 context-dimension/DPB bounds and decoder slice-parameter/fragment validation, HEVC Main subset, inline RPS parsing, single-SPS-RPS DPB reconstruction with P/B list ordering and SAO validation, submission-FIFO wraparound, synchronization concurrency, surface-lock lifetime, PRIME export flags, NV12 plane/object bounds, and distinct luma/chroma pitch handling. It does not replace hardware playback tests.

> The Hobot codec SDK's `mc_video_frame_buffer_info_t.vstride` is the chroma byte pitch, not a vertical row count. The driver keeps it separate from the public `hobot_surface_info.vstride`, which is the aligned luma height in lines.

---

## Known Issues & Status

| Issue | Status | Resolution |
|---|:---:|---|
| **H.264 Level 5.2 / DPB Overrun (e.g. YouTube)** | ✅ Solved | VPU Output Integrity drop + WatchDog SW fallback |
| **H.264 60fps B-Frame Pacing / Jitter** | ✅ Solved | VPU Reorder disabled (`reorder_enable=0`) + FIFO surface queue |
| **H.264 multi-reference PPS failure** | ✅ Solved | Synthesize a non-zero PPS L0 reference default when required by Wave521 |
| **Empty VPU output during startup/renegotiation** | ✅ Solved | Validate physical address, payload size, and virtual pointer before retaining or recycling buffers |
| **Invalid NV12 layout and image bounds** | ✅ Fixed | Require even dimensions; validate SDK per-plane sizes, FD/object bounds, and derived-image stride/plane offsets |
| **JPEG JPU decode path** | ✅ Limited | Baseline sequential 8-bit YUV 4:2:0 decode is exposed; other sampling modes remain unavailable |
| **Concurrent buffer-table mutation** | ✅ Fixed | Serialize buffer creation, destruction, and metadata changes with the driver mutex |
| **Global mutex held across VPU dequeue waits** | ✅ Fixed | Release the global mutex during dequeue and use a per-driver condition variable to serialize decoder sync, submission, and teardown |
| **VA config-query deadlock** | ✅ Solved | Unlock the driver mutex before returning config attributes |
| **CPU surface mapping and lock lifetime** | ✅ Fixed | Expose NV12 stride, offsets, DMA-BUF name, and CPU pointer; block surface reuse and teardown until matching `vaUnlockSurface` calls |
| **Audio Underrun Visual Stutter** | ✅ Solved | `audio-buffer=1` configuration in `mpv.conf` |
| **HEVC (H.265) parameter-set mismatch** | ✅ Fixed for tested subset | VPS/SPS/PPS now use submitted VA parameters; restricted Main 8-bit 4:2:0 VLD is exposed, advanced tools remain unsupported |
| **HEVC encode visible-size output** | ⚠️ Limited | Right/bottom SPS crop is hardware-tested at 640×360, 638×360, 638×362, and 626×354 in coded 640×368 frames; other geometry combinations are not exhaustive |
| **X11 Desktop DRI2 Failure** | ⚠️ Bypassed | Use DRM/GBM standalone mode; fallback to `hwdec=vaapi-copy` in X11 |

---

## Diagnostics & Monitoring

Monitor VPU hardware interrupt activity during playback:
```bash
watch -n 1 "cat /proc/interrupts | grep 3b000000.vpu"
```

Monitor WatchDog telemetry:
```bash
cat "/dev/shm/hobot_va_watchdog.$MPV_PID"
```

Monitor board temperatures:
```bash
cat /sys/class/thermal/thermal_zone*/temp | awk '{printf "%.1f°C\n", $1/1000}'
```

---

## License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.
