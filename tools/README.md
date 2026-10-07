# Verification and Benchmark Tools

This directory contains standalone test and benchmark tools for validating hardware zero-copy video playback pipelines on the D-Robotics RDK-X5 platform.

Build the VA-API C test programs from the current sources with `make build-va-tests`.
The default output directory is `/tmp/libva-hobot-tests`; set `TEST_BUILD_DIR` to change it.
Run the hardware-independent VA surface, ownership, and teardown regression suite with `make test-va-state`.
Headless VA decode, encode, config, and surface tests default to `/dev/dri/renderD128` to avoid taking the display DRM master. Set `HOBOT_DRM_DEVICE` to override it. DirectVIV and KMS display tests continue to use `/dev/dri/card0`.

### 1. `test_directviv.c`
Validates that Vivante GC8000L proprietary OpenGL ES direct texture extension (`GL_VIV_direct_texture` / `glTexDirectVIVMap`) can bind contiguous NV12 physical memory allocated via Hobot ION memory manager directly to a `GL_TEXTURE_2D` texture.

### 2. `test_render_directviv.c`
Creates a `GL_VIV_NV12` direct texture and executes a complete GLES shader rendering pipeline to sample the NV12 texture into an RGB framebuffer, validating GPU hardware color space conversion.

### 3. `test_nv12_overlay.c`
Validates importing Hobot NV12 DMA-BUF into DRM KMS (`/dev/dri/card0`) using `drmPrimeFDToHandle` and `drmModeAddFB2` for hardware overlay planes.

### 4. `test_va_directviv_bench.c`
Full end-to-end benchmark pipeline:
1. Initializes VA-API with `libva-hobot` driver on `/dev/dri/card0`.
2. Hardware decodes high-bitrate H.264 video streams frame-by-frame via Wave521 VPU.
3. Exports each decoded surface's contiguous physical address via `vaExportSurfaceHandle`.
4. Maps physical addresses directly into Vivante GLES 2D textures using `glTexDirectVIVMap`.
5. Renders a full-size textured quad with zero CPU frame copying. The GBM target and GLES viewport use the input stream dimensions. The benchmark drains delayed frames at EOF and checks the rendered count when the container reports it. Set `HOBOT_DIRECTVIV_VALIDATE_PIXELS=1` to read five framebuffer points on the first frame and reject an all-black or uniform result; this is a nonblank-output smoke check, not a software pixel comparison.

On 2026-10-07, the corrected candidate-driver test rendered all 64 frames of a 1920x1080p60
H.264 High B6 sample at 92.43 FPS without readback; the optional framebuffer sample check also
passed (91.63 FPS). This validates full-size off-screen rendering and nonblank color samples, not
pixel-exact agreement with software or display presentation. The previously recorded 185.90 and
111.91 FPS figures are withdrawn: those runs rendered into a 64x64 GBM surface with a 1920x1080
viewport, so they were not full-frame measurements. Candidate SHA-256:
`8cc1b9b709e599bfdb7705e225354600e949dabfe05552709d808274ce6e153c`.

A second candidate-driver run decoded/rendered all 64 frames of a 640x360 H.264 B6 sample with a
640x360 target. The optional five-point framebuffer smoke check passed. Its short 0.16-second
duration is not treated as a meaningful throughput or sustained-playback measurement.

### 5. `test_va_config.c`
Checks the exact advertised matrix of five profiles and ten VLD/encode entrypoint pairs across
H.264 Constrained Baseline/Main/High, HEVC Main, and JPEG Baseline. It validates advertised
H.264 CBR/VBR/CQP and HEVC CBR/VBR/CQP, single-reference previous-frame
prediction and one-slice limits, creates H.264 and HEVC CQP/VBR configs, checks JPEG CQP capabilities,
config attribute round trips using an
output array sized by `vaMaxNumConfigAttributes()`, and unsupported-profile/entrypoint errors.
It uses the two-call `vaQuerySurfaceAttributes()` query to check H.264's 4096x4096, HEVC Main VLD's
8192x4096, and HEVC Main EncSlice's 3840x2160 limits, then creates an NV12 surface and validates CPU
mapping plus separate-layer and two-plane DRM PRIME 2 exports. Run with `LIBVA_DRIVERS_PATH` to test
a candidate without replacing the installed driver.

### 6. `test_va_surface_export.c`
Decodes the complete input stream through VA-API. It first synchronizes each decoded VPU surface
with a 1 ms `vaSyncSurface2` timeout, retries timed-out surfaces with `VA_TIMEOUT_INFINITE`, and
checks both NV12 two-plane and separate-layer DRM PRIME 2 exports, including plane bounds against
the exported DMA-BUF object sizes. It also derives each frame as an image and validates the
DRM PRIME buffer-handle acquire/release lifecycle. Use H.264 samples with and without B-frames to exercise output
ordering and repeated surface reuse; a successful run reports the number and dimensions of all
decoded frames and timeout retries. `HOBOT_DRM_DEVICE` selects the DRM node; use a render node to
avoid taking the display card device.

