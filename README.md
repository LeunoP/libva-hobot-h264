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
- **Encoder rate control**: H.264 and HEVC EncSlice support CBR, VA VBR, and CQP. VA VBR maps maximum bitrate, target percentage, and window size to the SDK's AVBR target, separate maximum-bitrate control, and VBV window. VA AVBR is not advertised because equivalent convergence and accuracy semantics are not established.
- **Legacy surface-attribute query**: The deprecated `vaGetSurfaceAttributes` backend callback returns the same NV12, memory-type, external-buffer, and size attributes as `vaQuerySurfaceAttributes` for older libva clients.
- **H.264 B-frame handling**: Uses the VPU reorder setting and a 128-entry FIFO (`submitted_surfaces`) to associate submitted surfaces with decoded output. H.264 High samples with six consecutive B-frames were hardware-decoded bit-exactly against software NV12 output at 640x360 Level 4.1 (64 frames), 1920x1080p60 Level 5.1 (64 frames), and 3840x2160p30 Level 5.1 (16 frames); run `tools/test_h264_bframe_decode.sh` to repeat these regressions.
- **H.264 encoder sequence setup**: Defers VPU startup until the first encoded picture so the requested H.264 level and sequence VUI timing/SAR are applied before initialization. VA-API does not expose input full-range metadata here, so NV12 output is explicitly signaled as limited-range.
- **External H.264/HEVC encoder input**: Uses MediaCodec external-frame mode to submit compatible contiguous NV12 VA surfaces directly, avoiding a CPU frame copy. Crop/padding or incompatible layouts use a per-context contiguous hbmem scratch buffer; source-surface ownership is retained until the SDK reports input consumption. Hardware checks cover direct 640x368 input and a 638x360-to-640x368 crop/pad path.
- **H.264 visible-size surfaces**: Accepts exact visible-size NV12 surfaces when H.264 coded dimensions are macroblock padded. The input is placed at the SPS crop offset in the coded frame and edge pixels are replicated; full coded-size surfaces retain their original coordinates.
- **H.264 Parameter-Set Refresh**: Detects changed synthesized SPS/PPS data and re-injects the parameter sets before the next picture, keeping decoder state aligned across stream renegotiation.
- **VPU Output Integrity & Corrupted Frame Drop**: Checks the SDK decode result as well as `err_mb_in_frame_display`. H.264/HEVC `FAIL` (`0x00`) and undocumented result values are dropped; `SUCCESS_WITH_WARNING` (`0x10`) remains usable unless corruption fields require a drop, while still triggering the fallback watchdog. Only `SUCCESS` (`0x01`) is treated as clean.
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

> **Note on HEVC**: The earlier corrupted output was traced to synthesized parameter sets using defaults that did not match the VA picture parameters. The driver now synthesizes VPS/SPS/PPS from the submitted values and exposes only the tested Main 8-bit 4:2:0 subset. Encoding supports CBR, VA VBR, and CQP for I/P frames with one full-frame slice. VBR maps to the SDK AVBR target/VBV controls plus a separate maximum-bitrate control; VA AVBR is not advertised because matching convergence and accuracy semantics are unverified. HEVC CQP uses effective per-picture QP values from 0 through 51; target bitrate is ignored and HRD parameters are rejected. VLD PCM accepts 8-bit 4:2:0 parameters with PCM block-size bounds from 8 to 32 samples; hardware was verified with actual 8×8, 16×16, and 32×32 PCM coding units and both PCM loop-filter settings. Main 10 and other unsupported bitstream features remain unavailable.

## VA-API Scope & Limitations

One separate 100-loop run on a strict build of the current source decoded the 1,600-frame 4K60 HEVC B3 workload at 73.13 fps. This single run is separate from the earlier five-session 73.26–73.36 fps range; temperature was not measured for it.

For HEVC SPS data declaring zero or one short-term RPS set, the VLD path validates both inline and SPS-selected reference sets. Inline syntax is parsed for negative/positive picture counts and per-entry delta/usage flags, checked against VA-API `st_rps_bits`, and validated against active-reference counts and optional list modifications. When the sole SPS RPS is selected, current references are reconstructed from the VA DPB flags and POCs; SAO flags, default P/B list order, and VA `RefPicList` indices are checked. Long-term references remain unsupported, and other slice tools are limited to the explicitly validated patterns below.

The tested inline-RPS path now covers eight active L0 references. Wave521 needs 17 decoder frame buffers for this case: a 64-frame Level 4.1 stream with an SPS DPB size of nine is checked in five fresh sessions by `test_hevc_main_decode.sh`.

A checked-in two-picture, SAO-enabled single-SPS-RPS P fixture (640×360) decoded bit-exactly against software in five fresh candidate-driver VPU sessions (`test_hevc_single_rps_sao_decode.sh`).

A 32-picture x265 closed-GOP P fixture with one reference and one negative SPS RPS decoded bit-exactly against software in five fresh VPU sessions (`test_hevc_single_rps_decode.sh`). The script accepts `HOBOT_HEVC_SINGLE_RPS_CYCLES` to change the cycle count.

