# Verification and Benchmark Tools

This directory contains standalone test and benchmark tools for validating hardware zero-copy video playback pipelines on the D-Robotics RDK-X5 platform.

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
5. Renders textured quads at up to 180+ FPS with zero CPU copying and zero frame drops.

### 5. `test_va_config.c`
Creates an H.264 VLD configuration and calls `vaQueryConfigAttributes`, then creates an NV12
surface and verifies CPU mapping through `vaLockSurface`/`vaUnlockSurface` before destroying
the configuration and VA display. This covers the driver's config mutex, surface mapping, and
configuration validation paths.

### 6. `test_va_surface_export.c`
Decodes the complete input stream through VA-API. It synchronizes every decoded VPU surface and
checks both NV12 two-plane and separate-layer DRM PRIME 2 exports, including plane bounds against
the exported DMA-BUF object sizes. Use H.264 samples with and without B-frames to exercise output
ordering and repeated surface reuse; a successful run reports the number and dimensions of all
decoded frames.

`mc_video_frame_buffer_info_t.vstride` from the Hobot codec SDK is the chroma byte pitch, not a
vertical row count. The driver validates and copies Y and UV planes with independent pitches; the
public `hobot_surface_info.vstride` remains the aligned luma height in lines.

### 7. `test_h264_bframe_decode.sh`
Generates a deterministic 64-frame H.264 High clip with at least six consecutive B-frames, verifies
that frame pattern with `ffprobe`, then compares every hardware-decoded NV12 frame against FFmpeg's
software decode using `framemd5`. Results are preserved in a new temporary output directory.

```bash
bash tools/test_h264_bframe_decode.sh
```

### 8. `test_va_jpeg_encode.c`
Encodes one patterned NV12 surface through the JPEG Baseline hardware entrypoint using the
requested VA picture quality (1..100), then writes the coded buffer to a JPEG file. Run it twice
with different quality values and compare output sizes or decode both files to verify that the
per-picture quality setting reaches the JPU.

The driver also forwards loaded luma/chroma quantization tables and the JPEG scan restart
interval to the SDK. It accepts standard baseline Huffman tables and rejects custom tables,
progressive/multi-scan pictures, and unsupported component/table selectors instead of silently
pretending to honor them. The JPU's empty APP9 segment is removed from the returned JPEG so strict
decoders do not report an APP parsing error.

### 9. `test_h264_encode.sh`
Runs repeated H.264 VAAPI encode cycles for aligned 1280x720@60, visible 640x360 in a 640x368
coded frame, and 1280x720@30000/1001. It verifies High profile, level 4.1, frame rate,
limited-range color, frame count, and SPS level/VUI timing/cropping; then compares every
Wave521-decoded NV12 frame with software decode via `framemd5`. The surface-state unit test also
checks nonzero left/top crop offsets and Y/UV edge replication. The default is three cycles. Set
`HOBOT_H264_ENCODE_CYCLES` to change the count and `LIBVA_DRIVERS_PATH` to test a candidate driver
without installing it system-wide.

The encoder advertises a maximum of one slice per picture. Its fixed SDK GOP accepts I/P slices;
B/SP/SI slices, multiple or partial-frame slices, macroblock maps, and unsupported encoder buffer
or misc parameter types are rejected. VA HRD/VBV buffer size is converted to the SDK's 10–3000 ms
VBV window; the SDK does not expose a control for VA's initial CPB fullness, so that value is not
applied. The mocked state test covers conversion, invalid bounds, unsupported windows, and rollback
when the SDK rejects the rate-control update.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate HOBOT_H264_ENCODE_CYCLES=3 \
  bash tools/test_h264_encode.sh
```

### 10. `test_h264_crop_encode.sh`
Builds a small direct VA-API client and encodes a 638x360 visible surface in a 640x368 context
with nonzero SPS left/top cropping. It verifies the emitted SPS crop offsets, decodes the visible
region, and compares it with the original NV12 frame using a minimum PSNR threshold. Pass
`LIBVA_DRIVERS_PATH` to exercise a candidate without installing it.

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
The default is five frames; set `HOBOT_JPEG_DECODE_CYCLES` to change the count. Pass
`LIBVA_DRIVERS_PATH` to validate a candidate driver without installing it.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate HOBOT_JPEG_DECODE_CYCLES=20 \
  bash tools/test_va_jpeg_decode.sh
```