On 2026-10-07, the candidate driver validated 64 H.264 High B6 frames at 1920x1080 (54 timed
sync retries), 16 HEVC Main B3 frames at 3840x2160 (8 retries), and two HEVC Main frames at
8192x4096 (2 retries) on `/dev/dri/renderD128`. Every decoded frame passed both PRIME2 export
layouts and plane/object bounds validation. Candidate SHA-256:
`8cc1b9b709e599bfdb7705e225354600e949dabfe05552709d808274ce6e153c`.

### 6a. `test_va_surface_import.c`
Allocates an NV12 VA surface, verifies derived-image DRM PRIME buffer-handle acquire/release and
surface locking, writes a test pattern, exports and reimports it through composed-layer DRM PRIME 2,
then verifies the Y/UV bytes and physical layout through the imported surface. It closes the exported
FD and destroys the source before repeating the read to verify the imported reference remains valid,
then applies a `vaPutImage` patch to the surviving import and checks updated Y/UV bytes plus untouched
pixels. Build it with `make build-va-tests`; run against
a candidate driver named `hobot_drv_video.so` with `LIBVA_DRIVER_NAME=hobot` and
`LIBVA_DRIVERS_PATH` pointing to the candidate directory.

`mc_video_frame_buffer_info_t.vstride` from the Hobot codec SDK is the chroma byte pitch, not a
vertical row count. The driver validates and copies Y and UV planes with independent pitches; the
public `hobot_surface_info.vstride` remains the aligned luma height in lines.

### 7. `test_h264_bframe_decode.sh`
Generates H.264 High B6 clips at 640x360 Level 4.1 (64 frames), 1920x1080p60 Level 5.1 (64 frames),
and 3840x2160p30 Level 5.1 (16 frames), each with at least six consecutive B-frames. It verifies
the stream properties and frame pattern with `ffprobe`, then compares every hardware-decoded NV12
frame against FFmpeg's software decode using `framemd5`. Results are preserved in a new temporary
output directory.

```bash
bash tools/test_h264_bframe_decode.sh
```

### 7a. `test_h264_b_pyramid_decode.sh`
Generates a deterministic H.264 High stream with x264's B-pyramid enabled, verifies that the
stream has zero PPS L0/L1 reference defaults and at least one reference B slice, then compares all
64 Wave521-decoded NV12 frames against software decode. This covers PPS synthesis when picture and
slice parameters are submitted in separate `vaRenderPicture()` calls. The driver reads each slice's
reference-count override flags, derives PPS defaults only from inherited values, and accepts differing
counts where slices explicitly override them. Pass
`LIBVA_DRIVERS_PATH` to test a candidate without installing it.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_h264_b_pyramid_decode.sh
```

### 8. `test_va_jpeg_encode.c`
Encodes one patterned NV12 surface through the JPEG Baseline hardware entrypoint using the
requested VA picture quality (1..100), synchronizes the coded buffer with both zero and infinite
timeouts, then writes it to a JPEG file. Run it twice with different quality values and compare
output sizes or decode both files to verify that the per-picture quality setting reaches the JPU.

The driver also forwards loaded luma/chroma quantization tables and the JPEG scan restart
interval to the SDK. It accepts standard baseline Huffman tables and rejects custom tables,
progressive/multi-scan pictures, and unsupported component/table selectors instead of silently
pretending to honor them. The JPU's empty APP9 segment is removed from the returned JPEG so strict
decoders do not report an APP parsing error.

### 9. `test_h264_encode.sh`
Runs repeated H.264 VAAPI encode cycles for aligned 1280x720@60, visible 640x360 in a 640x368
coded frame, and 1280x720@30000/1001. It verifies the selected H.264 profile, level 4.1, frame rate,
limited-range color, frame count, and SPS level/VUI timing/cropping; then compares every
Wave521-decoded NV12 frame with software decode via `framemd5`. It tests CBR by default, and supports VBR; set
`HOBOT_H264_ENCODE_RC_MODE=CQP` to select CQP and `HOBOT_H264_ENCODE_QP` to choose QP 1–51
(default 26). This FFmpeg frontend maps `-qp 0` to VA's default QP; use
`HOBOT_H264_CROP_ENCODE_QP=0` with `test_h264_crop_encode.sh` for a direct VA QP-0 test.
Set `HOBOT_H264_ENCODE_PROFILE` to `constrained_baseline`, `main`, or `high`
(default `high`) to select the H.264 profile. CQP output is checked so
`pic_init_qp + slice_qp_delta` equals the requested QP.
The surface-state unit test also checks nonzero left/top crop offsets, Y/UV edge replication,
and CQP effective-QP bounds. The default is three cycles. Set `HOBOT_H264_ENCODE_CYCLES` to change
the count and `LIBVA_DRIVERS_PATH` to test a candidate driver without installing it system-wide.

`test_h264_crop_encode.sh` accepts `HOBOT_H264_CROP_ENCODE_QP=0..51` to run its three-profile,
three-frame crop case with direct VA CQP parameters and verify the emitted effective QP.

The encoder advertises a maximum of one slice per picture. Its fixed SDK GOP accepts I/P slices;
B/SP/SI slices, multiple or partial-frame slices, macroblock maps, and unsupported encoder buffer
or misc parameter types are rejected. VA HRD/VBV buffer size is converted to the SDK's 10–3000 ms
VBV window; the SDK does not expose a control for VA's initial CPB fullness, so that value is not
applied. The mocked state test covers conversion, invalid bounds, unsupported windows, and rollback
when the SDK rejects the rate-control update.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate HOBOT_H264_ENCODE_CYCLES=3 \
  HOBOT_H264_ENCODE_RC_MODE=CQP HOBOT_H264_ENCODE_QP=37 \
  bash tools/test_h264_encode.sh
```

