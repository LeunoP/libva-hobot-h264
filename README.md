# libva-hobot-h264

VA-API driver for the D-Robotics RDK-X5 Wave521 VPU.

[English](README.md) | [한국어](README.ko.md)

This driver targets the RDK-X5 Linux multimedia stack and its vendor libraries. It implements a tested subset of VA-API; it is not a general-purpose VA-API driver.

## Hardware support

| Codec | Hardware decode | Hardware encode | Scope |
|---|---|---|---|
| H.264 | Constrained Baseline, Main, High (tested through Level 5.1) | Constrained Baseline, Main, High | Tested 8-bit 4:2:0 patterns; encode is I/P, one full-frame slice |
| HEVC | Main, 8-bit 4:2:0 | Main, 8-bit 4:2:0 | Restricted to validated bitstream patterns; encode is I/P, one full-frame slice |
| JPEG | Baseline, 8-bit 4:2:0 | Baseline, 8-bit 4:2:0 | Limited VA feature set |
| H.264 Level 5.2 or unsupported DPB patterns | **Not supported** | Not applicable | The MPV helper may switch to software decoding when configured; this is not hardware support. |
| HEVC Main 10 and non-NV12 formats | **Not supported** | **Not supported** | No software fallback is provided by this driver. |

**Software fallback does not count as codec support.** The driver reports hardware capabilities only. `misc/mpv_scripts/fallback-restart.lua` can request MPV software decoding after an anomaly or decode error, but only when the helper and a software decoder are available and configured.

## Features and limits

- H.264 and HEVC EncSlice support CBR, VA VBR, and CQP for the tested I/P path. VA VBR maps to the SDK's AVBR target and VBV controls plus a separate maximum bitrate. VA AVBR is not advertised because equivalent behavior has not been established.
- Decode surfaces use even-sized 8-bit NV12. Surface limits depend on the profile and entrypoint; HEVC Main VLD is tested up to 8192×4096 and HEVC Main EncSlice up to 3840×2160.
- HEVC VLD accepts only validated syntax and reference patterns. Main 10, long-term references, and arbitrary combinations of advanced tools are unsupported or unverified.
- DRM PRIME 2 export is supported for validated layouts. Import is limited to linear, single-object NV12 buffers with resolvable HBmem metadata.
- The driver detects corrupt VPU output and publishes per-process events at `/dev/shm/hobot_va_watchdog.<pid>`. This telemetry is not itself a software decoder.
- Actual throughput and displayed frame rate depend on the stream, firmware, renderer, and board configuration. See the hardware tests in [`tools/README.md`](tools/README.md).

## Build and install

Build on the RDK-X5 with the D-Robotics multimedia SDK installed:

```bash
sudo apt-get install build-essential libva-dev libdrm-dev vainfo
make -j"$(nproc)"
sudo make install
LIBVA_DRIVER_NAME=hobot vainfo
```

The driver links against the vendor libraries under `/usr/hobot/lib`. `make install` installs the driver and `include/va/va_hobot.h`.

To test a build without installing it system-wide:

```bash
make -j"$(nproc)"
LIBVA_DRIVER_NAME=hobot LIBVA_DRIVERS_PATH="$PWD" vainfo
```

## Tests and MPV helpers

Build and run the API/state tests on the target board:

```bash
make build-va-tests
make test-va-state
LIBVA_DRIVER_NAME=hobot /tmp/libva-hobot-tests/test_va_config
```

Hardware regression scripts and their requirements are listed in [`tools/README.md`](tools/README.md). MPV DRM/GBM, X11, HLS, and fallback examples are documented in [`misc/mpv_scripts/README.md`](misc/mpv_scripts/README.md). The MPV fallback is a separate recovery path, not an expansion of the hardware support matrix.

## License

MIT. See [LICENSE](LICENSE).