The fixture's equivalent two-SPS-RPS form selects duplicate RPS index 0 and preserves the software NV12 hashes. Its SAO-enabled P path decoded bit-exactly on the candidate driver in 5/5 fresh VPU sessions. A separate HM 16.20 B fixture with two SPS RPS sets, index 0, one immediately previous reference in both lists, SAO enabled, and temporal MVP disabled also decoded bit-exactly in 5/5 fresh sessions. The same B-SAO fixture with non-default L0/L1 luma offset weights (+8) and with non-default Cb/Cr offset weights (+8) in both reference lists matches software; each tested variant runs in five fresh sessions (`test_hevc_two_rps_sao_decode.sh`). Other two-RPS B-SAO flag and weight-table combinations remain unverified. A generated six-picture x265 Main sample with zero SPS RPS sets, inline RPS, I/P/B slices, and luma/chroma SAO also matched software bit-exactly in 5/5 fresh sessions (`test_hevc_inline_rps_sao_decode.sh`).

Open-GOP support is deliberately narrow: a CRA's inline short-term RPS must contain no current references, and every unused RPS POC must match exactly one non-current VA DPB surface. RASL_N/R pictures (NAL types 8/9) then pass through the existing inline-RPS checks. Two generated 640×360, 64-frame x265 Main streams with B-pyramid off/on decoded bit-exactly in five fresh VPU sessions each (`test_hevc_open_gop_decode.sh`); the B-pyramid stream also exercises RASL_R. RADL pictures, long-term references, and other CRA/RPS layouts remain unsupported or unverified.

Narrow temporal-MVP exceptions are hardware-verified. Two-frame 640×360 two-SPS-RPS P streams with a single immediately previous reference decoded bit-exactly in five fresh VPU sessions per variant, for both SPS-selected RPS-index-0 and inline-RPS forms (`test_hevc_two_rps_tmvp_decode.sh`). A separate 640×360 HM 16.20 stream with one IDR plus four low-delay B pictures, two SPS RPS entries, selected index 0, the immediately previous POC as the sole L0/L1 reference, SAO enabled, and TMVP enabled decoded bit-exactly in five fresh candidate-driver VPU sessions (`test_hevc_two_rps_b_tmvp_decode.sh`). One additional 640×360 B fixture validates a selected index-0 RPS containing four negative current references, TMVP, SAO, and L0/L1 reordering `[1,2,3,0]`; all eight frames matched software in five fresh candidate-driver sessions (`test_hevc_two_rps_multiref_b_decode.sh`). These results do not imply support for other selected RPS indices, DPB/RPS layouts, B-slice TMVP/reference/collocation patterns, or other list modifications.

A generated two-picture 640×360 single-reference P stream with three SPS RPS entries selecting index 2, and a matching stream with the maximum 64 entries selecting index 63, each decoded bit-exactly against software in five fresh candidate-driver VPU sessions (`test_hevc_three_rps_decode.sh`). These results cover only the tested single-reference P patterns. Three-set inline-RPS parsing and rewriting are covered by unit tests, not hardware tests. Other SPS RPS counts, indices, and reference/list layouts remain unverified.

The checked-in 64×64 HM PCM fixtures cover actual 8×8, 16×16, and 32×32 PCM coding units and both values of `pcm_loop_filter_disabled_flag`. All four variants decoded bit-exactly against software in five fresh candidate-driver VPU sessions each (`test_hevc_pcm_decode.sh`). The accepted SPS bounds are 8–32 samples; PCM bit depths other than 8-bit 4:2:0 remain unverified or unsupported.

Sustained 4K60 throughput was measured on the current RDK-X5 candidate: 320 decoded frames per run over 20 loops, including VA-API hardware-frame download to system memory. Eight runs measured 66.25–67.65 fps (1.10–1.13× real time). Three additional 100-loop runs (1,600 frames each) measured 72.37, 73.23, and 73.26 fps. Timing uses monotonic `/proc/uptime` wall-clock time from process launch through completion, so short runs include fixed process and VPU startup overhead and are not directly comparable with longer runs. Reproduce with `tools/test_hevc_4k60_throughput.sh`; these results are specific to that board, candidate driver, and B3 workload.

The corrected off-screen H.264 VPU-to-DirectVIV GLES benchmark uses a GBM target and viewport matching the input dimensions. On 2026-10-07, a candidate-driver run rendered all 64 frames of a 1920x1080p60 H.264 High B6 sample at 92.43 fps; optional five-point framebuffer readback passed a nonblank/color-variation smoke check at 91.63 fps. A 640x360 B6 sample also rendered all 64 frames to a 640x360 target and passed the smoke check; its brief 0.16-second run is not a throughput or sustained-playback result. These checks do not prove pixel-exact software agreement or display presentation. Earlier 185.90/111.91 fps figures are withdrawn because those runs used a 64x64 GBM surface with a 1920x1080 viewport. The benchmark drains delayed decoder frames at EOF and checks the rendered count when the container reports it.