### 10. `test_h264_crop_encode.sh`
Builds a small direct VA-API client and encodes a 638x360 visible surface in a 640x368 context
for Constrained Baseline, Main, and High, with nonzero SPS left/top cropping and
`max_num_ref_frames=0`. For each profile it verifies the emitted SPS crop offsets and
zero-reference declaration, confirms all three consecutive output pictures are I pictures in the
same VPU context, decodes each visible region, and compares them with the original NV12 frame using
a minimum PSNR threshold. Set `HOBOT_H264_CROP_ENCODE_QP=0..51` to use direct VA CQP parameters
and check the emitted effective QP. Pass `LIBVA_DRIVERS_PATH` to exercise a candidate without
installing it.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_h264_crop_encode.sh
```

### 11. `test_h264_profile_decode.sh`
Generates Constrained Baseline, Main, and High H.264 clips (including three consecutive B-frames
for Main and six for High), verifies the stream profile, then compares all 64 VPU-decoded NV12
frames against FFmpeg software decode. Results are preserved in a new temporary output directory.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_h264_profile_decode.sh
```

### 12. `test_h264_encode_profiles.sh`
Encodes 64-frame H.264 Constrained Baseline, Main, and High clips through VAAPI, verifies the
emitted profile/level and NV12 format, then compares every hardware-decoded frame against FFmpeg
software decode. The Constrained Baseline SPS/PPS syntax is checked for its constraint flag,
CAVLC, absent high-profile PPS extension fields, and disabled weighted prediction.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_h264_encode_profiles.sh
```

```bash
make test_va_jpeg_encode
LIBVA_DRIVER_NAME=hobot ./test_va_jpeg_encode 25 /tmp/jpeg-q25.jpg
LIBVA_DRIVER_NAME=hobot ./test_va_jpeg_encode 95 /tmp/jpeg-q95.jpg
```

### 13. `test_va_jpeg_decode.sh`
Generates a baseline sequential 8-bit 4:2:0 JPEG, decodes it through JPEG VLD, repeats the
picture in one VA context, and compares the first NV12 output with a full-range software reference.
The default is five frames; set `HOBOT_JPEG_DECODE_CYCLES` to change the count. Each hardware
decode is bounded to 20 seconds by default; `HOBOT_JPEG_DECODE_TIMEOUT` changes the limit. Pass
`LIBVA_DRIVERS_PATH` to validate a candidate driver without installing it.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate HOBOT_JPEG_DECODE_CYCLES=20 \
  bash tools/test_va_jpeg_decode.sh
```

### 13a. `test_va_jpeg_rotation.c` + `test_va_jpeg_rotation.sh`
Builds a VA-API integration test and decodes an asymmetric baseline JPEG at 0/90/180/270 degrees.
It checks the swapped output geometry and NV12 layout for quarter-turn rotations, then compares
each hardware result with a software-rotated reference. The default is five repeated pictures in
one VA context per angle; set `HOBOT_JPEG_ROTATION_CYCLES` to change the count. The script accepts
a directory containing a candidate `hobot_drv_video.so` and does not install it.

```bash
make /tmp/libva-hobot-tests/test_va_jpeg_rotation
LIBVA_DRIVERS_PATH=/path/to/candidate \
  bash tools/test_va_jpeg_rotation.sh /path/to/candidate
```

