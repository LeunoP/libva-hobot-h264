# libva-hobot-h264 — RDK X5 VA-API Driver

[English](README.md) | [한국어](README.ko.md)

RDK X5 (D-Robotics, ARM AArch64) 플랫폼의 VPU를 위한 커스텀 VA-API 드라이버.

> [!NOTE]
> **알림**: 해당 코드는 AI(Google DeepMind Antigravity / Gemini)가 작성 및 최적화했습니다.

## 지원 환경

- **보드**: RDK X5 (D-Robotics, Cortex-A55)
- **OS**: Ubuntu 22.04 (aarch64)
- **드라이버명**: `hobot` (`LIBVA_DRIVER_NAME=hobot`)
- **설치 경로**: `/usr/lib/aarch64-linux-gnu/dri/hobot_drv_video.so`

## 지원 코덱

| 코덱 | 디코더 (VLD) | 인코더 (EncSlice) |
|------|:-----------:|:-----------------:|
| H.264 Constrained Baseline | ✅ | ✅ |
| H.264 Main | ✅ | ✅ |
| H.264 High | ✅ | ✅ |
| JPEG | ✅ (EncPicture) | ✅ |
| **HEVC/H.265 Main** | ❌ 제거됨 | ❌ 제거됨 |
| **HEVC/H.265 Main10** | ❌ 제거됨 | ❌ 제거됨 |

> **HEVC 비활성화 이유**: VPU의 HEVC 디코딩 출력에서 색상 깨짐(rainbow noise) 현상 발생.
> VA-API 프로파일에서 제거하여 소프트웨어 디코딩으로 폴백되도록 설정.

## VPU 출력 특성

- **픽셀 포맷**: NV12
- **스트라이드**: `width`
- **수직 스트라이드**: 8라인 패딩 (`vstride = (height+7)&~7`)
  - 예) 1920×1080 → `vstride = 1088`, UV 오프셋 = 1920×1088 = 2,088,960

## 빌드

```bash
cd /home/rho412/libva-hobot
make -j4
sudo cp hobot_drv_video.so /usr/lib/aarch64-linux-gnu/dri/
```

## 환경 변수

`/etc/environment`, `~/.bashrc`, `/etc/bash.bashrc`:
```
LIBVA_DRIVER_NAME=hobot
```

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

## 권장 MPV 재생 방법

### 모드 1: 독립 실행형 하드웨어 제로카피 (DRM/KMS) — 권장
DirectVIV 인터옵 모듈이 통합된 mpv를 사용하며, DRM Master를 독점하지 않도록 디스플레이 매니저(LightDM/Xorg)를 종료하거나 독립 TTY 환경에서 실행합니다:

```bash
# Vivante 라이브러리 경로 지정 후 DRM 모드로 mpv 실행
LD_LIBRARY_PATH=/usr/hobot/lib mpv --gpu-context=drm --vo=gpu --hwdec=vaapi /path/to/video.mp4
```

**실측 검증 성능:**
- **재생 품질**: 1080p 60.000 fps 완벽 유지
- **드롭 프레임**: 0개 (정상 재생 중 드롭 발생 없음)
- **A-V 동기화**: `A-V: 0.000` 완벽 일치
- **CPU 점유율**: 단일 코어 기준 ~35% (8코어 전체 시스템 기준 5% 미만)

### 모드 2: 데스크톱 X11 환경 (소프트웨어 복사 폴백)
Xorg가 화면을 제어 중인 데스크톱 세션:

`~/.config/mpv/mpv.conf`:
```ini
hwdec=vaapi-copy
vo=x11
sws-allow-zimg=no
sws-scaler=fast-bilinear
sws-fast=yes
audio-buffer=1
fs=yes
```

---

## DirectVIV 서피스 확장 인터페이스 (`va/va_hobot.h`)

`vaExportSurfaceHandle` 표준 디스패치와 벤더 메모리 타입 `VA_SURFACE_ATTRIB_MEM_TYPE_HOBOT_GRAPH_BUF` (`0x80484F10`)를 통해 VPU 물리/가상 주소를 질의합니다:

```c
#include <va/va.h>
#include <va/va_hobot.h>

struct hobot_surface_info info = {0};
VAStatus status = vaGetHobotSurfaceInfo(va_dpy, surface_id, &info);
```

## H.264 B-프레임 순서 및 페이싱 안정화 (2026-09-11)

### 문제
- B-프레임이 포함된 H.264 영상(예: `I, B, B, B, B, P...`) 재생 시 화면이 초당 60회씩 앞뒤로 미세하게 튀며 끊기는 현상(Frame Pacing Jitter) 발생.
- 원인:
  1. VPU 디코더에 `reorder_enable = 1`(기본값)이 설정되어 VPU가 B-프레임 대기 지연을 발생시킴.
  2. `vaSyncSurface`가 FIFO 큐에서 버퍼를 꺼낼 때 디코드 순서와 표시면(PTS) 순서가 달라 P-프레임과 B-프레임이 서로 엉뚱한 서피스에 할당됨.
  3. `vaSyncSurface`가 디큐 실패/타임아웃 시에도 무조건 SUCCESS를 반환하여 에러 은폐 및 프레임 드롭 폭주 유발.

### 해결
1. **`reorder_enable = 0`**: VPU 내부 재정렬을 끄고 즉시 디코드 순서로 출력.
2. **제출 서피스 FIFO 매핑 (`submitted_surfaces`)**: `vaBeginPicture`에 등록된 순서대로 VPU 디큐 프레임을 1:1 결합하여 각 서피스가 자신의 프레임만을 갖도록 보장.
3. **`vaQuerySurfaceStatus` / `vaSyncSurface` 상태 정상화**: 실제 디코드 완료 여부(`has_decoded_frame`) 반영 및 3회 재시도 로직.
4. **오디오 언더런 방지**: `mpv.conf`에 `audio-buffer=1` 추가로 PulseAudio 리샘플링 지연에 의한 비디오 건너뜀 제거.

## 알려진 문제

| 문제 | 상태 |
|------|------|
| HEVC VPU 깨짐 | ⚠️ VA-API에서 제거, SW 디코딩 폴백 |
| H.264 60fps B-프레임 페이싱/끊김 | ✅ VPU Reorder 해제 + FIFO 서피스 큐 매핑으로 완전 해결 |
| H.264 60fps 오디오 언더런 연계 드롭 | ✅ audio-buffer=1 로 완전 해결 |
| MPV SDL VO 검정 화면 | ❌ SDL LockTexture/OpenGL 호환성 미해결 |
| vo=xv 미지원 | ❌ 플랫폼에서 Xvideo 없음 |

## 라이선스 (License)

이 프로젝트는 [MIT License](LICENSE)를 따릅니다.
