# libva-hobot-h264

D-Robotics RDK-X5 Wave521 VPU용 VA-API 드라이버입니다.

[English](README.md) | [한국어](README.ko.md)

RDK-X5 Linux 멀티미디어 스택과 벤더 라이브러리를 대상으로 합니다. VA-API 전체가 아니라 검증된 기능 부분집합을 구현합니다.

## 하드웨어 지원

| 코덱 | 하드웨어 디코드 | 하드웨어 인코드 | 범위 |
|---|---|---|---|
| H.264 | Constrained Baseline, Main, High (Level 5.1까지 검증) | Constrained Baseline, Main, High | 검증된 8-bit 4:2:0 패턴. 인코드는 I/P, 프레임당 전체 단일 슬라이스 |
| HEVC | Main, 8-bit 4:2:0 | Main, 8-bit 4:2:0 | 검증된 비트스트림 패턴으로 제한. 인코드는 I/P, 프레임당 전체 단일 슬라이스 |
| JPEG | Baseline, 8-bit 4:2:0 | Baseline, 8-bit 4:2:0 | VA 기능 일부만 지원 |
| H.264 Level 5.2 또는 미지원 DPB 패턴 | **미지원** | 해당 없음 | MPV 보조 스크립트를 설정하면 SW 디코드로 전환할 수 있으나, 하드웨어 지원을 뜻하지 않습니다. |
| HEVC Main 10 및 NV12 이외 형식 | **미지원** | **미지원** | 드라이버가 소프트웨어 폴백을 제공하지 않습니다. |

**SW 폴백은 코덱 지원으로 계산하지 않습니다.** 드라이버는 하드웨어 기능만 광고합니다. `misc/mpv_scripts/fallback-restart.lua`는 이상이나 디코드 오류가 발생하면 MPV에 SW 디코드를 요청할 수 있습니다. 이 기능은 스크립트와 소프트웨어 디코더가 설치·설정된 경우에만 사용할 수 있습니다.

## 기능 및 제한

- H.264/HEVC EncSlice는 검증된 I/P 경로에서 CBR, VA VBR, CQP를 지원합니다. VA VBR은 SDK AVBR target/VBV와 별도 최대 비트레이트 설정으로 변환합니다. 동작 의미가 확인되지 않아 VA AVBR은 광고하지 않습니다.
- 디코드 표면은 짝수 크기의 8-bit NV12입니다. 표면 한도는 프로파일과 entrypoint에 따라 다르며, HEVC Main VLD는 최대 8192×4096, Main EncSlice는 최대 3840×2160까지 검증했습니다.
- HEVC VLD는 검증된 문법 및 참조 패턴만 받습니다. Main 10, long-term reference, 임의의 고급 도구 조합은 미지원 또는 미검증입니다.
- DRM PRIME 2 export는 검증된 레이아웃을 지원합니다. import는 HBmem 메타데이터를 확인할 수 있는 선형 단일 객체 NV12 버퍼로 제한됩니다.
- 손상된 VPU 출력을 감지하고 프로세스별 이벤트를 `/dev/shm/hobot_va_watchdog.<pid>`에 기록합니다. 이 감시 기능 자체가 SW 디코더는 아닙니다.
- 실제 처리량과 화면 프레임률은 스트림, 펌웨어, 렌더러 및 보드 설정에 따라 달라집니다. 하드웨어 시험 목록은 [`tools/README.md`](tools/README.md)를 참조하세요.

## 빌드 및 설치

D-Robotics 멀티미디어 SDK가 설치된 RDK-X5에서 빌드합니다.

```bash
sudo apt-get install build-essential libva-dev libdrm-dev vainfo
make -j"$(nproc)"
sudo make install
LIBVA_DRIVER_NAME=hobot vainfo
```

드라이버는 `/usr/hobot/lib`의 벤더 라이브러리를 사용합니다. `make install`은 드라이버와 `include/va/va_hobot.h`를 설치합니다.

시스템에 설치하지 않고 후보 빌드를 시험하려면:

```bash
make -j"$(nproc)"
LIBVA_DRIVER_NAME=hobot LIBVA_DRIVERS_PATH="$PWD" vainfo
```

## 테스트 및 MPV 설정

대상 보드에서 API/state 테스트를 빌드하고 실행합니다.

```bash
make build-va-tests
make test-va-state
LIBVA_DRIVER_NAME=hobot /tmp/libva-hobot-tests/test_va_config
```

하드웨어 회귀 스크립트와 요구사항은 [`tools/README.md`](tools/README.md)에 있습니다. MPV DRM/GBM, X11, HLS 및 폴백 예시는 [`misc/mpv_scripts/README.md`](misc/mpv_scripts/README.md)를 참조하세요. MPV의 SW 폴백은 복구 경로이며 하드웨어 지원 범위를 넓히지 않습니다.

## 라이선스

MIT. 자세한 내용은 [LICENSE](LICENSE)를 참조하세요.