### 14. `test_hevc_main_decode.sh`
Generates HEVC Main 8-bit 4:2:0 streams at 640x360, 1280x720p60, 1920x1080p60 Level 5.0,
3840x2160p60 Level 5.1, and 8192x4096 Level 6.0 (two frames at 1 fps), including three- and
six-consecutive-B-frame cases. The 8K case runs five fresh VPU decode sessions by default; set
`HOBOT_HEVC_8K_CYCLES` to change the cycle count or `HOBOT_HEVC_8K_ONLY=1` to run only the 8K
case. This is a low-rate maximum-geometry check, not an 8K60 throughput test. It also generates a
no-B weighted-P and B3 weighted-bipred stream, verifying the corresponding PPS flags and
non-default L0/L1 luma offsets in their P/B slice headers. It checks profile, pixel format,
dimensions, encoded level, frame count, and B-frame run, then compares hardware-decoded NV12
frames with FFmpeg software decode using `framemd5`. A separate 64-frame Level 4.1 P case
exercises eight active L0 references over five fresh sessions by default; the script verifies its
SPS DPB, RPS, and active-reference counts before decoding. Set `HOBOT_HEVC_REF8_CYCLES` to change
that repetition count. Set `HOBOT_HEVC_REF8_ONLY=1` to run only this reference-count case.
Set `HOBOT_HEVC_WEIGHTED_ONLY=1` to run both weighted cases,
or `HOBOT_HEVC_WEIGHTED_P_ONLY=1` to run only the weighted-P case. The tested scope is one
complete long-format slice per picture; Main 10 and unsupported HEVC tools are not covered. The
zero-SPS-RPS inline syntax and single-SPS-RPS selection path (DPB-derived current references,
SAO flags, and P/B list order), along with VA `st_rps_bits`, active-reference counts, signaled list
modifications, and exact VA `RefPicList` surface-index order, are validated by the driver and mocked
state test. Pass
`LIBVA_DRIVERS_PATH` to test a candidate driver without installing it.

On 2026-10-07, the 8K-only case passed five fresh decode sessions with bit-exact NV12 output
against FFmpeg software decode using candidate SHA-256
`8cc1b9b709e599bfdb7705e225354600e949dabfe05552709d808274ce6e153c`. Artifacts are in
`/mnt/data/hobot-vpu-verify/hevc-main-decode-8k-codex-20261007`. This verifies the two-frame,
1-fps decode pattern only. The kernel journal showed five matched VPU open/close pairs and no
matching VPU, Oops, fault, timeout, or error records in the queried test interval.

```bash
HOBOT_HEVC_8K_ONLY=1 HOBOT_HEVC_8K_CYCLES=5 \
  LIBVA_DRIVER_NAME=hobot LIBVA_DRIVERS_PATH=/path/to/candidate \
  bash tools/test_hevc_main_decode.sh /path/to/new-output-directory
```

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_main_decode.sh
```

### 14a. `test_hevc_single_rps_decode.sh`
Checks the 32-picture x265 closed-GOP P fixture with one reference and one negative SPS RPS.
It verifies the SPS/slice syntax and stream geometry, then compares every hardware-decoded NV12
frame with software output in five fresh VPU sessions by default. Set
`HOBOT_HEVC_SINGLE_RPS_CYCLES` to change the cycle count (1–20). Pass `LIBVA_DRIVERS_PATH` to test
a candidate driver without installing it.

```bash
HOBOT_HEVC_SINGLE_RPS_CYCLES=5 LIBVA_DRIVER_NAME=hobot \
  LIBVA_DRIVERS_PATH=/path/to/candidate \
  bash tools/test_hevc_single_rps_decode.sh /path/to/new-output-directory
```

### 15. `test_hevc_4k60_throughput.sh`
Measures short-run HEVC Main 4K60 decode throughput by looping a verified input in one
VA-API process and downloading every frame to system memory. It requires at least 60 decoded
frames per second by default; set `HOBOT_MIN_DECODE_FPS` to change the threshold. The default
is 20 loops. Pass a 3840x2160, 60fps HEVC Main 8-bit 4:2:0 file, such as the `b3_4k60` sample
preserved by `test_hevc_main_decode.sh`.

On 2026-10-07, six fresh runs of the 16-frame B3 sample (20 loops, 320 frames per run) on the
candidate (SHA-256 `8cc1b9b709e599bfdb7705e225354600e949dabfe05552709d808274ce6e153c`)
decoded at 65.04–67.09 fps; all six exceeded the 60 fps threshold. This
includes hardware-frame download and does not measure display presentation or long-duration
thermal stability. An earlier four-run record in this file reported 72.87–73.16 fps; that
range was not reproduced in this session. The cause is unknown because the earlier binary hash
and system conditions were not recorded.

Five consecutive fresh sessions, each using 100 loops (1,600 frames), measured 73.26–73.36 fps
on the same candidate. CPU/DDR thermal zones rose by about 4.3C over roughly two minutes. The
kernel journal showed five matched VPU open/close pairs and no matching Oops, fault, timeout, or
error records. One separate 100-loop run on a strict build of the current source decoded 1,600
frames at 73.13 fps; it is not pooled with the five-session range, and temperature was not
measured for that run. These runs are still too short to establish sustained thermal behavior.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate \
  bash tools/test_hevc_4k60_throughput.sh /path/to/hevc-main-b3_4k60.mp4 20
```

