# libva-hobot-h264 — RDK X5 VPU VA-API 드라이버

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-D--Robotics%20RDK--X5-green.svg)]()
[![Architecture](https://img.shields.io/badge/Arch-ARM64%20(aarch64)-orange.svg)]()
[![Codec](https://img.shields.io/badge/Codec-H.264%20%7C%20JPEG-red.svg)]()
[![Branch: dev](https://img.shields.io/badge/Branch-dev-brightgreen.svg)]()

[English](README.md) | [한국어](README.ko.md)

`libva-hobot-h264`는 **D-Robotics RDK-X5** (Chips&Media Wave521 VPU + ARM Cortex-A55) 싱글보드 컴퓨터를 위한 오픈소스 고성능 VA-API(Video Acceleration API) 백엔드 드라이버입니다.

표준 리눅스 미디어 소프트웨어에서 사용할 수 있는 D-Robotics VPU용 VA-API 드라이버입니다. 실제 해상도와 프레임률은 스트림, 펌웨어, 렌더러 및 보드 설정에 따라 달라지며, 이 문서는 특정 성능을 보장하지 않습니다.

> [!NOTE]
> **알림**: 본 코드베이스 및 드라이버 아키텍처는 Google DeepMind Antigravity / Gemini 및 OpenAI Codex AI가 LeunoP와의 협업을 통해 연구, 개발 및 최적화했습니다.

---

## 주요 기능 및 아키텍처 특징

- **VA-API 지원 부분집합**: 아래 지원 범위와 제한 API를 참조하세요. 전체 VA-API 기능 또는 모든 libva 클라이언트와의 호환성을 의미하지 않습니다.
- **H.264 B-프레임 처리**: VPU 내부 재정렬 설정과 128엔트리 FIFO(`submitted_surfaces`)를 사용해 제출된 표면과 출력 프레임을 대응시킵니다. 6개 연속 B-frame을 포함한 64프레임 H.264 High 샘플을 하드웨어로 디코드하고 소프트웨어 NV12 결과와 프레임별 완전 일치함을 확인했습니다. `tools/test_h264_bframe_decode.sh`로 재검증할 수 있습니다.
- **H.264 인코더 시퀀스 설정**: 첫 인코딩 프레임까지 VPU 시작을 미뤄 요청된 H.264 레벨과 시퀀스 VUI 타이밍/SAR을 초기화 전에 적용합니다. VA-API가 이 경로에서 입력 full-range 메타데이터를 제공하지 않으므로 NV12 출력을 limited-range로 명시합니다.
- **H.264 visible 크기 표면**: 매크로블록 정렬된 coded 크기와 정확히 일치하는 visible 크기의 NV12 표면을 지원합니다. 입력을 SPS crop 위치에 배치하고 가장자리 픽셀을 복제해 coded 프레임을 채우며, coded 크기 표면은 기존 좌표 그대로 복사합니다.
- **H.264 파라미터 세트 갱신**: 합성 SPS/PPS가 변경되면 다음 픽처 전에 다시 주입하여 스트림 재협상 시 디코더 상태를 최신 상태로 유지합니다.
- **VPU 출력 무결성 판정 및 손상 프레임 DROP**: 벤더 라이브러리의 에러 마스킹을 우회하여 드라이버 경계에서 `err_mb_in_frame_display`를 직접 검사. 손상 프레임(`err_mb > 0`)을 화면에 전달하지 않고 VPU 버퍼 풀로 자동 반환하여 화면 찢어짐(Tearing) 원천 차단.
- **VPU 빈 출력 버퍼 보호**: 디코더 시작 또는 해상도 재협상 중 발생하는 빈 출력 버퍼를 이미지 매핑·DRM export 전에 검증하고 VPU로 반환합니다.
- **Hobot-VA WatchDog 실시간 IPC 텔레메트리**: VPU 이상 상태를 PID별 `/dev/shm/hobot_va_watchdog.<pid>` 파일로 전달합니다. 고유 임시 파일, 원자적 이름 변경, 단조 증가 이벤트 순번으로 동시 디코더 스레드와 독립 플레이어 간 덮어쓰기·삭제 간섭을 막습니다.
- **멀티 슬라이스 프레임 결합**: 방송용 인코더 및 스트리밍 서버의 다중 슬라이스 패킷을 VPU `MC_FEEDING_MODE_FRAME_SIZE` 규격에 맞춰 1개 프레임으로 자동 결합.
- **DMA-BUF 및 DRM PRIME 2 Export**: 디코드된 VPU DMA-BUF를 `dup(fd)`로 안전하게 export합니다. 디코드 출력이 없는 vendor surface-info 경로에서만 Hobot 그래픽 버퍼를 지연 할당합니다.
- **2단계 MPV 자동화 보조 스크립트 모음 (`misc/mpv_scripts/`)**: 최초 5초 무음 프로빙 검증 및 재생 중 인플레이스 SW 전환을 위한 프로덕션 스크립트 내장.

---

## 지원 코덱 매트릭스

| 코덱 | 프로파일 | 디코더 (VLD) | 인코더 (EncSlice / EncPicture) |
|---|---|:---:|:---:|
| **H.264 (AVC)** | Constrained Baseline | ✅ | ✅ 제한 지원 |
| **H.264 (AVC)** | Main | ✅ | ✅ |
| **H.264 (AVC)** | High (최대 Level 5.1) | ✅ | ✅ |
| **H.264 (AVC)** | High (Level 5.2 / DPB 16 초과 스트림) | 🛡️ 자동 DROP → SW 폴백 | ❌ |
| **JPEG** | Baseline 8-bit YUV 4:2:0 | ✅ 제한 지원 | ✅ |
| **HEVC (H.265)** | Main, 8-bit YUV 4:2:0 (제한 지원) | ✅ 제한 지원 | ✅ 제한 지원 |
| **HEVC (H.265)** | Main 10 / 기타 형식 | ❌ 미지원 | ❌ 미지원 |

> **Level 5.2 / 고참조(High DPB) 스트림 처리**: 유튜브 1080p60 등 참조 프레임 수가 16개에 달하는 고레벨 비트스트림은 VPU의 하드웨어 DPB 한계를 초과하여 매크로블록 손상(`err_mb > 0`)이 발생합니다. 본 드라이버는 이를 안전하게 폐기하고 동반 스크립트를 통해 CPU 소프트웨어 디코딩으로 매끄럽게 전환합니다.

> **HEVC 지원 범위**: 기존 출력 손상은 VA picture 파라미터와 다른 기본값으로 VPS/SPS/PPS를 합성한 문제로 확인했습니다. 현재 드라이버는 전달받은 VA 파라미터로 헤더를 합성하며, 검증된 Main 8-bit 4:2:0 범위만 노출합니다. 인코딩은 CBR, I/P 프레임, 프레임당 전체 단일 슬라이스로 제한됩니다. Main 10 및 미지원 비트스트림 기능은 계속 거부합니다.

## VA-API 범위와 제한

HEVC SPS의 short-term RPS 세트가 0개 또는 1개인 VLD 입력은 inline 및 SPS 선택 참조 집합을 검증합니다. Inline 문법의 음수/양수 참조 수와 각 delta·사용 플래그를 파싱해 VA-API `st_rps_bits`, 활성 참조 수 및 선택적 list modification과 대조합니다. 단일 SPS RPS를 선택하면 VA DPB 플래그와 POC에서 현재 참조를 재구성하고 SAO 플래그, P/B 기본 리스트 순서, VA `RefPicList` 인덱스가 일치하는지 검사합니다. Long-term reference는 지원하지 않으며 다른 슬라이스 도구는 아래에 명시한 검증 패턴으로 제한됩니다.

SAO가 활성화된 640×360 단일 SPS-RPS P fixture(2프레임)는 후보 드라이버의 새 VPU 세션 5회에서 모두 소프트웨어와 비트 단위로 일치했습니다(`test_hevc_single_rps_sao_decode.sh`).

- 표면 및 이미지 경로는 8-bit NV12 / YUV420이며, 너비와 높이는 짝수여야 합니다. 일반 표면 크기는 64~4096 픽셀 범위이며 HEVC Main 컨텍스트는 최대 3840×2160으로 제한됩니다.
- H.264 VLD와 EncSlice는 Constrained Baseline, Main, High를 광고합니다. Constrained Baseline 인코딩은 SDK Baseline 프로파일을 선택하고 CAVLC를 강제하며 8x8 변환을 비활성화합니다. 검증된 Baseline SPS NAL에만 `constraint_set1_flag`를 설정하고, chroma QP 의미를 보존할 수 있는 0값 PPS 고프로파일 확장만 제거합니다. 지원 범위는 검증된 8-bit 4:2:0, CBR, I/P 프레임, 프레임당 전체 단일 슬라이스로 제한하며 문법과 하드웨어 디코드는 `test_h264_encode_profiles.sh`에서 검사합니다.
- H.264 디코더는 Constrained Baseline, Main, High에 맞는 SPS를 합성하며 8-bit 4:2:0과 POC type 0/2를 지원합니다. 코딩 매크로블록 치수는 VA 컨텍스트 경계를 매크로블록 단위로 올림한 범위까지 허용하고, VA DPB 용량 16개를 넘는 참조 프레임 수도 거부합니다. VA picture 파라미터에 POC type 1의 offset-cycle 값이 없어 해당 입력은 거부합니다. 합성 PPS가 표현하지 않는 FMO 및 redundant picture-count 모드도 거부합니다. Constrained Baseline에서는 인터레이스, CABAC, 가중 예측, 8x8 변환 파라미터를 거부합니다. High 프로파일의 monochrome 및 4:2:2/4:4:4 형식도 지원하지 않습니다.
> HEVC 참조 리스트 수정 범위: SPS RPS가 0개인 inline 경로는 신호된 재정렬을 파싱해 VA `RefPicList`와 대조합니다. 실제 P L0 재정렬 `[1,0]`과 세 참조를 사용하는 B L0/L1 재정렬 `[1,2,0]`의 640×360 Main fixture를 각각 후보 드라이버 새 VPU 세션 5회로 검증했으며, 두 fixture 모두 8프레임 전체가 소프트웨어 출력과 비트 단위로 일치했습니다(`test_hevc_inline_refmod_decode.sh`). 별도의 두 SPS-RPS 인터 경로는 활성 리스트 수정을 계속 거부하며, 다른 inline RPS/list 배치까지 검증된 것은 아닙니다.
- HEVC VLD는 Main Level 5.1, 8-bit 4:2:0, 컨텍스트와 일치하는 최대 3840×2160 크기, CTU 주소가 0에서 시작해 그림 내부에서 증가하는 순서의 long-format 슬라이스 열을 지원합니다. Main 10, tiles, WPP, scaling list, PCM, 별도 색 평면, long-term reference, dependent slice, 불완전한 slice-data 조각과 entry-point offset은 거부합니다. 슬라이스 내부 RPS 비트 수는 허용합니다. SPS RPS 두 세트 형식은 각 슬라이스가 제한된 지원 패턴을 개별 검증받아야 합니다. IDR은 DPB가 비어 있어야 하고, P는 RPS 인덱스 0을 선택해 바로 이전 POC 하나만 참조해야 합니다. B도 RPS 인덱스 0과 같은 직전 POC 하나를 L0/L1 양쪽에 사용해야 합니다. 지원하는 8-bit 4:2:0 경로의 luma/chroma weighted P/B 테이블을 문법·값 범위·헤더 경계까지 검사합니다. 실제 참조 리스트 수정 문법, temporal MVP, SAO와 검증되지 않은 다른 슬라이스 도구는 거부합니다. 단위검사는 잘못된 값, chroma weight/offset 경계와 헤더 경계를 포함합니다. 후보 드라이버에서 640×360 HM 2-slice, 두 SPS RPS 스트림에 중립 테이블, 비중립 luma offset 테이블, Cb/Cr offset +8 chroma 테이블을 넣어 검증했습니다. 각 테이블은 P 패턴 3프레임과 B 패턴 4프레임에서 소프트웨어와 비트 단위로 일치했고, 각 사례를 5회 반복 통과했습니다. 다른 가중치 값과 다른 RPS/weight-table 배치는 미검증입니다. 단일 참조라 list-modification 문법이 생기지 않는 경우 PPS capability flag만 설정된 입력은 허용합니다. 이 조건에서 단일·멀티슬라이스 그림을 허용합니다. 후보 드라이버의 640×360 HM 2-slice IDR+P+P 및 IDR+BBB 스트림은 각각 새 VPU 세션 5회 모두 소프트웨어와 비트 단위로 일치했습니다. 두 세트 이외의 0이 아닌 SPS RPS 개수는 독립 IDR 그림에서만 허용하며, 사용되지 않는 RPS는 드라이버가 합성하는 SPS에서 제외합니다. SPS RPS 14개인 640×360 2-slice IDR도 5회 모두 일치했습니다. 별도의 단일 슬라이스 inline-RPS 가중 P(24프레임) 및 weighted B3(32프레임) 스트림도 후보 드라이버에서 비트 단위로 일치했습니다. 이 결과는 다른 RPS/weight-table 배치를 검증하지 않습니다. 임의의 멀티슬라이스 인터 스트림은 아직 미검증이며 두 세트가 아닌 0이 아닌 SPS RPS 구성은 인터 그림에서 fail-closed로 거부합니다. 기존 단일 슬라이스 테스트는 640×360 P(30프레임), 1280×720@60 B3/B6(각 64프레임), 1920×1080@60 B6(64프레임), 3840×2160@60 B3(16프레임)에서 비트 단위로 일치했습니다. 60fps는 스트림 타임스탬프이지 지속 디코드 처리량 측정값이 아닙니다.
- HEVC 두 SPS RPS 세트의 좁은 추가 지원 패턴으로, B 슬라이스는 RPS 인덱스 0을 선택하고 바로 이전 POC 하나를 L0/L1 양쪽의 동일한 유일 참조로 사용해야 합니다. 지원하는 luma/chroma weighted P/B 문법은 단위 및 값 범위 검사에 포함됩니다. 두 SPS RPS와 중립, 비중립 luma offset, Cb/Cr offset +8 chroma 테이블을 결합한 2-slice HM 스트림은 후보 드라이버에서 P 패턴 3프레임과 B 패턴 4프레임 모두 소프트웨어와 비트 단위로 일치했고, 각 사례를 5회 반복 통과했습니다. 다른 가중치 값은 미검증입니다. 실제 참조 리스트 수정 문법, temporal MVP, SAO는 계속 거부합니다. 단일 참조인 경우 문법이 생기지 않으므로 PPS의 list-modification capability flag만 설정된 입력은 허용합니다. 640×360 IDR+BBB 2-slice HM 스트림은 후보 드라이버에서 새 VPU 세션 5회 모두 소프트웨어와 비트 단위로 일치했습니다. 임의의 멀티슬라이스 인터 스트림을 의미하지 않습니다.
- HEVC EncSlice는 Main 8-bit 4:2:0, CBR, I/P 프레임, 프레임당 전체 단일 슬라이스로 제한됩니다. 디코더 SDK가 HEVC SAO를 지원하지 않아 초기화 시 SAO를 명시적으로 끕니다. SDK의 HEVC crop 설정은 SPS를 바꾸지 않아 오른쪽/아래 크롭에는 coded 크기를 보존하는 표준 SPS conformance-window 문법을 추가합니다. 각 크롭은 16픽셀 미만이고 4:2:0 crop 단위에 맞아야 하며 CTU 격자가 같고 coded buffer에 추가 문법을 넣을 공간도 있어야 합니다. coded 640×368에서 visible 640×360, 638×360, 638×362, 626×354의 64프레임 Level 4.1 스트림은 모두 하드웨어 재디코드가 소프트웨어와 비트 단위로 일치했습니다. 다른 모든 기하 조합을 포괄하는 검증은 아닙니다.
- H.264 VLD 슬라이스 파라미터는 `slice_data_offset`, `slice_data_size`, `ALL/BEGIN/MIDDLE/END` 플래그를 반영합니다. 여러 요소를 가진 파라미터 버퍼 1개와 데이터 버퍼 1개, 또는 요소 1개씩인 동일 개수의 순서 대응 버퍼 쌍을 지원하며, 모호한 조합과 범위 밖 데이터는 거부합니다. 640×360 High, 그림당 2슬라이스, 6연속 B-frame인 64프레임 스트림을 새 VPU 세션 5회 모두 소프트웨어와 비트 단위로 일치시켰습니다.
- H.264 인코딩은 CBR 및 프레임당 단일 전체 프레임 I/P 슬라이스로 제한되며 최대 슬라이스 수는 1입니다. SDK 고정 GOP는 단일 참조 I/P 패턴을 사용하므로 B/SP/SI 슬라이스, 여러 슬라이스, 부분 프레임 슬라이스, 매크로블록 맵과 미지원 인코더 버퍼/misc 파라미터는 거부합니다. VA HRD/VBV 버퍼 크기는 SDK의 10~3000ms VBV 시간 창으로 변환합니다. 설치된 SDK에는 VA 초기 CPB fullness 제어 항목이 없어 해당 값은 적용하지 못합니다. H.264 레벨과 지원되는 시퀀스 VUI 항목은 VPU 시작 전에 고정되며 변경하려면 새 VA 컨텍스트가 필요합니다. JPEG VLD는 Baseline 순차형 8-bit 3성분 YUV 4:2:0, 단일 전체 프레임 스캔, 회전 없음으로 제한됩니다. 다른 샘플링, progressive/multi-scan 입력과 미지원 VA 필드는 거부합니다. 소프트웨어 NV12 결과와 픽셀 비교 및 반복 하드웨어 디코드를 검증했습니다. JPEG 인코딩은 Baseline 순차형 8-bit 3성분, 프레임별 품질값(1~100), 전달된 양자화표와 restart interval을 지원합니다. 사용자 지정 Huffman 표 등 미지원 인코더 설정은 거부합니다. `VA_ATTRIB_NOT_SUPPORTED` 속성은 지원되지 않습니다.
- DMA-BUF 표면 내보내기는 DRM PRIME 2를 지원합니다. 사전 할당 NV12 표면은 경계를 검증한 단일 또는 분리 FD 레이아웃만 허용하며, 검증되지 않은 다중-FD 디코더 출력은 거부합니다. 레거시 DRM PRIME과 외부 메모리 가져오기는 지원하지 않습니다.
- Subpicture, display attribute 조작, image palette, `vaPutSurface`는 구현되지 않았으며 호출 시 성공을 가장하지 않고 오류를 반환합니다. `vaPutImage`는 CPU NV12 staging 경로에서 동작하며 하드웨어 블릿을 제공하지 않습니다.
- 단위 테스트와 빌드 성공은 실제 보드의 재생·인코딩 성능 검증을 대체하지 않습니다. `/dev/dri`와 Wave521 하드웨어가 있는 대상 보드에서 스트림 회귀 테스트를 별도로 수행해야 합니다.
- `tools/test_va_surface_state.c`는 멀티미디어 호출을 모의해 버퍼 소유권과 teardown, 스레드·프로세스별 watchdog IPC 격리 및 재시작 순번, 요청 coded-buffer 용량 보존, API 배열 경계, 인코더 설정 오류 전파, H.264 HRD-to-VBV 변환과 단일 슬라이스 제한, H.264 컨텍스트 치수/DPB 경계 및 디코더 슬라이스 파라미터/분할 검증, HEVC Main 허용 범위, inline RPS 및 단일 SPS-RPS의 DPB 기반 참조 재구성/P·B 리스트 순서/SAO 검사, 2-RPS luma/chroma weighted P/B 헤더 파싱, 잘못된 weight/offset 및 헤더 경계 거부, 32비트 제출 FIFO 순환, sync 동시성, surface lock 수명, PRIME export 플래그, NV12 plane/객체 경계와 Y/UV 독립 pitch 처리를 검사합니다. 두 SPS RPS 가중치 하드웨어 회귀는 아래 별도 스크립트가 담당합니다.
- `tools/test_va_surface_export.c`는 VA-API로 입력 전체를 디코드하고 모든 VPU 프레임을 동기화한 뒤 NV12 및 separate-layer DRM PRIME 2 export와 plane/객체 경계를 검증합니다.
- `tools/test_va_jpeg_encode.c`는 품질값(1~100)을 picture parameter로 전달해 JPEG Baseline 하드웨어 인코딩을 수행하고 결과 파일을 저장합니다. 서로 다른 품질값으로 실행해 크기 차이와 디코드 가능성을 확인할 수 있습니다.
- `tools/test_va_jpeg_decode.sh`는 Baseline 4:2:0 JPEG를 생성해 한 VA 컨텍스트에서 반복 디코드하고, 출력 NV12를 full-range 소프트웨어 기준과 PSNR 비교합니다. 기본 5회이며 `HOBOT_JPEG_DECODE_CYCLES`로 횟수를 바꿀 수 있습니다.
- `tools/test_hevc_main_decode.sh`는 HEVC Main 8-bit 4:2:0 스트림을 640×360, 720p60, 1080p60 Level 5.0, 4K60 Level 5.1로 생성합니다. P/B-frame 패턴과 레벨을 확인하고, non-default luma offset이 실제 포함된 no-B weighted-P 및 B3 weighted-bipred 케이스도 하드웨어 NV12 프레임과 소프트웨어 결과를 `framemd5`로 비교합니다. `HOBOT_HEVC_WEIGHTED_ONLY=1`은 두 가중 예측 회귀만, `HOBOT_HEVC_WEIGHTED_P_ONLY=1`은 P 회귀만 실행합니다.
- `tools/test_hevc_multislice_b_decode.sh`는 640×360 HM 샘플의 IDR+BBB 저지연 B 패턴과 그림당 2슬라이스를 확인합니다. B의 L0/L1은 바로 이전 POC 하나를 공통 참조로 사용하며, 기본 5개 새 VPU 세션에서 모든 NV12 프레임을 소프트웨어와 비교합니다.
- `tools/test_hevc_two_rps_weighted_decode.sh`는 기존 2-RPS HM 샘플에 중립, 비중립 luma offset, Cb/Cr offset +8 weighted P/B 테이블을 넣고 필드를 검증합니다. 중립 테이블은 변환 전후 소프트웨어 출력이 같고, luma/chroma offset 테이블은 출력이 실제로 바뀌는지 확인한 뒤 후보 드라이버와 비교합니다(P/B 패턴 각각 3/4프레임). 기본 5회 반복이며 `HOBOT_HEVC_TWO_RPS_WEIGHTED_CYCLES`로 횟수를 바꿀 수 있습니다. 다른 weight/RPS 조합은 미검증입니다.
- `tools/test_hevc_main_encode.sh`는 VAAPI로 HEVC Main 무-B CBR 스트림 64프레임을 인코딩하고 profile/level/visible·coded 크기/프레임 수, SPS의 SAO 비활성화와 crop offset을 확인하며 모든 하드웨어 재디코드 프레임을 소프트웨어와 비교합니다. 디코더 SDK가 HEVC SAO를 지원하지 않아 인코더 초기화 시 SAO를 명시적으로 끕니다. 640×360, 638×360, 638×362, 626×354에서 각각 64프레임 재디코드가 일치했고, 최대 14픽셀 크롭까지 확인했습니다. 모든 기하 조합을 포괄하는 검증은 아닙니다.
- `tools/test_h264_bframe_decode.sh`는 6개 이상 연속 B-frame을 가진 64프레임 H.264 High 샘플을 생성하고, ffprobe로 패턴을 확인한 뒤 하드웨어 NV12 디코드 결과를 소프트웨어 결과와 프레임별 해시 비교합니다.
- `tools/test_h264_encode_profiles.sh`는 Constrained Baseline, Main, High H.264 스트림을 각각 64프레임 인코딩하고 프로파일·레벨·픽셀 형식을 검사합니다. Constrained Baseline은 `trace_headers`로 constraint 플래그, CAVLC, 고프로파일 PPS 확장 문법의 부재 및 가중 예측 비활성화를 확인하며, 세 프로파일 모두 모든 하드웨어 재디코드 프레임을 소프트웨어와 해시 비교합니다.
- `tools/test_h264_encode.sh`는 정렬된 720p60, coded 640x368 안의 visible 640x360, 30000/1001fps H.264 VAAPI 인코딩을 반복합니다. SPS 레벨/타이밍/crop/limited-range를 검사한 뒤 모든 하드웨어 디코드 프레임을 소프트웨어 결과와 해시 비교합니다. 모의 surface-state 테스트는 좌·상단 crop 위치와 Y/UV 가장자리 복제도 검사합니다. 기본 3회이며 `HOBOT_H264_ENCODE_CYCLES`로 조정할 수 있습니다.
- `tools/test_h264_crop_encode.sh`는 638x360 VA 표면을 640x368 H.264 context에 넣고 SPS의 좌·상단 offset을 비영 값으로 지정해 인코딩합니다. 출력 crop syntax를 확인하고 visible 영역을 원본 NV12와 PSNR 비교합니다.

> Hobot codec SDK의 `mc_video_frame_buffer_info_t.vstride`는 세로 라인 수가 아니라 크로마 바이트 pitch입니다. 드라이버는 이를 공개 `hobot_surface_info.vstride`(정렬된 세로 라인 수)와 분리해 처리합니다.

---

## 하드웨어 및 시스템 요구사항

- **대상 보드**: D-Robotics RDK-X5 (8-core Cortex-A55; 메모리는 SKU별 상이)
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
 Vivante GPU (GLES)
       │
       ▼
    DRM / GBM
       │
       ▼
   HDMI 출력 (주사율과 실제 표시 프레임률은 시스템 설정에 따름)
```

### X11 vs DRM/GBM 환경 비교
- **DRM/GBM (하드웨어 DirectVIV 직결)**: 검증된 경로에서 `glTexDirectVIVMap`은 VPU의 NV12 버퍼를 CPU 프레임 복사 없이 샘플링합니다. 다만 화면 표시 프레임률을 보장하지는 않습니다. 현재 키오스크 경로의 Moonlight 실측에서는 60fps 스트림이 약 30 표시 FPS로 관측됐습니다. Vsync, 합성기 정책, 디스플레이 모드는 VPU 디코드 처리량과 별개로 표시 속도에 영향을 줍니다.
- **X11**: 렌더링 및 zero-copy 지원은 설치된 벤더 그래픽 스택과 세션 설정에 따라 다릅니다. `vaapi-copy` 경로는 CPU 메모리 복사 비용을 추가하므로 고정된 CPU 사용량이나 모든 X11 환경의 제약으로 일반화하지 말고 대상 이미지에서 검증해야 합니다.

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
   드라이버는 이상 발생 시 `/dev/shm/hobot_va_watchdog.<pid>` 파일로 실시간 상태를 게시합니다:
   ```text
   <pid> <컨텍스트_어노말리_수> <epoch_타임스탬프> <err_mb> <total_mb> <프로세스_이벤트_순번>
   ```
   10초간 정상 재생이 지속되면 컨텍스트별 어노말리 카운터는 0으로 초기화되지만 프로세스 이벤트 순번은 단조 증가합니다. 각 MPV 인스턴스는 자신의 PID 파일만 읽고 이벤트 순번으로 중복 처리를 막으며, 재생 중에는 동시 게시와의 경쟁을 피하기 위해 파일을 지우지 않고 종료 시 자기 파일만 정리합니다. 드라이버는 고유 임시 파일과 원자적 이름 변경으로 게시합니다. 플레이어 폴백 기준은 **어노말리 1회**입니다.

---

## MPV 연동 보조 스크립트 (`misc/mpv_scripts/`)

[`misc/mpv_scripts/`](misc/mpv_scripts/) 디렉토리는 RDK-X5의 안정적인 비디오 재생을 위한 통합 스크립트 모음을 제공합니다.

### 1. `fallback-restart.lua` (자동 검증 및 2단계 SW 전환 스크립트)

| 재생 단계 | 조건 | 플레이어 동작 | OSD 화면 알림 |
| **최초 검증 단계 (첫 5초)** | 어노말리 1회 또는 HW 디코드 실패 | **`00:00:00`으로 되감기**, SW 디코더(`hwdec=no`) 전환, 화면 송출 및 음소거 해제 | **좌측 상단 작게 표시 (1.5초)**:<br>`SW 디코더` |
| **최초 검증 단계 (첫 5초)** | 어노말리 0건 (정상 통과) | **`00:00:00`으로 되감기**, 화면 송출 및 음소거 해제, HW DirectVIV 유지 | *(없음 — 매끄럽게 재생 시작)* |
| **이후 재생 중 (정상 재생 진입 후)** | 재생 중 어노말리 1회 또는 디코드 오류 | **되감지 않고 현재 위치 유지**, 즉시 인플레이스 SW 전환 | **좌측 상단 작게 표시 (1.5초)**:<br>`SW 디코더` |

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
| **H.264 다중 참조 PPS 오류** | ✅ 완전 해결 | Wave521에 필요한 PPS L0 기본 참조 수를 조건부로 합성 |
| **시작/재협상 중 VPU 빈 출력 버퍼** | ✅ 완전 해결 | 물리 주소·payload 크기·가상 포인터 검증 후 보관/재활용 |
| **잘못된 NV12 레이아웃 및 크기 경계** | ✅ 수정 | 짝수 크기, SDK plane별 크기, FD/객체 경계와 파생 이미지 stride/plane offset을 검증 |
| **JPEG JPU 디코드 경로** | ✅ 제한 지원 | Baseline 순차형 8-bit YUV 4:2:0 VLD를 노출; 다른 샘플링은 미지원 |
| **버퍼 테이블 동시 변경** | ✅ 수정 | 버퍼 생성·해제·매핑 메타데이터 갱신을 드라이버 mutex로 직렬화 |
| **VPU dequeue 중 전역 mutex 장기 점유** | ✅ 수정 | dequeue 대기 중 전역 mutex를 풀고, 디코더별 condition variable로 sync·제출·해제 순서를 보호 |
| **VA 설정 조회 교착** | ✅ 완전 해결 | 설정 속성 반환 전에 드라이버 mutex 해제 |
| **CPU 표면 매핑 및 lock 수명** | ✅ 수정 | NV12 stride·offset·DMA-BUF 이름·CPU 포인터 제공, 대응하는 `vaUnlockSurface` 전까지 표면 재사용과 해제를 차단 |
| **오디오 언더런 연계 화면 튐** | ✅ 완전 해결 | `mpv.conf` 내 `audio-buffer=1` 최적화 |
| **HEVC (H.265) 파라미터 세트 불일치** | ✅ 검증 범위 해결 | VA 파라미터 기반으로 VPS/SPS/PPS 합성; 제한된 Main 8-bit 4:2:0 VLD만 노출하고 고급 기능은 계속 거부 |
| **HEVC 인코딩 visible-size 출력** | ⚠️ 제한 | coded 640×368에서 640×360, 638×360, 638×362, 626×354 SPS crop 하드웨어 검증 완료; 모든 기하 조합을 포괄하지는 않음 |
| **X11 데스크톱 DRI2 인증 오류** | ⚠️ 우회 완료 | DRM/GBM 직결 모드 사용; X11 환경에서는 `hwdec=vaapi-copy` 사용 |

---

## 진단 및 모니터링

재생 중 VPU 하드웨어 인터럽트 활동 확인:
```bash
watch -n 1 "cat /proc/interrupts | grep 3b000000.vpu"
```

WatchDog 실시간 텔레메트리 확인:
```bash
cat "/dev/shm/hobot_va_watchdog.$MPV_PID"
```

보드 발열 및 온도 모니터링:
```bash
cat /sys/class/thermal/thermal_zone*/temp | awk '{printf "%.1f°C\n", $1/1000}'
```

---

## 라이선스 (License)

이 프로젝트는 [MIT License](LICENSE)를 따릅니다.
