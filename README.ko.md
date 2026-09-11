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

## MPV 설정

### `/etc/mpv/mpv.conf` 및 `~/.config/mpv/mpv.conf`
```ini
hwdec=vaapi-copy
vo=x11
sws-allow-zimg=no
sws-scaler=fast-bilinear
sws-fast=yes
```

### 래퍼 `/usr/local/bin/mpv`
```bash
#!/bin/bash
export LIBVA_DRIVER_NAME=hobot
# /usr/hobot/lib 내 불완전한 Vivante Vulkan 라이브러리 충돌 방지
export LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libvulkan.so.1${LD_PRELOAD:+:$LD_PRELOAD}

args=()
for arg in "$@"; do
    if [ "$arg" = "--gpu-context=x11egl" ] || [ "$arg" = "--vo=gpu" ] || [ "$arg" = "--hwdec=no" ]; then
        continue
    fi
    args+=("$arg")
done
exec /usr/bin/mpv "${args[@]}"
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