### 16. `test_hevc_main_encode.sh`
Encodes a 64-frame HEVC Main no-B stream with VAAPI (CBR by default; VBR and CQP can be selected), checks profile, level, visible/coded
dimensions, frame count, and that SAO is disabled in the SPS, then compares every hardware
re-decoded frame hash with software.
The tested geometries are 640x360, 638x360, 638x362, and 626x354 in a 640x368 coded frame. They
exercise bottom-only through 14-pixel right/bottom SPS conformance-window crops and the validated
two-set single-reference RPS path. Every case checks the emitted SAO-disabled SPS and compares all
hardware re-decoded frames bit-exactly with software. Separate 16-frame 3840x2160 Level 5.1 no-B
CBR samples encoded at 30fps and 60fps also re-decoded bit-exactly. These validate short
maximum-geometry output correctness, not sustained 4K60 throughput. Other geometry combinations
are not exhaustively validated. Set
`HOBOT_HEVC_ENCODE_WIDTH` and `HOBOT_HEVC_ENCODE_HEIGHT` to test a different visible size.
Set `HOBOT_HEVC_ENCODE_RC_MODE=CQP` and `HOBOT_HEVC_ENCODE_QP=1..51` to exercise FFmpeg CQP;
the FFmpeg path does not submit QP 0 reliably, so use the direct-VA `test_hevc_cqp_encode.sh`
for the QP 0 boundary.

On 2026-10-07, both 16-frame 4K encode rates passed with candidate SHA-256
`8cc1b9b709e599bfdb7705e225354600e949dabfe05552709d808274ce6e153c`. The 30fps and 60fps
artifacts are in `/mnt/data/hobot-vpu-verify/candidate-suite-20261007/tmp/hobot-hevc-encode.rjzZOy`
and `/mnt/data/hobot-vpu-verify/candidate-suite-20261007/tmp/hobot-hevc-encode.vKu5jy` respectively.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_main_encode.sh
```

`test_hevc_cqp_encode.sh` separately tests effective-QP boundaries, per-picture I/P QP and slice
QP deltas, WPP output, invalid-QP rejection, bitstream QP values, and software/hardware decode
equivalence. It covers selected cases only, not arbitrary streams or sustained throughput.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_cqp_encode.sh
```

### 17. `test_hevc_multislice_decode.sh`
Uses a checked-in 640x360 HM fixture containing one IDR and two P pictures, each with two
independent slices at CTU addresses 0 and 30. FFmpeg `trace_headers` verifies the slice topology,
then candidate-driver VPU output is compared frame-by-frame with software NV12 output. This
exercises empty-DPB IDR and single-reference P pictures selecting RPS 0.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_multislice_decode.sh
```

### 18. `test_hevc_multislice_b_decode.sh`
Uses a checked-in 640x360 HM fixture containing one IDR followed by three B pictures, each with
two independent slices at CTU addresses 0 and 30. Each B slice uses the same immediately previous
POC as its sole L0 and L1 reference. FFmpeg `trace_headers` verifies the slice topology, then
candidate-driver VPU output is compared frame-by-frame with software NV12 output over five fresh
VPU sessions by default. Set `HOBOT_HEVC_MULTISLICE_B_CYCLES` to change the repetition count.
The `test_va_surface_state` unit suite also checks luma/chroma weighted P/B syntax for the
two-SPS-RPS parser, inline RPS counts/flags and `st_rps_bits` length, inline RPS-to-VA POC/category,
active-reference counts, default P/B list order and signaled list modifications, including
weight/offset limits and byte-boundary rejection.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_multislice_b_decode.sh
```

