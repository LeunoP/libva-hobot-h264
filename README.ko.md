# libva-hobot-h264 — RDK X5 VPU VA-API 드라이버

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-D--Robotics%20RDK--X5-green.svg)]()
[![Architecture](https://img.shields.io/badge/Arch-ARM64%20(aarch64)-orange.svg)]()
[![Codec](https://img.shields.io/badge/Codec-H.264%20%7C%20JPEG-red.svg)]()
[![Branch: watchdog-fallback](https://img.shields.io/badge/Branch-watchdog--fallback-brightgreen.svg)]()

[English](README.md) | [한국어](README.ko.md)

`libva-hobot-h264`는 **D-Robotics RDK-X5** (Chips&Media Wave521 VPU + ARM Cortex-A55) 싱글보드 컴퓨터를 위한 오픈소스 고성능 VA-API(Video Acceleration API) 백엔드 드라이버입니다.

표준 리눅스 미디어 소프트웨어(mpv, ffmpeg, Chromium 등)에서 **1080p 60fps H.264 하드웨어 디코딩 및 인코딩**을 지원하며, 0 드롭 프레임 및 5% 미만의 낮은 CPU 점유율을 제공합니다.

> [!NOTE]
> **알림**: 본 코드베이스 및 드라이버 아키텍처는 Google DeepMind Antigravity / Gemini AI가 LeunoP와의 협업을 통해 연구, 개발 및 최적화했습니다.

---

## 주요 기능 및 아키텍처 특징

- **표준 VA-API 완벽 준수**: `libva` 1.14+ / 2.x 표준 인터페이스 호환.
- **H.264 B-프레임 페이싱 지터 완전 해결**: VPU 내부 재정렬 비활성화(`reorder_enable = 0`) 및 전용 128엔트리 FIFO 큐(`submitted_surfaces`)를 통한 프레임 순서 제어로 1080p60 고비트레이트 영상에서 **0 드롭 프레임** 달성.
- **VPU 출력 무결성 판정 및 손상 프레임 DROP**: 벤더 라이브러리의 에러 마스킹을 우회하여 드라이버 경계에서 `err_mb_in_frame_display`를 직접 검사. 손상 프레임(`err_mb > 0`)을 화면에 전달하지 않고 VPU 버퍼 풀로 자동 반환하여 화면 찢어짐(Tearing) 원천 차단.
- **Hobot-VA WatchDog 실시간 IPC 텔레메트리**: VPU 이상 상태를 감지하여 `/dev/shm/hobot_va_watchdog`로 실시간 전송. 프로세스 PID 및 Epoch 타임스탬프를 검증하여 세션 간 오탐 없는 고속 플레이어 폴백 지원.
- **멀티 슬라이스 프레임 결합**: 방송용 인코더 및 스트리밍 서버의 다중 슬라이스 패킷을 VPU `MC_FEEDING_MODE_FRAME_SIZE` 규격에 맞춰 1개 프레임으로 자동 결합.
- **DMA-BUF 사전 할당 및 DRM PRIME 2 Export**: 서피스 생성(`vaCreateSurfaces2`) 시 Hobot 메모리 관리자(`hb_mem_alloc_graph_buf`)를 통해 연속 물리 메모리를 사전 할당, 클라이언트가 `dup(fd)` 수명주기 안전성을 갖춘 유효한 DMA-BUF 핸들을 즉시 내보낼 수 있도록 지원.
- **2단계 MPV 자동화 보조 스크립트 모음 (`misc/mpv_scripts/`)**: 최초 5초 무음 프로빙 검증 및 재생 중 인플레이스 SW 전환을 위한 프로덕션 스크립트 내장.

---

## 지원 코덱 매트릭스

| 코덱 | 프로파일 | 디코더 (VLD) | 인코더 (EncSlice / EncPicture) |
|---|---|:---:|:---:|
| **H.264 (AVC)** | Constrained Baseline | ✅ | ✅ |
| **H.264 (AVC)** | Main | ✅ | ✅ |
| **H.264 (AVC)** | High (최대 Level 5.1) | ✅ | ✅ |
| **H.264 (AVC)** | High (Level 5.2 / DPB 16 초과 스트림) | 🛡️ 자동 DROP → SW 폴백 | ❌ |
| **JPEG** | Baseline | ✅ | ✅ |
| **HEVC (H.265)** | Main / Main 10 | ❌ 비활성화 | ❌ 비활성화 |

> **Level 5.2 / 고참조(High DPB) 스트림 처리**: 유튜브 1080p60 등 참조 프레임 수가 16개에 달하는 고레벨 비트스트림은 VPU의 하드웨어 DPB 한계를 초과하여 매크로블록 손상(`err_mb > 0`)이 발생합니다. 본 드라이버는 이를 안전하게 폐기하고 동반 스크립트를 통해 CPU 소프트웨어 디코딩으로 매끄럽게 전환합니다.

> **HEVC 비활성화 사유**: 현재 RDK-X5 VPU 펌웨어의 HEVC 디코딩 출력에서 무지개색 노이즈가 발생하므로, 플레이어가 깨끗한 CPU 소프트웨어 디코딩을 선택하도록 VA-API 프로파일에서 의도적으로 배제했습니다.

---

## 하드웨어 및 시스템 요구사항

- **대상 보드**: D-Robotics RDK-X5 (Cortex-A55 Quad-Core, 4GB/8GB RAM)
- **VPU**: Chips&Media Wave521
- **운영체제**: Ubuntu 22.04 LTS (Jammy) / Debian Linux 6.1 (aarch64)
- **드라이버 식별자**: `hobot` (`LIBVA_DRIVER_NAME=hobot`)
- **설치 경로**: `/usr/lib/aarch64-linux-gnu/dri/hobot_drv_video.so`
- **필수 라이브러리**: `libva-dev`, `libdrm-dev`, D-Robotics 멀티미디어 라이브러리 (`/usr/hobot/lib/libmultimedia.so`)

---

## 빌드 및 설치

### 1. 사전 패키지 설치
```bash
sudo apt-get update
sudo apt-get install -y build-essential libva-dev libdrm-dev vainfo
```

### 2. 드라이버 컴파일
```bash
cd libva-hobot
make clean
make -j$(nproc)
```

### 3. 설치
```bash
sudo cp -f hobot_drv_video.so /usr/lib/aarch64-linux-gnu/dri/hobot_drv_video.so
```

### 4. 환경 변수 등록
`libva`가 기본 드라이버로 `hobot`을 호출하도록 설정합니다:
```bash
echo "export LIBVA_DRIVER_NAME=hobot" >> ~/.bashrc
sudo sh -c 'echo "LIBVA_DRIVER_NAME=hobot" >> /etc/environment'
source ~/.bashrc
```

### 5. 설치 확인
```bash
vainfo
```

---

## 하드웨어 제로카피 파이프라인 (DRM/GBM + Vivante DirectVIV)

RDK-X5에서 하드웨어 성능을 100% 활용하는 최적의 제로카피 비디오 파이프라인:

```text
H.264 비트스트림
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
       ▼ (물리 메모리 연속 NV12 프레임 버퍼)
 Hobot Graphics Buffer
       │
       ▼ (vaExportSurfaceHandle 기반 물리/가상 주소 추출)
 Vivante DirectVIV (`glTexDirectVIVMap`)
       │
       ▼ (GPU 실리콘 하드웨어 텍스처 샘플링)
 Vivante GC8000L GLES
       │
       ▼
    DRM / GBM
       │
       ▼
   HDMI 출력 (1080p 60fps)
```

### X11 vs DRM/GBM 환경 비교
- **DRM/GBM (하드웨어 DirectVIV 직결) — 권장**: Vivante GC8000L GPU가 VPU의 NV12 물리 메모리를 `glTexDirectVIVMap`으로 직접 매핑하여 **CPU 복사 0회, 1080p60 프레임 드롭 0개, 시스템 전체 CPU 점유율 5% 미만(단일 코어 ~35%)**을 달성합니다.
- **X11 데스크톱 환경의 한계**: Vivante X11 DRI2 드라이버의 인증 실패로 인해 Mesa 소프트웨어 래스터라이저(`llvmpipe`)로 폴백됩니다. 따라서 X11 환경에서는 부득이하게 소프트웨어 복사 모드(`hwdec=vaapi-copy` + `vo=x11`)를 사용해야 하며, 이 경우 CPU 점유율이 350% 이상으로 치솟습니다.

---

## VPU 출력 무결성 판정 및 WatchDog 서브시스템

### 문제 원인: 벤더 스택의 에러 마스킹
DPB 사양을 초과하는 H.264 스트림(Level 5.2, DPB > 16) 디코딩 시, 벤더 라이브러리(`libmultimedia.so`)는 내부 심각한 디코딩 장애에도 불구하고 `error_reason = 0x00000000` (성공)으로 마스킹하여 반환합니다:
```text
warn_info = 0x01200000 (Level / DPB 한계 초과)
err_mb = 7800 ~ 8160 / 8160 (95% ~ 100% 매크로블록 손상)
```
이 서피스를 그대로 DirectVIV 디스플레이로 보내면 격렬한 화면 깨짐 및 찢어짐 현상이 발생합니다.

### 해결책: 출력 무결성 기반 프레임 DROP 및 실시간 텔레메트리
1. **손상 프레임 DROP**:
   `libva-hobot`은 드라이버 경계에서 `out_info.video_frame_info.err_mb_in_frame_display`를 검사합니다. 손상 매크로블록이 발견되면(`err_mb > 0`) 해당 버퍼를 디스플레이로 보내지 않고 VPU 버퍼 풀로 즉각 반환합니다:
   ```c
   if (err_mb > 0 || (err_reason & 0x00020000)) {
       fprintf(stderr, "[HOBOT-VA][DROP] Dropping corrupted frame (err_mb=%d/%d)\n", err_mb, total_mb);
       hb_mm_mc_queue_output_buffer(mctx, &out_buf, 50);
       continue;
   }
   ```
2. **WatchDog IPC 텔레메트리**:
   드라이버는 이상 발생 시 `/dev/shm/hobot_va_watchdog` 파일로 실시간 상태를 브로드캐스트합니다:
   ```text
   <pid> <누적_어노말리_수> <epoch_타임스탬프> <err_mb> <total_mb>
   ```
   10초간 정상 재생이 지속되면 어노말리 카운터가 자동으로 0으로 초기화되며, 컨텍스트 생성/종료 시 IPC 파일이 자동으로 정리되어 오탐을 방지합니다.

---

## MPV 연동 보조 스크립트 (`misc/mpv_scripts/`)

[`misc/mpv_scripts/`](misc/mpv_scripts/) 디렉토리는 RDK-X5의 안정적인 비디오 재생을 위한 통합 스크립트 모음을 제공합니다.

### 1. `fallback-restart.lua` (자동 검증 및 2단계 SW 전환 스크립트)

| 재생 단계 | 조건 | 플레이어 동작 | OSD 화면 알림 |
| **최초 검증 단계 (첫 5초)** | 어노말리 3회 누적 또는 HW 디코드 실패 | **`00:00:00`으로 되감기**, SW 디코더(`hwdec=no`) 전환, 화면 송출 및 음소거 해제 | **좌측 상단 작게 표시 (1.5초)**:<br>`SW 디코더` |
| **최초 검증 단계 (첫 5초)** | 어노말리 0건 (정상 통과) | **`00:00:00`으로 되감기**, 화면 송출 및 음소거 해제, 60fps HW DirectVIV 유지 | *(없음 — 매끄럽게 재생 시작)* |
| **이후 재생 중 (정상 재생 진입 후)** | 재생 중 어노말리 3회 누적 또는 디코드 오류 | **되감지 않고 현재 위치 유지**, 즉시 인플레이스 SW 전환 | **좌측 상단 작게 표시 (1.5초)**:<br>`SW 디코더` |

### 2. `mpv.conf` (RDK-X5 최적화 설정)
DRM/GBM 직결 DirectVIV Zero-Copy(`vo=gpu`, `gpu-context=drm`, `hwdec=vaapi`, `vd-lavc-software-fallback=1`), 유튜브 1080p H.264 우선 프로파일, HLS 실시간 스트림 자동 CPU 디코딩 프로파일을 기본 탑재하고 있습니다.

### 3. `mpv-launcher-wrapper.sh` (프로덕션 런처)
필수 환경변수(`LIBVA_DRIVER_NAME=hobot`, `LD_LIBRARY_PATH=/usr/hobot/lib`)를 자동 주입하며, LightDM / Xorg 간 DRM Master 권한 충돌을 자동으로 중재 및 복원합니다.

### 빠른 설치 방법
```bash
# 1. Lua 스크립트 설치
mkdir -p ~/.config/mpv/scripts
cp misc/mpv_scripts/fallback-restart.lua ~/.config/mpv/scripts/

# 2. MPV 설정 파일 설치
cp misc/mpv_scripts/mpv.conf ~/.config/mpv/mpv.conf

# 3. 런처 스크립트 등록
sudo cp misc/mpv_scripts/mpv-launcher-wrapper.sh /usr/local/bin/mpv
sudo chmod +x /usr/local/bin/mpv
```

---

## DirectVIV 서피스 확장 인터페이스 (`va/va_hobot.h`)

표준 `vaExportSurfaceHandle` 디스패치와 벤더 메모리 타입 `VA_SURFACE_ATTRIB_MEM_TYPE_HOBOT_GRAPH_BUF` (`0x80484F10`)를 통해 VPU 물리/가상 주소를 질의할 수 있습니다:

```c
#include <va/va.h>
#include <va/va_hobot.h>

struct hobot_surface_info info = {0};
VAStatus status = vaGetHobotSurfaceInfo(va_dpy, surface_id, &info);
if (status == VA_STATUS_SUCCESS) {
    // info.phys_addr[0]: Y 평면 물리 주소
    // info.phys_addr[1]: UV 평면 물리 주소
    // info.virt_addr[0]: Y 평면 가상 주소
    // info.virt_addr[1]: UV 평면 가상 주소
    // info.stride, info.vstride, info.width, info.height, info.dma_fd
}
```

---

## 알려진 문제 및 해결 현황

| 문제 현상 | 상태 | 해결 내용 |
|---|:---:|---|
| **H.264 Level 5.2 / DPB 초과 스트림 (유튜브 등)** | ✅ 완전 해결 | VPU 출력 무결성 DROP + WatchDog 자동 SW 폴백 |
| **H.264 60fps B-프레임 페이싱/끊김** | ✅ 완전 해결 | VPU Reorder 해제 (`reorder_enable=0`) + FIFO 서피스 매핑 |
| **오디오 언더런 연계 화면 튐** | ✅ 완전 해결 | `mpv.conf` 내 `audio-buffer=1` 최적화 |
| **HEVC (H.265) 무지개색 노이즈** | ⚠️ 우회 완료 | VA-API 프로파일에서 제외, CPU 소프트웨어 디코딩으로 처리 |
| **X11 데스크톱 DRI2 인증 오류** | ⚠️ 우회 완료 | DRM/GBM 직결 모드 사용; X11 환경에서는 `hwdec=vaapi-copy` 사용 |

---

## 진단 및 모니터링

재생 중 VPU 하드웨어 인터럽트 활동 확인:
```bash
watch -n 1 "cat /proc/interrupts | grep 3b000000.vpu"
```

WatchDog 실시간 텔레메트리 확인:
```bash
cat /dev/shm/hobot_va_watchdog
```

보드 발열 및 온도 모니터링:
```bash
cat /sys/class/thermal/thermal_zone*/temp | awk '{printf "%.1f°C\n", $1/1000}'
```

---

## 라이선스 (License)

이 프로젝트는 [MIT License](LICENSE)를 따릅니다.