- Surface and image paths support 8-bit NV12 / YUV420 with even width and height. Generic surfaces accept 64–8192 luma samples in width and 64–4096 in height; context limits are entrypoint-specific. HEVC Main VLD contexts accept up to 8192×4096, while HEVC Main EncSlice remains capped at 3840×2160.
- H.264 VLD and EncSlice advertise Constrained Baseline, Main, and High. Constrained Baseline encoding selects the SDK Baseline profile, forces CAVLC and disables 8x8 transform, sets `constraint_set1_flag` on verified Baseline SPS NAL units, and removes only the verified zero-valued high-profile PPS extension while preserving chroma-QP semantics. Encoding is limited to tested 8-bit 4:2:0, CBR/VBR/CQP, I/P, single-full-frame-slice operation. VA VBR maps maximum bitrate, target percentage, and window to SDK AVBR target/VBV controls; VA AVBR is not advertised because equivalent convergence and accuracy semantics are unverified. Profile syntax and hardware re-decode are checked by `test_h264_encode_profiles.sh` and `test_h264_encode.sh`.
- The H.264 decoder synthesizes profile-matched SPS data for Constrained Baseline, Main, and High; it supports progressive 8-bit 4:2:0 and POC types 0 and 2. Interlaced coding tools (MBAFF and field pictures) are rejected for every profile because the X5 VPU does not support them. Monochrome and 4:2:2/4:4:4 formats are also rejected for every profile. Coded macroblock dimensions are limited to the VA context dimensions rounded up to macroblock boundaries; reference-frame counts above the VA DPB capacity of 16 are rejected. POC type 1 is rejected because VA picture parameters do not expose its offset-cycle fields. FMO and redundant-picture-count PPS modes are rejected because the synthesized PPS does not represent them. Constrained Baseline additionally rejects CABAC, weighted-prediction, and 8x8-transform picture parameters.
> HEVC list-modification scope: the zero-SPS-RPS inline path parses and validates signaled reordering against VA `RefPicList`. Checked-in 640×360 Main fixtures cover P L0 reordering `[1,0]` and B L0/L1 reordering `[1,2,0]` with three current references; both decoded all 8 pictures bit-exactly against software in 5/5 fresh candidate-driver VPU sessions (`test_hevc_inline_refmod_decode.sh`). The two-SPS-RPS inter-picture path also validates one selected-index-0 B pattern with four current references and L0/L1 reordering `[1,2,3,0]`; other selected RPS indices and list/RPS layouts are not implied.
- HEVC VLD advertises Main 8-bit 4:2:0 with context-matching dimensions up to 8192×4096; HEVC Main EncSlice remains capped at 3840×2160 and emits Level 5.1. VLD accepts ordered long-format slice sequences whose CTU addresses start at zero and increase within the picture. WPP is accepted only with tiles disabled; the Main profile forbids enabling both tools in one PPS. WPP entry-point counts are bounded by picture CTB rows. Uniform and explicit non-uniform tile grids are accepted when ordered slices begin at tile boundaries and per-slice entry-point syntax matches the tile substreams. Hardware tests cover a uniform 2×2 IDR single slice, an explicit non-uniform 2×2 P single slice, a uniform 2×2 P stream with four independent tile-aligned slices per picture, and an explicit non-uniform 2×2 B/P stream with four independent tile-aligned slices; each 640×360 HM 18.0 fixture decoded bit-exactly in five fresh candidate-driver sessions (`test_hevc_tiles_decode.sh`, `test_hevc_tiles_nonuniform_decode.sh`, `test_hevc_tiles_multislice_decode.sh`, `test_hevc_tiles_nonuniform_multislice_decode.sh`). Other tile geometries and partial-tile slice starts are unverified. Dependent slice segments are accepted only when enabled in the PPS, never as the first segment, and only when they preserve the inherited slice type; the hardware-tested dependent-segment case is limited to the encoder's 640×360 WPP IDR output. A generated 640×360 single-slice WPP stream is also hardware-tested; other WPP layouts remain unverified. Standard default scaling matrices are accepted only when the VA IQ matrix exactly matches those defaults; non-default scaling-list data, Main 10, PCM outside the verified 8-bit 4:2:0 parameter bounds, separate colour planes, long-term references, and partial slice-data fragments are rejected. PCM allows 8–32-sample SPS bounds; actual 8×8, 16×16, and 32×32 PCM coding units are hardware-verified in 8-bit 4:2:0 with both PCM loop-filter settings (`test_hevc_pcm_decode.sh`). `test_hevc_scaling_list_decode.sh` checks bit-exact default-matrix decoding and rejection of a non-default matrix. Slice-local RPS bit counts are accepted. For 2–64 declared SPS RPS sets, inter slices must pass RPS index, VA DPB reconstruction, active-reference list, and remaining slice-header checks before their RPS syntax is rewritten inline for the synthetic SPS. Hardware verification covers only the tested single-reference P patterns with three sets/index 2 and 64 sets/index 63 (two 640×360 pictures, 5/5 sessions per case); other counts, indices, and layouts remain unverified. Three-set inline-RPS parsing and rewriting are unit-tested, not hardware-tested. IDR pictures require an empty DPB; a two-slice IDR with 14 unused SPS RPS sets decoded bit-exactly in 5/5 sessions.

  The established two-set hardware patterns are narrower: P selects index 0 with the immediately previous POC as its sole reference; B selects index 0 and matches either that POC as the sole L0/L1 reference or the tested POC-4 case with four negative current references, TMVP, SAO, and list reordering `[1,2,3,0]` in both lists. Luma/chroma weighted P/B tables are range- and boundary-checked; hardware coverage includes the SAO-enabled P fixture, B with TMVP disabled, non-default weighted B-SAO variants, single-reference low-delay B-TMVP, and four-reference B. Other two-set B-SAO flag/weight combinations, weight values, and RPS/weight-table layouts remain unverified. Temporal MVP, active-reference overrides, list modifications, and every slice in single-/multi-slice pictures must match the exact validated patterns. Other unvalidated slice-header tools are rejected. Unit tests cover malformed syntax, chroma limits, header bounds, reference mapping, list modifications, and multi-reference RPS rewriting. Two-slice 640×360 HM IDR+P+P and IDR+BBB streams passed 5/5 sessions. Single-slice inline-RPS weighted P (24 frames) and B3 (32 frames) streams also matched software. Arbitrary multi-slice inter streams remain unvalidated. The established single-slice hardware suite remains bit-exact for 640×360 P (30 frames, Level 4.1), 1280×720p60 B3/B6 (64 frames each, Level 4.1), 1920×1080p60 B6 (64 frames, Level 5.0), and 3840×2160p60 B3 (16 frames, Level 5.1). An 8192×4096 Level 6.0 two-frame stream at 1 fps passed 5/5 sessions; this is repeated low-rate maximum geometry, not 8K60 throughput. Stream timestamps alone do not establish throughput: six fresh sessions decoding the 16-frame 4K B3 sample over 20 loops (320 frames) measured 65.04–67.09 fps. Five consecutive fresh sessions, each decoding 1,600 frames in a 100-loop run, measured 73.26–73.36 fps; CPU/DDR thermal zones rose about 4.3C over roughly two minutes. Both tests include hardware-frame download; this short run does not establish long-duration thermal stability or display presentation. Arbitrary HEVC tools or streams are not implied.