### 19. `test_hevc_two_rps_weighted_decode.sh`
Adds syntax-valid neutral, non-default luma, and chroma weighted-P/B tables to the checked-in
two-SPS-RPS HM fixtures. The generator verifies SPS RPS counts, denominators, and applicable
L0/L1 weight flags and values. Neutral tables must preserve software output; luma and chroma
tables use denominator 0, delta weight 0, and offset +8 for each applicable list/component and
must change software output. Candidate-driver output is compared bit-exactly with software for
three P-pattern frames and four B-pattern frames per profile. It runs five cycles by default;
set `HOBOT_HEVC_TWO_RPS_WEIGHTED_CYCLES` to change the count. Other RPS/weight-table layouts
remain unverified.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_two_rps_weighted_decode.sh
```

### 20. `test_hevc_inline_refmod_decode.sh`
Uses checked-in 640x360 HEVC Main P and B fixtures with zero SPS short-term RPS sets. FFmpeg
`trace_headers` verifies actual P L0 reordering `[1,0]` and B L0/L1 reordering `[1,2,0]` with
three current references. Each fixture's 8 decoded NV12 pictures are compared bit-exactly with
software output in five fresh VPU sessions by default; set `HOBOT_HEVC_INLINE_REFMOD_CYCLES` to
change the cycle count. Other RPS/list layouts and the separate two-SPS-RPS path remain unverified.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_inline_refmod_decode.sh
```

### 21. `test_hevc_single_rps_sao_decode.sh`
Uses a checked-in two-picture HEVC Main fixture with SAO enabled and exactly one negative SPS
RPS selected by its P slice. FFmpeg `trace_headers` validates the SAO and RPS syntax, then the
candidate driver's hardware output is compared bit-exactly with software over five fresh VPU
sessions by default. Set `HOBOT_HEVC_SAO_RPS_CYCLES` to change the repetition count.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_single_rps_sao_decode.sh
```

### 22. `test_hevc_two_rps_sao_decode.sh`
Transforms the checked-in SAO-enabled single-RPS P fixture into an equivalent two-SPS-RPS
stream by adding a duplicate negative-reference set and selecting RPS index 0. It also checks a
two-picture HM B fixture whose B slice selects RPS index 0 and uses the immediately previous POC
as its sole L0 and L1 reference. FFmpeg `trace_headers` verifies both streams, including luma and
chroma SAO; the B fixture also requires temporal MVP to be disabled. The script additionally adds
non-default L0/L1 luma offset weights (+8) to that B-SAO picture and verifies the combined syntax
and changed software output. It also adds Cb/Cr offset +8 weights to both reference lists and
checks the combined weighted-B-SAO syntax and software output. Candidate-driver output is compared
bit-exactly against software in five fresh VPU sessions per pattern by default. Other two-RPS SAO
and weight-table combinations remain unverified. Set `HOBOT_HEVC_TWO_RPS_SAO_CYCLES` to change the
repetition count.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_two_rps_sao_decode.sh
```

### 23. `test_hevc_single_rps_multislice_decode.sh`
Uses a checked-in 640x360 HM fixture with one IDR and three P pictures. Every picture has two
independent slices at CTU addresses 0 and 30; each P slice selects the only SPS short-term RPS.
FFmpeg header tracing checks the slice topology, independent-slice flags, and RPS selection, then
candidate-driver output is compared bit-exactly with software. This covers the single-RPS slice
header rewrite on a later independent slice, complementing the isolated rewrite unit test.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_single_rps_multislice_decode.sh
```

### 24. `test_h264_multislice_decode.sh`
Generates a 64-frame H.264 High 640x360 stream with six consecutive B-frames and two slices per
picture. FFmpeg `trace_headers` checks all 128 slice starts (macroblocks 0 and 480), then the
candidate-driver VPU output is compared frame-by-frame with software NV12 output.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_h264_multislice_decode.sh
```

### 25. `test_hevc_inline_rps_sao_decode.sh`
Generates a six-picture 640x360 HEVC Main stream with FFmpeg/libx265, zero SPS RPS sets, inline
RPS, I/P/B slices, and luma/chroma SAO enabled. `trace_headers` validates the relevant syntax,
then candidate-driver output is compared bit-exactly with software over five fresh VPU sessions
by default. This covers the generated inline-RPS pattern only; B-slice SAO with an SPS-selected
RPS is not hardware-verified. The separate two-SPS-RPS B-SAO parser acceptance, metadata mismatch,
and header-boundary cases are unit-tested in `test_va_surface_state`. Set
`HOBOT_HEVC_INLINE_SAO_CYCLES` to change the repetition count.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_inline_rps_sao_decode.sh
```

### 26. `test_vpu_concurrent_decode.sh`
Runs independent H.264 and HEVC VA-API decoders concurrently in separate FFmpeg processes.
Both inputs must be 8-bit `yuv420p` and contain at least the requested number of frames (64 by
default). Software reference hashes are generated first; while the VPU sessions overlap, each
hardware-decoded NV12 output is compared bit-exactly with its reference. Timeout defaults to 120
seconds and can be changed with `HOBOT_CONCURRENT_DECODE_TIMEOUT`. Logs and hashes are retained in
a new temporary directory.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate \
  bash tools/test_vpu_concurrent_decode.sh h264-high-b6.mp4 hevc-main-b3.mp4 64
```

