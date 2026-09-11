# D-Robotics RDK-X5 MPV Scripts & Deployment Assets

이 디렉토리는 **D-Robotics RDK-X5** 플랫폼의 하드웨어 가속 비디오 재생 파이프라인(Wave521 VPU + Vivante DirectVIV Zero-Copy)을 위한 보조 스크립트 및 설정 파일 모음입니다.

---

## 📁 구성 파일 안내

### 1. `fallback-restart.lua` (자동 검증 및 SW 디코더 스마트 전환 스크립트)
VPU 하드웨어 디코더의 이상 동작 및 손상 스트림을 감지하여 안전하게 소프트웨어 디코더로 전환하는 mpv Lua 스크립트입니다.

* **최초 5초 하드웨어 검증 단계 (Silent Probing)**:
  * 재생 시작 직후 5초간 검은 화면 오버레이 및 음소거 상태로 백그라운드에서 하드웨어 가속 정상 여부를 검증합니다.
  * **어노말리 3회 누적 또는 디코딩 실패 시**:
    * 즉시 소프트웨어 디코더(`hwdec=no`)로 전환
    * 영상 시작 지점(`00:00:00`)으로 자동 되감기
    * 화면 중앙에 2줄 안내 OSD 3.5초간 표시:
      ```text
      SW디코더로 전환 합니다
      잠시만 기다려주세요
      ```
    * 오버레이 제거 및 음소거 해제 후 처음부터 깨끗하게 소프트웨어 재생
  * **5초간 이상 없을 시**:
    * 영상 시작 지점(`00:00:00`)으로 자동 되감기
    * 오버레이 제거 및 음소거 해제 후 60fps DirectVIV 제로카피 하드웨어 가속 유지
* **이후 재생 중 오류 감지 (Mid-Playback Fallback)**:
  * 정상 재생 진입 후 비트스트림 오류 등으로 어노말리가 3회 누적되거나 디코더 장애 발생 시
  * **영상을 처음으로 되감지 않고 현재 재생 위치 그대로 유지**하며 SW 디코더로 즉각 인플레이스(In-place) 전환
  * 화면 좌측 상단에 작은 안내 OSD 3.0초간 표시:
    ```text
    SW디코더 전환
    ```

### 2. `mpv.conf` (RDK-X5 최적화 재생 환경설정)
* **DirectVIV Zero-Copy 기본 활성화**:
  * `vo=gpu`, `gpu-context=drm`, `hwdec=vaapi`
* **소프트웨어 폴백 활성화**:
  * `vd-lavc-software-fallback=1`
* **스트리밍 최적화**:
  * 유튜브 1080p H.264 AVC1 우선 스트림 선택
  * HLS / m3u8 실시간 스트림 자동 CPU 디코딩 프로파일(`[hls-live]`) 내장
* **X11 데스크톱 예외 프로파일**:
  * `mpv --profile=x11 <file>` 실행 시 `vo=x11`, `hwdec=vaapi-copy` 자동 적용

### 3. `mpv-launcher-wrapper.sh` (프로덕션 실행 래퍼)
* 시스템 환경변수 자동 구성:
  * `LIBVA_DRIVER_NAME=hobot`
  * `LD_LIBRARY_PATH=/usr/hobot/lib:$LD_LIBRARY_PATH`
* DRM/KMS 직결 시 Xorg / LightDM 간 DRM Master 권한 충돌 자동 중재 (임시 정지 및 종료 시 자동 복원)

---

## 🚀 설치 및 적용 방법

```bash
# 1. MPV 스크립트 설치
mkdir -p ~/.config/mpv/scripts
cp fallback-restart.lua ~/.config/mpv/scripts/

# 시스템 전역 적용 시 (선택)
sudo mkdir -p /etc/mpv/scripts
sudo cp fallback-restart.lua /etc/mpv/scripts/

# 2. MPV 설정 파일 적용
cp mpv.conf ~/.config/mpv/mpv.conf
# 시스템 전역 적용 시: sudo cp mpv.conf /etc/mpv/mpv.conf

# 3. 런처 스크립트 등록
sudo cp mpv-launcher-wrapper.sh /usr/local/bin/mpv
sudo chmod +x /usr/local/bin/mpv
```
