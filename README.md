# libva-hobot-h264 — VA-API Driver for D-Robotics RDK-X5

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-D--Robotics%20RDK--X5-green.svg)]()
[![Architecture](https://img.shields.io/badge/Arch-ARM64%20(aarch64)-orange.svg)]()
[![Codec](https://img.shields.io/badge/Codec-H.264%20%7C%20JPEG-red.svg)]()

[English](README.md) | [한국어](README.ko.md)

`libva-hobot-h264` is an open-source, high-performance VA-API (Video Acceleration API) backend driver for the **D-Robotics RDK-X5** single-board computer (powered by Chips&Media Wave521 VPU and ARM Cortex-A55).

It delivers hardware-accelerated **1080p 60fps H.264 decoding and encoding** with rock-solid frame pacing, 0 dropped frames, and low CPU utilization, making it ideal for media playback (mpv, Kodi) and cloud game streaming (Moonlight).

> [!NOTE]
> **Notice**: This codebase and driver architecture were researched, developed, and optimized by AI (Google DeepMind Antigravity / Gemini) in collaboration with LeunoP.
> *(해당 코드는 AI가 작성 및 최적화했습니다.)*

---

## Key Features

- **Standard VA-API Implementation**: Full compatibility with `libva` 1.14+ / 2.x API.
- **Flawless H.264 B-Frame Pacing**: Solves the Wave521 VPU B-frame jitter issue by disabling internal VPU reordering (`reorder_enable = 0`) and managing presentation order via a dedicated 128-entry FIFO queue (`submitted_surfaces`). Verified **0 dropped frames** on 1080p 60fps high-bitrate video.
- **Multi-Slice Frame Assembly**: Aggregates multi-slice pictures (e.g. from Moonlight / Sunshine game streaming or broadcast encoders) into single VPU frames per the `MC_FEEDING_MODE_FRAME_SIZE` specification.
- **Zero-Copy DRM PRIME 2 Export**: Implements `vaExportSurfaceHandle` with duplicate file descriptor management (`dup(fd)`) to guarantee correct DMA-BUF lifecycle across external renderers without closing underlying driver buffers.
- **Row-by-Row Pitch Alignment**: Handles VPU 8-line vertical padding (`vstride = (height + 7) & ~7`) and horizontal stride discrepancies cleanly during surface copies and export.

---

## Codec Support Matrix

| Codec | Profile | Decode (VLD) | Encode (EncSlice / EncPicture) |
|---|---|:---:|:---:|
| **H.264 (AVC)** | Constrained Baseline | ✅ | ✅ |
| **H.264 (AVC)** | Main | ✅ | ✅ |
| **H.264 (AVC)** | High (up to Level 5.1) | ✅ | ✅ |
| **JPEG** | Baseline | ✅ | ✅ |
| **HEVC (H.265)** | Main / Main 10 | ❌ Disabled | ❌ Disabled |

> **Note on HEVC**: Hardware HEVC decoding on the RDK-X5 VPU firmware currently produces severe rainbow color artifacts. It is intentionally excluded from supported VA-API profiles so that players (mpv, ffmpeg) cleanly fall back to software decoding.

---

## Hardware & System Requirements

- **Target Board**: D-Robotics RDK-X5 (Cortex-A55 Quad-Core, 4GB/8GB RAM)
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
sudo make install
# Installs hobot_drv_video.so to /usr/lib/aarch64-linux-gnu/dri/
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

Expected output:
```text
libva info: VA-API version 1.14.0
libva info: User environment variable requested driver 'hobot'
libva info: Trying to open /usr/lib/aarch64-linux-gnu/dri/hobot_drv_video.so
libva info: Found init function __vaDriverInit_1_0
libva info: va_openDriver() returns 0
vainfo: VA-API version: 1.14 (libva 2.12.0)
vainfo: Driver version: D-Robotics RDK-X5 VPU VA-API Driver 0.4.0 (Decode + Encode)
vainfo: Supported profile and entrypoints
      VAProfileH264ConstrainedBaseline:	VAEntrypointVLD
      VAProfileH264ConstrainedBaseline:	VAEntrypointEncSlice
      VAProfileH264Main               :	VAEntrypointVLD
      VAProfileH264Main               :	VAEntrypointEncSlice
      VAProfileH264High               :	VAEntrypointVLD
      VAProfileH264High               :	VAEntrypointEncSlice
      VAProfileJPEGBaseline           :	VAEntrypointVLD
      VAProfileJPEGBaseline           :	VAEntrypointEncPicture
```

---

## Recommended MPV Configuration

On RDK-X5 under X11, optimal playback performance is achieved using `hwdec=vaapi-copy` with `vo=x11`.

### Configuration: `~/.config/mpv/mpv.conf`
```ini
# Hardware Decoding via libva-hobot
hwdec=vaapi-copy
vo=x11

# Software Scaler Optimization
sws-allow-zimg=no
sws-scaler=fast-bilinear
sws-fast=yes

# Audio Buffer (Prevents PulseAudio resampling jitter from dropping video frames)
audio-buffer=1

# Fullscreen (1:1 1080p pixel mapping avoids CPU scaling overhead)
fs=yes
```

### Vulkan Loader Wrapper: `/usr/local/bin/mpv`
If Vivante's GPU library exports an incomplete Vulkan driver, preload the standard Vulkan loader:
```bash
#!/bin/bash
export LIBVA_DRIVER_NAME=hobot
export DISPLAY="${DISPLAY:-:0}"
export LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libvulkan.so.1${LD_PRELOAD:+:$LD_PRELOAD}

exec /usr/bin/mpv "$@"
```
Make it executable:
```bash
sudo chmod +x /usr/local/bin/mpv
```

---

## Diagnostics & Monitoring

Monitor VPU hardware interrupt activity during playback:
```bash
watch -n 1 "cat /proc/interrupts | grep 3b000000.vpu"
```

Monitor board temperatures:
```bash
cat /sys/class/thermal/thermal_zone*/temp | awk '{printf "%.1f°C\n", $1/1000}'
```

---

## License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.