`test_vpu_multicontext_decode.sh` is the same-process companion: it muxes the two video streams
into one container, decodes both VA contexts in one FFmpeg process, and compares each stream with
its software reference. Set `HOBOT_MULTICONTEXT_DECODE_CYCLES` to repeat the run and
`HOBOT_MULTICONTEXT_DECODE_TIMEOUT` to change its per-cycle timeout.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate \
  bash tools/test_vpu_multicontext_decode.sh h264-high-b6.mp4 hevc-main-b3.mp4 64
```

### 27. `test_hevc_two_rps_tmvp_decode.sh`
Generates a two-frame 640x360 HEVC Main temporal-MVP stream, then creates equivalent
two-SPS-RPS variants whose P slice selects SPS RPS index 0 or carries the RPS inline. It verifies
the syntax with `trace_headers`, confirms both rewrites preserve software NV12 output, and compares
candidate-driver output bit-exactly over five fresh VPU sessions by default. This covers the
two-RPS temporal-MVP path that canonicalizes the slice RPS before Wave521 decode. Set
`HOBOT_HEVC_TWO_RPS_TMVP_CYCLES` to change the repetition count.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_two_rps_tmvp_decode.sh
```

### 28. `test_hevc_two_rps_b_tmvp_decode.sh`
Checks a checked-in five-picture HM 16.20 fixture: one IDR followed by four 640x360 low-delay B
pictures. Each B slice selects SPS RPS index 0, uses the immediately previous POC as its only
L0/L1 reference, enables temporal MVP and luma/chroma SAO, disables active-reference override, and
sets `collocated_from_l0_flag=0` to select the collocated reference from L1. `trace_headers` checks
the fixture syntax; every candidate-driver NV12 frame
is compared bit-exactly with software over five fresh VPU sessions by default. This validates only
this exact B-slice TMVP pattern, not arbitrary reference lists or collocated-reference layouts.
Set `HOBOT_HEVC_TWO_RPS_B_TMVP_CYCLES` to change the cycle count.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_two_rps_b_tmvp_decode.sh
```

### 29. `test_hevc_three_slice_decode.sh`
Checks a checked-in four-picture HM 16.20 640x360 HEVC Main fixture with three independent
slices per picture at CTU addresses 0, 20, and 40. WPP, tiles, TMVP, and SAO are disabled.
`trace_headers` verifies all slice boundaries and the disabled WPP flag; every candidate-driver
NV12 frame is compared bit-exactly with software over five fresh VPU sessions by default. This
validates this exact three-slice IDR+P pattern, not arbitrary multislice inter streams. Set
`HOBOT_HEVC_THREE_SLICE_CYCLES` to change the cycle count.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_three_slice_decode.sh
```

### 30. `test_hevc_open_gop_decode.sh`
Generates two 640x360, 64-frame x265 Main open-GOP streams with CRA and RASL leading pictures,
using B-pyramid off and on. `trace_headers` verifies CRA, RASL_N, the RASL_R case, and an inline
CRA RPS with four unused following references. Each stream is compared bit-exactly with software
over five fresh VPU sessions by default. This covers the exact generated CRA/RASL patterns, not
RADL, long-term references, or arbitrary CRA/RPS layouts. Set `HOBOT_HEVC_OPEN_GOP_CYCLES` to
change the cycles per variant and `HOBOT_HEVC_OPEN_GOP_TIMEOUT` to adjust the per-run timeout.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_open_gop_decode.sh
```

### 31. `test_hevc_two_rps_multiref_b_decode.sh`
Checks an eight-picture 640x360 HEVC Main fixture with two SPS RPS entries. Its POC-4 B slice
selects RPS index 0 with four negative current references, enables TMVP and luma/chroma SAO, and
reorders both active lists as `[1,2,3,0]`. The other slices retain inline RPS syntax. The script
checks the fixture fields with `trace_headers`, then compares all eight candidate-driver NV12
frames with software over five fresh VPU sessions by default. This validates that exact selected-RPS
and list pattern only; it does not imply support for other RPS indices or layouts. Set
`HOBOT_HEVC_TWO_RPS_MULTIREF_CYCLES` to change the repetition count.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_two_rps_multiref_b_decode.sh
```

### 32. `test_hevc_three_rps_decode.sh`
Generates two two-picture 640x360 HEVC Main single-reference P streams with FFmpeg/libx265. One
transforms its inline RPS into three SPS RPS entries and selects index 2; the other creates the
maximum 64 entries and selects index 63. `trace_headers` and `ffprobe` verify the syntax, profile,
geometry, and frame count. Each transformed stream must retain software NV12 frame hashes;
candidate-driver output is then compared bit-exactly over five fresh VPU sessions per case by
default. Hardware coverage is limited to these two patterns. Set `HOBOT_HEVC_THREE_RPS_CYCLES` to
change the cycle count.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_three_rps_decode.sh
```

### 33. `test_hevc_wpp_decode.sh`
Generates a 32-frame 640x360 HEVC Main stream with WPP enabled and tiles disabled. It verifies the
parameter-set and entry-point syntax with `trace_headers`, then compares candidate-driver NV12
output against software across five fresh VPU sessions. Main-profile validation rejects a PPS that
enables both WPP and tiles. Other WPP layouts are not covered. Set
`HOBOT_HEVC_WPP_CYCLES` to change the cycle count.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_wpp_decode.sh
```