- Temporal-MVP scope: hardware verification is limited to the two P patterns and two exact B patterns described above. The single-reference B fixture uses the immediately previous picture in both lists, `collocated_from_l0_flag=0`, and SAO. The four-reference fixture covers only its stated POC/RPS/list pattern; neither fixture implies support for arbitrary TMVP references or collocation layouts.
- HEVC Main geometry validation enforces CTB sizes from 16 through 64 and, when tiles are enabled, minimum tile dimensions of 256 luma samples wide by 64 high before VPU submission.
- HEVC EncSlice is limited to Main 8-bit 4:2:0, CBR/VBR/CQP, I/P frames, and one full-frame slice submitted through VA per picture. VA VBR uses the same SDK AVBR target/VBV mapping described above; VA AVBR is not advertised. CQP applies effective per-picture QP values from 0 through 51; target bitrate is ignored and HRD parameters are unsupported. The picture's `entropy_coding_sync_enabled_flag` configures the SDK's static `wpp_enable` value before VPU startup; changing it requires a new VA context. A 640×360 I/P WPP stream (IDR followed by one single-reference P picture) produced five dependent row segments per picture and decoded bit-exactly in software and hardware in 5/5 fresh sessions. B-frame WPP encoding and other WPP geometries are unverified. The encoder explicitly sets the SDK HEVC SAO control to 0 and initialization fails if that API call fails. The SDK header marks this setting as encoding-supported and decoding-unsupported; that control limitation is distinct from VLD bitstream decoding. The SDK's HEVC crop setting did not change the emitted SPS, so the driver adds standard SPS conformance-window syntax for right/bottom crops while preserving coded dimensions; each crop must be smaller than 16 pixels, align to the 4:2:0 crop unit, retain the same CTU grid, and fit the coded-buffer capacity. 64-frame Level 4.1 streams were hardware re-decoded bit-exactly to software at 640×360 (right/bottom offsets 0/4), 638×360 (1/4), 638×362 (1/3), and 626×354 (7/7) in the SPS. Separate 16-frame no-B CBR samples at 3840×2160 Level 5.1 were encoded and re-decoded bit-exactly at both 30fps and 60fps on the candidate driver. These are short output-correctness checks, not sustained 4K60 throughput tests or exhaustive geometry validation.
- H.264 VLD slice parameters honor `slice_data_offset`, `slice_data_size`, and the `ALL/BEGIN/MIDDLE/END` flags. The driver accepts one parameter buffer with multiple elements paired to one data buffer, or equal-count ordered parameter/data buffer pairs with one element each; ambiguous pairings and out-of-range data are rejected. A 64-frame 640×360 High stream with two slices per picture and six consecutive B-frames decoded bit-exactly against software in 5/5 fresh VPU sessions.
- H.264 encoding supports CBR/VBR/CQP, with one full-frame I/P slice per picture (maximum slice count: 1). CQP uses `pic_init_qp + slice_qp_delta` as the effective QP, requires a value from 0 through 51, ignores target bitrate, and rejects HRD parameters. Hardware CQP checks cover Constrained Baseline, Main, and High: QP 0 was sent through direct VA calls and validated in cropped all-intra output; QP 1, 26, 37, and 51 were tested at aligned, cropped, and 30000/1001 fps sizes with every decoded frame matching software. HEVC Main supports CBR/VBR/CQP I/P encoding. Direct-VA tests cover effective QP boundaries, separate per-picture I/P QP values, and WPP; FFmpeg CQP samples at QP 1, 26, and 51 passed hardware/software decode comparison. HEVC CQP ignores target bitrate and rejects HRD parameters. H.264 and HEVC use MediaCodec external-frame input: compatible contiguous NV12 VA surfaces are submitted directly, while crop/padding or incompatible layouts are staged through per-context hbmem scratch storage. The source surface remains owned until the SDK input-consumed callback. RDK-X5's [MediaCodec documentation](https://developer.d-robotics.cc/x5_sdk_doc_v2.0.0/en/multimedia_development/10-MediaCodec_API_zh_CN.html) supports only GOP presets 1 and 9: `ip_period` must be 1 and `max_num_ref_frames` cannot exceed 1. A zero-reference sequence selects all-intra GOP preset 1 and accepts only I slices; one reference selects GOP preset 9 for the single-reference I/P pattern. H.264 and HEVC encode configurations advertise previous-frame prediction only. B/SP/SI slices, multiple/partial slices, macroblock maps, and unsupported encoder buffer or misc parameter types are rejected. VA HRD/VBV buffer size is converted to the SDK's 10–3000 ms VBV window; the installed SDK has no control for VA's initial CPB fullness, so that value is not applied. H.264 level and supported sequence VUI fields are fixed before VPU startup; changing them requires a new VA context. JPEG VLD supports baseline sequential, 8-bit, three-component YUV 4:2:0 with one full-frame scan and 0/90/180/270-degree rotation. Rotation is fixed for a VA context; 90/270-degree output surfaces use swapped dimensions. The JPU reports coded dimensions in its frame metadata, so the driver separately checks the rotated NV12 plane layout. Other sampling modes, progressive/multi-scan input, and unsupported VA fields are rejected. JPEG encoding supports baseline sequential 8-bit, three-component pictures, per-picture quality (1–100), supplied quantization tables, and restart intervals. All four decode rotations were checked against a software NV12 reference in five repeated cycles per angle. Attributes reported as `VA_ATTRIB_NOT_SUPPORTED` are not implemented.
- DMA-BUF export supports DRM PRIME 2. Import accepts only a bounds-validated, linear, single-object NV12 descriptor backed by HBmem metadata available in this process; multi-object, separate-layer, non-linear-modifier, and metadata-unresolvable DMA-BUF layouts are rejected. Decoded VPU frames with unverified multi-FD layouts are also rejected. Legacy DRM PRIME import is unsupported.
- `vaSyncSurface2` applies its monotonic-clock deadline to driver-mutex acquisition and decoder waits, including bounded output dequeue/recycle calls. A timed-out wait leaves pending decode and buffer ownership intact so the caller can retry; `VA_TIMEOUT_INFINITE` follows the existing `vaSyncSurface` path.
- `vaSyncBuffer` validates buffer IDs and includes driver-mutex acquisition in its timeout. VA input buffers are consumed synchronously by `vaRenderPicture`, and encoded output is copied into driver-owned coded-buffer storage before successful `vaEndPicture` return.
- `vaDeriveImage` succeeds only when the surface provides directly CPU-mappable NV12 backing; it does not create a staging copy. Only verified contiguous single-FD layouts are accepted, and CPU access through `vaMapBuffer`/`vaUnmapBuffer` or `vaLockSurface`/`vaUnlockSurface` performs DMA cache invalidate/flush operations. Failed flush state is retained and retried per surface, while export and surface copies are rejected during CPU mappings. The source surface remains locked while the derived image exists, so `vaGetImage`/`vaPutImage` targeting that source return `VA_STATUS_ERROR_SURFACE_BUSY`; copies with other surfaces are supported.
- `vaQuerySurfaceError` is not implemented: the Wave521 SDK reports aggregate corrupted-MB counts and an error reason, but not the start/end MB addresses required for truthful `VASurfaceDecodeMBErrors` records. The driver does not invent locations; `vaSyncSurface` still reports a failed frame with `VA_STATUS_ERROR_DECODING_ERROR`. Subpictures, display-attribute operations, image palettes, and `vaPutSurface` are also not implemented and return errors rather than false success. `vaPutImage` uses a CPU NV12 staging path; it does not provide a hardware blit.
- `vaAcquireBufferHandle`/`vaReleaseBufferHandle` support the DRM PRIME FD lifecycle only for derived NV12 images backed by a verified contiguous single-FD DMA-BUF. The acquired duplicate remains valid until release, and the parent surface is kept unavailable for conflicting operations while acquired. Generic malloc-backed buffers and unverified or multi-FD layouts are rejected. Multi-frame-context operations, processing-rate queries, and generic `vaCopy` remain unimplemented. `vaBufferSetNumElements` is not supported for `VAEncCodedBufferType`; coded-buffer capacity is fixed by `vaCreateBuffer`.
- Unit tests and a successful build do not establish playback or encoding performance on hardware. Run stream regression tests on the target board with `/dev/dri` and the Wave521 VPU available. Headless VA tests default to `/dev/dri/renderD128`; set `HOBOT_DRM_DEVICE` to override it. Display/DirectVIV tests that require KMS continue to use `/dev/dri/card0`.