### 14. `test_hevc_main_decode.sh`
Generates HEVC Main 8-bit 4:2:0 streams at 640x360, 1280x720p60, 1920x1080p60 Level 5.0, and
3840x2160p60 Level 5.1, including three- and six-consecutive-B-frame cases. It also generates a
no-B weighted-P and B3 weighted-bipred stream, verifying the corresponding PPS flags and
non-default L0/L1 luma offsets in their P/B slice headers. It checks profile, pixel format,
dimensions, encoded level, frame count, and B-frame run, then compares hardware-decoded NV12
frames with FFmpeg software decode using `framemd5`. Set `HOBOT_HEVC_WEIGHTED_ONLY=1` to run both weighted cases,
or `HOBOT_HEVC_WEIGHTED_P_ONLY=1` to run only the weighted-P case. The tested scope is one
complete long-format slice per picture; Main 10 and unsupported HEVC tools are not covered. The
zero-SPS-RPS inline syntax and single-SPS-RPS selection path (DPB-derived current references,
SAO flags, and P/B list order), along with VA `st_rps_bits`, active-reference counts, signaled list
modifications, and exact VA `RefPicList` surface-index order, are validated by the driver and mocked
state test. The
60 fps values are stream timestamps, not a sustained decode-throughput measurement. Pass
`LIBVA_DRIVERS_PATH` to test a candidate driver without installing it.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_main_decode.sh
```

### 15. `test_hevc_main_encode.sh`
Encodes a 64-frame HEVC Main no-B CBR stream with VAAPI, checks profile, level, visible/coded
dimensions, frame count, and that SAO is disabled in the SPS, then compares every hardware
re-decoded frame hash with software.
The tested geometries are 640x360, 638x360, 638x362, and 626x354 in a 640x368 coded frame. They
exercise bottom-only through 14-pixel right/bottom SPS conformance-window crops and the validated
two-set single-reference RPS path. Every case checks the emitted SAO-disabled SPS and compares all
hardware re-decoded frames bit-exactly with software. Other geometry combinations are not
exhaustively validated. Set `HOBOT_HEVC_ENCODE_WIDTH` and `HOBOT_HEVC_ENCODE_HEIGHT` to test a
different visible size.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_main_encode.sh
```

### 16. `test_hevc_multislice_decode.sh`
Uses a checked-in 640x360 HM fixture containing one IDR and two P pictures, each with two
independent slices at CTU addresses 0 and 30. FFmpeg `trace_headers` verifies the slice topology,
then candidate-driver VPU output is compared frame-by-frame with software NV12 output. This
exercises empty-DPB IDR and single-reference P pictures selecting RPS 0.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_multislice_decode.sh
```

### 17. `test_hevc_multislice_b_decode.sh`
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

### 18. `test_hevc_two_rps_weighted_decode.sh`
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

### 19. `test_hevc_inline_refmod_decode.sh`
Uses checked-in 640x360 HEVC Main P and B fixtures with zero SPS short-term RPS sets. FFmpeg
`trace_headers` verifies actual P L0 reordering `[1,0]` and B L0/L1 reordering `[1,2,0]` with
three current references. Each fixture's 8 decoded NV12 pictures are compared bit-exactly with
software output in five fresh VPU sessions by default; set `HOBOT_HEVC_INLINE_REFMOD_CYCLES` to
change the cycle count. Other RPS/list layouts and the separate two-SPS-RPS path remain unverified.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_inline_refmod_decode.sh
```

### 20. `test_hevc_single_rps_sao_decode.sh`
Uses a checked-in two-picture HEVC Main fixture with SAO enabled and exactly one negative SPS
RPS selected by its P slice. FFmpeg `trace_headers` validates the SAO and RPS syntax, then the
candidate driver's hardware output is compared bit-exactly with software over five fresh VPU
sessions by default. Set `HOBOT_HEVC_SAO_RPS_CYCLES` to change the repetition count.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_single_rps_sao_decode.sh
```

### 21. `test_hevc_single_rps_multislice_decode.sh`
Uses a checked-in 640x360 HM fixture with one IDR and three P pictures. Every picture has two
independent slices at CTU addresses 0 and 30; each P slice selects the only SPS short-term RPS.
FFmpeg header tracing checks the slice topology, independent-slice flags, and RPS selection, then
candidate-driver output is compared bit-exactly with software. This covers the single-RPS slice
header rewrite on a later independent slice, complementing the isolated rewrite unit test.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_hevc_single_rps_multislice_decode.sh
```

### 22. `test_h264_multislice_decode.sh`
Generates a 64-frame H.264 High 640x360 stream with six consecutive B-frames and two slices per
picture. FFmpeg `trace_headers` checks all 128 slice starts (macroblocks 0 and 480), then the
candidate-driver VPU output is compared frame-by-frame with software NV12 output.

```bash
LIBVA_DRIVERS_PATH=/path/to/candidate bash tools/test_h264_multislice_decode.sh
```