### 34. `test_hevc_wpp_encode.sh`
Encodes a 640x360 HEVC Main WPP IDR/P pair through a direct VAAPI client. It verifies I/P
picture order and the SDK-emitted dependent row-segment layout for both pictures with
`trace_headers`, then compares software and candidate-driver hardware decode hashes. The
single-reference P pattern passed five fresh sessions; B-frame WPP and other geometries are not
validated.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_wpp_encode.sh
```

### 35. `test_hevc_pcm_decode.sh`
Uses checked-in 64x64 HM fixtures with actual 8x8, 16x16, and 32x32 PCM coding units, covering
both PCM loop-filter settings. SHA-256 checks bind each fixture to its CU-size verification;
FFmpeg `trace_headers` checks the 8-bit 4:2:0 PCM SPS bounds. Each
candidate-driver decode is compared bit-exactly with software over five fresh VPU sessions by
default. The accepted SPS bounds are 8-32 samples; 8-bit 4:2:0 is the only tested PCM format.
Set `HOBOT_HEVC_PCM_CYCLES` to change the count.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_pcm_decode.sh
```

### 36. `test_hevc_tiles_decode.sh`
Uses an HM 18.0 640x360 HEVC Main IDR fixture with uniform 2x2 tiles. It verifies the tile and
entry-point syntax with `trace_headers`, then compares candidate-driver NV12 output bit-exactly
with software over five fresh VPU sessions. Only this single-whole-picture-slice tile layout is
verified by this test; explicit non-uniform and the tested four-slice-per-tile layouts are covered
below. Main-profile validation enforces CTB sizes 16-64 and minimum tile dimensions 256x64 luma
samples. Other geometries remain unverified; WPP combined with tiles is forbidden by the Main
profile and rejected by picture-parameter validation. Set
`HOBOT_HEVC_TILES_CYCLES` to change the cycle count.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_tiles_decode.sh
```

### 37. `test_hevc_tiles_nonuniform_decode.sh`
Uses an HM 18.0 640x360, 8-frame HEVC Main P stream with explicit non-uniform 2x2 tiles: 6/4 CTB
columns and 4/2 CTB rows. It validates the PPS geometry and three entry points, then compares
candidate-driver NV12 output bit-exactly with software over five fresh VPU sessions. Other tile
geometries remain unverified. Set `HOBOT_HEVC_TILES_NONUNIFORM_CYCLES` to change the cycle count.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_tiles_nonuniform_decode.sh
```

### 38. `test_hevc_tiles_multislice_decode.sh`
Uses an HM 18.0 640x360, 8-frame HEVC Main stream with uniform 2x2 tiles and four independent
slices per picture, one starting at each tile. It validates the three explicit slice addresses
and zero entry points per slice, then compares candidate-driver NV12 output bit-exactly with
software over five fresh VPU sessions. Other tile geometries remain unverified; WPP combined with
tiles is forbidden by the Main profile and rejected by picture-parameter validation. Set
`HOBOT_HEVC_TILES_MULTISLICE_CYCLES` to change the cycle count.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_tiles_multislice_decode.sh
```

### 39. `test_hevc_tiles_nonuniform_multislice_decode.sh`
Uses an HM 18.0 640x360, 8-frame HEVC Main B/P stream with explicit non-uniform 2x2 tiles
(6/4 CTB columns, 4/2 CTB rows) and four independent slices per picture, one at each tile.
It verifies the tile geometry, three explicit slice addresses, and zero entry points per slice,
then compares candidate-driver NV12 output bit-exactly with software over five fresh VPU sessions.
Other geometries remain unverified. Set `HOBOT_HEVC_TILES_NONUNIFORM_MULTISLICE_CYCLES` to change
the cycle count.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_tiles_nonuniform_multislice_decode.sh
```

### 40. `test_vbr_encode.sh`
Runs H.264 and HEVC VA VBR hardware encoding through FFmpeg, then compares hardware re-decode
against software. A direct-VA HEVC case changes maximum bitrate, target percentage, and VBV window
between pictures. This checks accepted controls and output integrity, not target-bitrate accuracy
for arbitrary content. Pass a new output directory to preserve logs and bitstreams.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate \
  bash tools/test_vbr_encode.sh /path/to/new-output-directory
```