### Earlier Candidate Verification (2026-10-08)

Candidate driver SHA-256 `ff7dee2bf5b64ff4a61f1ac7f652d679a0b1f43010256ee41e641818b7e7059d` passed all 37 `tools/test_*.sh` hardware regression scripts. This includes a 5/5-cycle 8192×4096 Level 6.0 HEVC Main decode, H.264/HEVC dual-context decoding in one process (64 frames per stream, 3/3 cycles), concurrent separate-process decoding (64 frames per stream), HEVC three-independent-slice IDR+P (5 VPU sessions), H.264 reference-B B-pyramid (64 frames), H.264 Baseline/Main/High and HEVC Main hardware encode/re-decode, JPEG quality-90 encode/decode, and all four JPU rotations (five cycles each, 76.14 dB PSNR). A separate PRIME2 export test validated 16 decoded 3840×2160p60 HEVC B3 frames, both NV12 layouts, plane/object bounds, and eight retried `vaSyncSurface2` timeouts. The 4K60 HEVC B3 throughput test decoded 320 frames in 20 loops at 66.53 fps in the latest run; four runs on this candidate measured 66.53–67.37 fps. The test-state suite passed under AddressSanitizer and UndefinedBehaviorSanitizer. GCC `-fanalyzer` reported two allocation-lifetime warnings for storage retained in driver-owned structs (`coded_segment.buf` and `surface.raw_data`); both were traced to explicit buffer/surface cleanup paths. No kernel warning or error entries were present in the journal during this validation window. These are targeted fixtures and short runs, not evidence for arbitrary streams or long-duration thermal/display performance. The candidate was built and tested from `/tmp`; it was not installed.

