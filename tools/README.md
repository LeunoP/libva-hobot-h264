# Verification tools

The tools in this directory cover VA capability/state checks and hardware regressions. Hardware tests must run on an RDK-X5 with the vendor VPU stack available. A passing fixture verifies that case only; it does not imply support for arbitrary bitstreams.

## Build and run

```bash
make build-va-tests
make test-va-state
LIBVA_DRIVER_NAME=hobot /tmp/libva-hobot-tests/test_va_config
```

To load a candidate driver from the repository instead of the installed copy:

```bash
make -j"$(nproc)"
LIBVA_DRIVER_NAME=hobot LIBVA_DRIVERS_PATH="$PWD" <test-command>
```

Read each script's usage and environment-variable section before running it. Tests that take media inputs or output paths require those arguments. Scripts may save logs and encoded streams under a temporary or caller-selected directory.

## Test index

| Area | Tests |
|---|---|
| VA capabilities and mock state | `test_va_config.c`, `test_va_surface_state.c` |
| Surface export/import and sync | `test_va_surface_export.c`, `test_va_surface_import.c` |
| H.264 decode | `test_h264_bframe_decode.sh`, `test_h264_b_pyramid_decode.sh`, `test_h264_profile_decode.sh`, `test_h264_multislice_decode.sh` |
| HEVC decode | `test_hevc_main_decode.sh`; single-/two-/three-RPS and inline-RPS tests; multislice, SAO, TMVP, weighted prediction, open-GOP, PCM, tiles, and WPP tests (`test_hevc_*_decode.sh`) |
| H.264 encode | `test_h264_encode.sh`, `test_h264_encode_profiles.sh`, `test_h264_crop_encode.sh` |
| HEVC encode | `test_hevc_main_encode.sh`, `test_hevc_cqp_encode.sh`, `test_hevc_wpp_encode.sh`, `test_vbr_encode.sh` |
| JPEG | `test_va_jpeg_decode.sh`, `test_va_jpeg_rotation.sh`, `test_va_jpeg_encode.c` |
| Concurrency and performance | `test_vpu_concurrent_decode.sh`, `test_vpu_multicontext_decode.sh`, `test_hevc_4k60_throughput.sh`, `test_va_directviv_bench.c` |
| DirectVIV/DRM smoke tests | `test_directviv.c`, `test_render_directviv.c`, `test_nv12_overlay.c` |

HEVC bitstream fixtures are in [`testdata/`](testdata/); their test mapping is listed in [`testdata/README.md`](testdata/README.md). Decode regressions compare hardware output with a software reference where the test supports that comparison. Throughput and rendering benchmarks measure only their stated workload and setup.