### Current Driver Verification (2026-10-08)

The installed candidate SHA-256 `e3138675c740ed95ea8891b1fe660bf007c86a0ccffff7343d85a502c2167da2` passed all 39 `tools/test_*.sh` hardware regressions using the candidate path. Coverage includes H.264/HEVC CBR, VBR, and CQP encoding, bit-exact hardware/software re-decode, dynamic HEVC VBR target/max/window updates, decode fixtures, JPEG rotations, concurrent and multicontext decode, and 4K60 throughput (320 frames at 67.65 fps). `test_va_config` and `test_va_surface_state` passed; the state test includes VBR config acceptance and VA AVBR rejection. The same SHA was atomically installed at `/usr/lib/aarch64-linux-gnu/dri/hobot_drv_video.so`; installed-path `test_va_config` and the complete VBR encode/re-decode suite passed. Artifacts are preserved under `/mnt/data/hobot-vpu-verify/vbr-20261008/`. No kernel warning-or-higher entries appeared in the post-test journal window.

The previous installed driver was preserved at `/var/backups/hobot_drv_video.so.20261008-pre-vbr` (SHA-256 `24c737351ef95e15d18a6a1513e1cb0bd36e9ce54fb1eaae6fe14e13fee4fec9`). These are bounded fixtures and short hardware runs, not support claims for arbitrary streams or long-duration thermal/display performance.

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
   `libva-hobot` checks the SDK decode result even when `error_reason` is masked, and also inspects `err_mb_in_frame_display`. Fail and undocumented results are dropped; `SUCCESS_WITH_WARNING` is reported to the fallback watchdog but can still be displayed when the corruption fields allow it:
   ```c
   if ((decode_result != HOBOT_DECODE_RESULT_SUCCESS &&
        decode_result != HOBOT_DECODE_RESULT_SUCCESS_WITH_WARNING) ||
       err_mb > 0 || (err_reason & 0x00020000)) {
       fprintf(stderr, "[HOBOT-VA][DROP] Dropping failed/corrupt frame\n");
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
- **`test_va_directviv_bench.c`**: Measures H.264 VPU decode plus full-size off-screen DirectVIV GLES rendering, drains delayed frames at EOF, and checks the output count when the container reports it. The GBM target and viewport follow the input dimensions. A 2026-10-07 candidate-driver run rendered all 64 frames of a 1920×1080p60 H.264 High B6 sample at 92.43 FPS; optional five-point framebuffer readback passed a nonblank/color-variation smoke check at 91.63 FPS. This does not prove pixel-exact software agreement, display presentation, or sustained playback. Earlier 185.90/111.91 FPS records are withdrawn because they used a 64×64 GBM surface with a 1920×1080 viewport.
- **`test_va_config.c`**: Checks the exact advertised matrix of five profiles and ten VLD/encode entrypoint pairs across H.264 Constrained Baseline/Main/High, HEVC Main, and JPEG Baseline; verifies encode capabilities, config query/creation, profile-specific surface limits, and NV12 CPU/DRM PRIME 2 export.
- **`test_va_surface_export.c`**: Decodes the entire input with VA-API, checks timed `vaSyncSurface2` including timeout retries, validates both NV12 and separate-layer DRM PRIME 2 exports with plane/object bounds checks, and checks derived-image DRM PRIME buffer-handle acquire/release for every decoded frame. `HOBOT_DRM_DEVICE` selects the DRM node.
- **`test_va_surface_import.c`**: Allocates an NV12 VA surface and checks derived-image DRM PRIME buffer-handle acquire/release, FD closure, and surface locking on both allocated and reimported surfaces. It writes a test pattern, exports and reimports through composed-layer DRM PRIME 2, verifies Y/UV bytes and physical layout, then closes the exported FD and destroys the source before repeating the read. A `vaPutImage` Y/UV patch through the surviving import also checks that untouched pixels remain intact. Load a candidate driver with `LIBVA_DRIVER_NAME=hobot LIBVA_DRIVERS_PATH=<candidate-directory>`.
- **`test_va_jpeg_encode.c`**: Hardware JPEG Baseline encode smoke test; accepts a per-picture quality value, checks zero-timeout and infinite-timeout `vaSyncBuffer`, and writes one encoded JPEG for independent decode checks.
- **`test_va_jpeg_decode.sh`**: Generates baseline 4:2:0 JPEG input, checks repeated JPEG VLD output in one VA context, and compares NV12 with a full-range software reference using PSNR. A decode attempt is bounded to 20 seconds by default (`HOBOT_JPEG_DECODE_TIMEOUT`).
- **`test_va_jpeg_rotation.c` + `test_va_jpeg_rotation.sh`**: Submits JPEG Baseline VA buffers for 0/90/180/270-degree VLD, checks the rotated output geometry and NV12 layout, and compares each result with a software-rotated reference over repeated cycles. The C test is built by `make build-va-tests`; the script accepts a candidate-driver directory.
- **`test_hevc_main_decode.sh`**: Generates HEVC Main 8-bit 4:2:0 P/B streams at 640×360, 720p60, 1080p60 Level 5.0, 4K60 Level 5.1 (including B3/B6 cases), and 8192×4096 Level 6.0 at 1 fps. It also verifies a 64-frame 640×360 Level 4.1 inline-RPS stream with eight active L0 references in five fresh sessions by default (`HOBOT_HEVC_REF8_CYCLES` adjusts the count). The 8K sample runs five fresh VPU sessions by default (`HOBOT_HEVC_8K_CYCLES` adjusts the count); set `HOBOT_HEVC_8K_ONLY=1` to run only that case. This is not an 8K60 throughput test. The suite also generates no-B weighted-P and B3 weighted-bipred streams with verified non-default luma offsets in P/B slice tables. It verifies the SPS DPB/RPS/active-reference counts, encoded levels and frame patterns, then compares every hardware-decoded NV12 frame against software output. Set `HOBOT_HEVC_REF8_ONLY=1` to run only the eight-reference regression, `HOBOT_HEVC_WEIGHTED_ONLY=1` to run both weighted regressions, or `HOBOT_HEVC_WEIGHTED_P_ONLY=1` for only the weighted-P case.
- **`test_hevc_4k60_throughput.sh`**: Replays a verified 3840×2160p60 HEVC Main stream in one VA-API process, downloads every decoded frame, and checks sustained output against a configurable minimum FPS.
- **`test_vpu_concurrent_decode.sh`**: Runs independent H.264 and HEVC VA-API decoders concurrently in separate processes, then compares every hardware NV12 frame against software output. Requires at least 8-bit `yuv420p` input and retains logs/hashes under a temporary directory.
- **`test_vpu_multicontext_decode.sh`**: Muxes H.264 and HEVC inputs and decodes both VA contexts in one FFmpeg process, then compares each hardware NV12 stream against software output. `HOBOT_MULTICONTEXT_DECODE_CYCLES` repeats the test; each cycle has a bounded timeout.
- **`test_hevc_multislice_b_decode.sh`**: Checks a 640×360 HM fixture with two slices per picture and an IDR+BBB low-delay B pattern. Both reference lists use the same immediately previous POC as their sole reference. It compares candidate-driver NV12 output with software across five fresh VPU sessions by default.
- **`test_hevc_three_slice_decode.sh`**: Checks a four-picture 640×360 HM 16.20 Main fixture with three independent slices at CTU addresses 0, 20, and 40 per picture. `trace_headers` verifies the topology and disabled WPP; candidate-driver NV12 output matches software bit-exactly across five fresh VPU sessions. This does not imply support for arbitrary multislice streams.
- **`test_hevc_wpp_encode.sh`**: Encodes a 640×360 HEVC Main WPP IDR/P pair through the direct VAAPI test client, verifies I/P order and dependent row-segment syntax for both pictures with `trace_headers`, and compares software and candidate-driver hardware decode hashes. B-frame WPP and other geometries are unverified.
- **`test_hevc_wpp_decode.sh`**: Generates a 32-frame 640×360 HEVC Main stream with WPP enabled and tiles disabled, checks the parameter-set and entry-point syntax, then compares candidate-driver NV12 output with software across five fresh VPU sessions. Other WPP layouts are unverified.
- **HEVC tile decode (`test_hevc_tiles_decode.sh`, `test_hevc_tiles_nonuniform_decode.sh`, `test_hevc_tiles_multislice_decode.sh`, `test_hevc_tiles_nonuniform_multislice_decode.sh`)**: Covers uniform single-slice IDR, explicit non-uniform single-slice P, uniform four-slice-per-picture P, and explicit non-uniform four-slice-per-picture B/P fixtures. Each is compared bit-exactly with software over five fresh VPU sessions. Other tile geometries and WPP-plus-tiles layouts remain unverified.
- **`test_hevc_tiles_nonuniform_decode.sh`**: Uses an HM 18.0 640×360, 8-frame P stream with explicit non-uniform 2×2 tiles (6/4 CTB columns, 4/2 CTB rows). It validates PPS geometry and all three entry points, then compares candidate-driver NV12 output bit-exactly with software over five fresh VPU sessions. Other layouts are unverified.
- **`test_hevc_two_rps_weighted_decode.sh`**: Adds syntax-valid neutral, non-default luma, and chroma weighted P/B tables to checked-in two-SPS-RPS HM fixtures. It verifies table fields, confirms neutral tables preserve software output and non-default tables change it, then compares candidate-driver output against software (3 P-pattern frames and 4 B-pattern frames per profile) for five cycles by default. Set `HOBOT_HEVC_TWO_RPS_WEIGHTED_CYCLES` to change the repetition count. Other RPS/weight-table layouts remain unverified.
- **`test_hevc_two_rps_tmvp_decode.sh`**: Generates equivalent two-SPS-RPS P streams with temporal MVP, one selecting SPS RPS index 0 and one carrying the RPS inline. It checks syntax and software equivalence, then compares candidate-driver NV12 output over five fresh VPU sessions per variant by default. A separate, narrowly scoped B-slice TMVP case is covered by `test_hevc_two_rps_b_tmvp_decode.sh`.
- **`test_hevc_two_rps_b_tmvp_decode.sh`**: Checks one HM 16.20 IDR+four-low-delay-B 640×360 fixture with two SPS RPS entries, selected index 0, one immediately previous reference in each list, SAO, and temporal MVP. It verifies the syntax and compares every hardware NV12 frame with software over five fresh VPU sessions by default. Other B-slice TMVP reference/collocation patterns are not implied.
- **`test_hevc_three_rps_decode.sh`**: Generates two 640×360 two-picture single-reference P streams, one with three SPS RPS entries selecting index 2 and one with 64 entries selecting index 63. It checks syntax and software equivalence, then compares candidate-driver output bit-exactly over five fresh VPU sessions per case by default. Only these patterns are hardware-verified.
- **`test_hevc_pcm_decode.sh`**: Uses checked-in 64×64 HM fixtures containing actual 8×8, 16×16, and 32×32 PCM coding units and both `pcm_loop_filter_disabled_flag` values. It validates the PCM SPS fields and compares candidate-driver output with software over five fresh VPU sessions per variant. Other PCM bit depths are not covered.
- **`test_hevc_main_encode.sh`**: Encodes a 64-frame HEVC Main no-B stream with VAAPI (CBR default; VBR and CQP supported), checks profile/level/visible and coded dimensions/frame count, verifies SAO is disabled and checks SPS crop offsets, then compares every hardware-decoded frame with software. The tested sizes are 640×360, 638×360, 638×362, and 626×354; separate 16-frame 3840×2160 Level 5.1 streams were encoded at 30fps and 60fps and re-decoded bit-exactly. These are short output-correctness checks; sustained 4K60 throughput and other geometry combinations are unverified.
- **`test_vbr_encode.sh`**: Exercises H.264 and HEVC VA VBR, including per-frame HEVC target/max/window updates. It validates stream metadata and compares hardware re-decode with software; it does not claim target-bitrate accuracy across arbitrary content.
- **`test_hevc_cqp_encode.sh`**: Uses direct VA calls to test HEVC Main CQP effective-QP boundaries, per-picture I/P QPs and QP deltas, WPP output, bitstream QP fields, invalid-QP rejection, and software/hardware decode equivalence. Cases cover effective QPs 0, 37, and 51 plus WPP I=26/P=37; they do not imply arbitrary-stream or sustained-throughput support.
- **`test_h264_encode.sh`**: Repeats aligned 720p60, visible 640x360 in a 640x368 coded frame, and 30000/1001 fps VAAPI H.264 encoding. It selects Constrained Baseline, Main, or High, supports CBR/VBR/CQP, checks SPS level/timing/cropping and limited-range signaling, then compares every hardware-decoded NV12 frame with software decode. The mocked surface-state test also checks nonzero left/top crop placement and Y/UV edge replication.
- **`test_h264_crop_encode.sh`**: Directly submits a 638x360 VA surface to 640x368 H.264 contexts for Constrained Baseline, Main, and High, with nonzero SPS left/top offsets and zero references. It verifies the crop/reference syntax and three consecutive I-picture outputs in each VPU context, decodes all visible frames, and checks them against the source NV12 frame with a PSNR threshold. `HOBOT_H264_CROP_ENCODE_QP` enables direct VA CQP testing, including QP 0.
- **`test_va_surface_state.c`**: Mocked regression checks for decoder `decode_result` handling (fail, unknown, warning, success), buffer ownership/teardown, PRIME2 import validation and rollback, external-encoder consumed-callback single-flight ownership, `vaQuerySurfaceStatus` during decode, active encode, and external-input consumption, PID-scoped watchdog publication across threads/processes and sequence restart, requested coded-buffer capacity, API array bounds, encoder-control and JPEG quality/quantization/restart/Huffman validation, H.264 zero-reference I-only enforcement, GOP preset selection, reference-pattern rejection and prediction-direction reporting, HRD-to-VBV conversion and single-slice limits, H.264 context-dimension/DPB bounds and decoder slice-parameter/fragment validation, HEVC Main subset, 2–64-set SPS-RPS index validation and DPB-derived inline rewriting, inline RPS parsing, single-SPS-RPS DPB reconstruction with P/B list ordering and SAO validation, ordered HEVC slice-sequence validation including a three-slice mock case, submission-FIFO wraparound, synchronization concurrency and timeout deadlines under contended mutex acquisition, surface-lock lifetime, PRIME export flags, NV12 plane/object bounds, and distinct luma/chroma pitch handling. It does not replace hardware playback tests; hardware fixtures cover only the exact patterns documented above.

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
| **JPEG JPU decode path** | ✅ Limited | Baseline sequential 8-bit YUV 4:2:0 decode with context-fixed 0/90/180/270-degree rotation; other sampling modes remain unavailable |
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
