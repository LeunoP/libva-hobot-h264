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
- **인코더 비트레이트 제어**: H.264/HEVC EncSlice는 CBR, VA VBR, CQP를 지원합니다. VA VBR의 최대 비트레이트·target percentage·window를 SDK AVBR target·별도 최대 비트레이트·VBV window로 매핑합니다. 수렴 및 정확도 의미가 동등하다고 입증되지 않아 VA AVBR은 광고하지 않습니다.
- **구형 surface 속성 조회 호환성**: deprecated `vaGetSurfaceAttributes` 백엔드 콜백은 구형 libva 클라이언트에 `vaQuerySurfaceAttributes`와 같은 NV12, 메모리 유형, 외부 버퍼 및 크기 속성을 제공합니다.
- **H.264 B-프레임 처리**: VPU 내부 재정렬 설정과 128엔트리 FIFO(`submitted_surfaces`)를 사용해 제출된 표면과 출력 프레임을 대응시킵니다. 6개 연속 B-frame을 포함한 64프레임 H.264 High 샘플을 하드웨어로 디코드하고 소프트웨어 NV12 결과와 프레임별 완전 일치함을 확인했습니다. `tools/test_h264_bframe_decode.sh`로 재검증할 수 있습니다.
- **H.264 인코더 시퀀스 설정**: 첫 인코딩 프레임까지 VPU 시작을 미뤄 요청된 H.264 레벨과 시퀀스 VUI 타이밍/SAR을 초기화 전에 적용합니다. VA-API가 이 경로에서 입력 full-range 메타데이터를 제공하지 않으므로 NV12 출력을 limited-range로 명시합니다.
- **H.264/HEVC 외부 인코더 입력**: MediaCodec 외부 프레임 모드로 호환되는 연속 NV12 VA 표면을 직접 제출해 CPU 프레임 복사를 피합니다. 크롭/패딩 또는 호환되지 않는 레이아웃은 컨텍스트별 연속 hbmem scratch 버퍼를 사용하며, SDK 소비 콜백이 올 때까지 원본 표면 소유권을 유지합니다. 640x368 직접 입력과 638x360에서 640x368로 변환하는 하드웨어 경로를 확인했습니다.
- **H.264 visible 크기 표면**: 매크로블록 정렬된 coded 크기와 정확히 일치하는 visible 크기의 NV12 표면을 지원합니다. 입력을 SPS crop 위치에 배치하고 가장자리 픽셀을 복제해 coded 프레임을 채우며, coded 크기 표면은 기존 좌표 그대로 복사합니다.
- **H.264 파라미터 세트 갱신**: 합성 SPS/PPS가 변경되면 다음 픽처 전에 다시 주입하여 스트림 재협상 시 디코더 상태를 최신 상태로 유지합니다.
- **VPU 출력 무결성 판정 및 손상 프레임 DROP**: `err_mb_in_frame_display`와 SDK 디코드 결과를 함께 검사합니다. H.264/HEVC의 `FAIL`(`0x00`) 및 미정의 결과는 폐기하고, `SUCCESS_WITH_WARNING`(`0x10`)은 손상 필드가 추가 폐기를 요구하지 않는 한 표시 가능 상태로 유지하면서 폴백 watchdog에는 이상으로 전달합니다. 정상 `SUCCESS` 값은 `0x01`입니다.
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

> **HEVC 지원 범위**: 기존 출력 손상은 VA picture 파라미터와 다른 기본값으로 VPS/SPS/PPS를 합성한 문제로 확인했습니다. 현재 드라이버는 전달받은 VA 파라미터로 헤더를 합성하며, 검증된 Main 8-bit 4:2:0 범위만 노출합니다. 인코딩은 CBR/VBR/CQP, I/P 프레임, 프레임당 전체 단일 슬라이스를 지원합니다. CQP 유효 QP는 0~51이며 target bitrate는 무시하고 HRD 파라미터는 거부합니다. Main 10 및 미지원 비트스트림 기능은 계속 거부합니다.

## VA-API 범위와 제한

HEVC Main 기하 검사는 VPU 제출 전에 CTB 크기 16~64와 타일 사용 시 타일별 최소 크기 256×64 luma samples를 강제합니다.

HEVC VLD PCM 파라미터는 8-bit 4:2:0과 8~32 샘플 SPS 블록 경계를 허용합니다. 하드웨어에서는 실제 8×8, 16×16, 32×32 PCM CU와 PCM 루프 필터 활성/비활성 조합을 포함한 64×64 HM fixture 네 개를 후보 드라이버 새 VPU 세션 각각 5회 실행해 소프트웨어와 비트 단위 일치를 확인했습니다(`test_hevc_pcm_decode.sh`). 8-bit 4:2:0 이외 PCM 비트 깊이는 미검증 또는 미지원입니다.

현재 소스의 strict 빌드로 4K60 HEVC B3 스트림을 100회 반복한 별도 1회 측정에서 1,600프레임을 73.13fps로 처리했습니다. 기존 5회 세션의 73.26–73.36fps 범위에는 합치지 않았으며, 이번 실행에서는 온도를 측정하지 않았습니다.

검증된 3840×2160p60 B3 샘플은 VPU 출력 프레임을 시스템 메모리로 다운로드하며 20회 반복한 처리량 시험에서 실행당 320프레임을 처리했습니다. 프로세스 실행부터 종료까지 단조 시계(`/proc/uptime`)로 측정한 8회 결과는 66.25–67.65fps(실시간의 1.10–1.13배)였습니다. 추가 100회 반복(각 1,600프레임) 세 번에서는 72.37fps, 73.23fps, 73.26fps를 측정했습니다. 측정 구간에는 프로세스 실행과 VPU 초기화 시간이 포함되므로 짧은 반복은 고정 초기화 오버헤드의 비중이 커 장시간 반복 결과와 직접 비교할 수 없습니다. 이 결과는 해당 보드·후보 드라이버·B3 비트스트림에 한정되며 화면 표시 속도나 임의 HEVC 입력의 성능을 보장하지 않습니다.

HEVC SPS의 short-term RPS 세트가 0개 또는 1개인 VLD 입력은 inline 및 SPS 선택 참조 집합을 검증합니다. Inline 문법의 음수/양수 참조 수와 각 delta·사용 플래그를 파싱해 VA-API `st_rps_bits`, 활성 참조 수 및 선택적 list modification과 대조합니다. 단일 SPS RPS를 선택하면 VA DPB 플래그와 POC에서 현재 참조를 재구성하고 SAO 플래그, P/B 기본 리스트 순서, VA `RefPicList` 인덱스가 일치하는지 검사합니다. Long-term reference는 지원하지 않으며 다른 슬라이스 도구는 아래에 명시한 검증 패턴으로 제한됩니다.

SAO가 활성화된 640×360 단일 SPS-RPS P fixture(2프레임)는 후보 드라이버의 새 VPU 세션 5회에서 모두 소프트웨어와 비트 단위로 일치했습니다(`test_hevc_single_rps_sao_decode.sh`).

참조 1개와 음수 SPS RPS 하나를 사용하는 32프레임 x265 closed-GOP P fixture도 새 VPU 세션 5회 모두 소프트웨어와 비트 단위로 일치했습니다(`test_hevc_single_rps_decode.sh`). 반복 횟수는 `HOBOT_HEVC_SINGLE_RPS_CYCLES`로 조정할 수 있습니다.

같은 fixture를 중복 RPS 인덱스 0을 선택하는 두 SPS-RPS 형식으로 변환해도 소프트웨어 NV12 해시는 유지됩니다. SAO가 켜진 P 경로와, 두 SPS RPS 중 인덱스 0을 선택하고 두 참조 리스트 모두 직전 POC 하나만 사용하는 HM 16.20 B fixture(TMVP 비활성)는 후보 드라이버에서 각각 새 VPU 세션 5/5회 소프트웨어와 비트 단위로 일치했습니다. 이 B-SAO fixture에 L0/L1 luma offset +8 가중치 또는 양쪽 참조 리스트의 Cb/Cr offset +8 가중치를 추가한 두 조합도 각각 새 세션 5/5회 일치했습니다(`test_hevc_two_rps_sao_decode.sh`). 다른 two-RPS B-SAO 플래그와 가중치 조합은 미검증입니다. 별도로 생성한 640×360 x265 Main 6프레임 샘플은 SPS RPS 0개, inline RPS, I/P/B 슬라이스와 luma/chroma SAO를 포함하며 후보 세션 5/5회에서 소프트웨어와 비트 단위로 일치했습니다(`test_hevc_inline_rps_sao_decode.sh`).

Open-GOP 지원은 좁게 제한합니다. CRA inline short-term RPS에는 현재 참조가 없어야 하며, 미사용 RPS의 각 POC는 현재 참조가 아닌 VA DPB 표면 하나와 정확히 일치해야 합니다. 이후 RASL_N/R(NAL type 8/9)은 기존 inline-RPS 검사로 전달됩니다. 640×360 64프레임 x265 Main 스트림(B-pyramid 비활성/활성)은 각각 새 VPU 세션 5회 모두 소프트웨어와 비트 단위로 일치했고(`test_hevc_open_gop_decode.sh`), B-pyramid 활성 스트림은 RASL_R도 포함합니다. RADL, long-term reference 및 다른 CRA/RPS 배치는 미지원 또는 미검증입니다.

temporal MVP 하드웨어 검증은 좁은 예외에 한정됩니다. 단일 직전 참조를 갖는 640×360 two-SPS-RPS P 스트림은 SPS RPS 인덱스 0 선택형과 inline RPS형 모두 새 후보 VPU 세션 5회에서 비트 단위로 일치했습니다(`test_hevc_two_rps_tmvp_decode.sh`). 별도의 HM 16.20 640×360 fixture는 IDR 뒤 저지연 B 4프레임, SPS RPS 2개 중 인덱스 0 선택, L0/L1의 유일한 직전 POC 참조, SAO 및 TMVP 활성 조건에서 후보 새 VPU 세션 5회 모두 소프트웨어와 일치했습니다(`test_hevc_two_rps_b_tmvp_decode.sh`). 또 다른 8프레임 B fixture는 POC 4에서 RPS 인덱스 0의 음수 current reference 4개와 양쪽 리스트 재정렬 `[1,2,3,0]`, TMVP 및 SAO를 사용하며 새 후보 VPU 세션 5회 모두 비트 단위 일치했습니다(`test_hevc_two_rps_multiref_b_decode.sh`). 이 fixture들은 다른 B-slice TMVP 참조·collocation 및 RPS 배치까지 보장하지 않습니다.

SPS RPS 3개 중 인덱스 2를 선택하는 합성 640×360 단일 참조 P 스트림과, 최대 64개 중 인덱스 63을 선택하는 스트림은 각각 후보 드라이버 새 VPU 세션 5회 모두 소프트웨어와 비트 단위로 일치했습니다(`test_hevc_three_rps_decode.sh`). 이는 이 두 단일 참조 P 패턴만 검증합니다. 세트 3개인 inline-RPS 문법 파싱과 재작성은 단위검사에 포함되며 하드웨어 검증은 하지 않았습니다. 그 밖의 SPS RPS 개수·인덱스·참조/list 배치는 미검증입니다.

- 표면 및 이미지 경로는 8-bit NV12 / YUV420이며, 너비와 높이는 짝수여야 합니다. 일반 표면은 너비 64~8192, 높이 64~4096까지 생성할 수 있으며 컨텍스트 한도는 entrypoint별로 다릅니다. HEVC Main VLD는 최대 8192×4096, HEVC Main EncSlice는 최대 3840×2160입니다.
- H.264 VLD와 EncSlice는 Constrained Baseline, Main, High를 광고합니다. Constrained Baseline 인코딩은 SDK Baseline 프로파일을 선택하고 CAVLC를 강제하며 8x8 변환을 비활성화합니다. 검증된 Baseline SPS NAL에만 `constraint_set1_flag`를 설정하고, chroma QP 의미를 보존할 수 있는 0값 PPS 고프로파일 확장만 제거합니다. 지원 범위는 검증된 8-bit 4:2:0, CBR/VBR/CQP, I/P 프레임, 프레임당 전체 단일 슬라이스입니다. 프로파일 문법과 CBR 하드웨어 디코드는 `test_h264_encode_profiles.sh`, CQP는 아래 QP 회귀에서 검사합니다.
- H.264 디코더는 Constrained Baseline, Main, High에 맞는 SPS를 합성하며 progressive 8-bit 4:2:0과 POC type 0/2를 지원합니다. X5 VPU가 인터레이스 코딩 도구를 지원하지 않아 MBAFF와 field picture는 모든 프로파일에서 거부합니다. Monochrome 및 4:2:2/4:4:4 형식도 모든 프로파일에서 거부합니다. 코딩 매크로블록 치수는 VA 컨텍스트 경계를 매크로블록 단위로 올림한 범위까지 허용하고, VA DPB 용량 16개를 넘는 참조 프레임 수도 거부합니다. VA picture 파라미터에 POC type 1의 offset-cycle 값이 없어 해당 입력은 거부합니다. 합성 PPS가 표현하지 않는 FMO 및 redundant picture-count 모드도 거부합니다. Constrained Baseline에서는 CABAC, 가중 예측, 8x8 변환 파라미터를 추가로 거부합니다.
> HEVC 참조 리스트 수정 범위: SPS RPS가 0개인 inline 경로는 신호된 재정렬을 파싱해 VA `RefPicList`와 대조합니다. 실제 P L0 재정렬 `[1,0]`과 세 참조를 사용하는 B L0/L1 재정렬 `[1,2,0]`의 640×360 Main fixture를 각각 후보 드라이버 새 VPU 세션 5회로 검증했으며, 두 fixture 모두 8프레임 전체가 소프트웨어 출력과 비트 단위로 일치했습니다(`test_hevc_inline_refmod_decode.sh`). 두 SPS-RPS 인터 경로도 선택 RPS 인덱스 0의 특정 4-reference B 패턴에서 양쪽 리스트 재정렬 `[1,2,3,0]`을 검증합니다. 다른 RPS 인덱스와 참조/list 배치는 이 fixture로 검증되지 않았습니다.
- HEVC VLD는 Main 8-bit 4:2:0, 컨텍스트 최대 8192×4096을 지원하며 Main EncSlice는 최대 3840×2160, Level 5.1입니다. 8192×4096 Level 6.0 2프레임 1fps 샘플은 새 VPU 세션 5회 모두 비트 단위로 일치했습니다. 이 최대 해상도 저속 검증은 8K60 처리량을 뜻하지 않습니다. `HOBOT_HEVC_8K_ONLY=1`로 해당 회귀만 단독 실행할 수 있습니다. VLD는 CTU 주소가 0에서 시작해 그림 안에서 증가하는 순서의 long-format 슬라이스를 받습니다. WPP는 타일이 꺼진 경우만 허용하며, Main 프로파일은 한 PPS에서 두 도구를 동시에 켜는 것을 금지합니다. WPP entry-point 수는 그림의 CTB 행 수로 제한합니다. 균일 및 explicit 비균일 타일은 슬라이스 시작점이 타일 경계이며 슬라이스별 entry-point 문법이 타일 substream 수와 일치할 때 허용합니다. 하드웨어에서는 HM 18.0의 640×360 균일 2×2 IDR 단일 슬라이스, 비균일 2×2 P 단일 슬라이스, 그리고 타일마다 독립 슬라이스 4개를 갖는 균일 2×2 P 및 explicit 비균일 2×2 B/P 스트림을 각각 새 후보 VPU 세션 5회로 검증해 소프트웨어와 비트 단위 일치했습니다(`test_hevc_tiles_decode.sh`, `test_hevc_tiles_nonuniform_decode.sh`, `test_hevc_tiles_multislice_decode.sh`, `test_hevc_tiles_nonuniform_multislice_decode.sh`). 다른 타일 기하와 타일 내부 시작점은 미검증입니다. PPS가 허용하고 첫 segment가 아니며 직전 독립 slice의 slice type을 상속하는 dependent slice segment를 지원합니다. 하드웨어 검증은 인코더의 640×360 WPP IDR 출력 한 패턴에 한정됩니다. 별도로 640×360 단일 슬라이스 WPP 스트림도 하드웨어 검증했으며 다른 WPP 배치는 미검증입니다. Slice-local RPS bit count를 받습니다. 2~64개 SPS RPS를 선언한 inter slice는 RPS 인덱스, VA DPB 재구성, 활성 참조 리스트 및 나머지 슬라이스 헤더 검사를 통과한 뒤 synthetic SPS용 inline 형식으로 재작성됩니다. 하드웨어에서는 3세트/인덱스 2 및 64세트/인덱스 63의 단일 참조 P 패턴(각 640×360 2프레임, 5/5 세션)만 검증했습니다. 다른 개수·인덱스·참조/list 배치는 미검증입니다. 세트 3개인 inline-RPS 파싱·재작성은 단위검사만 했습니다. 기본 표준 scaling matrix는 VA IQ matrix가 표준 기본값과 정확히 일치하는 경우에만 허용하며, 비기본 scaling-list 데이터, Main 10, 검증된 8-bit 4:2:0 PCM 블록 범위 외 PCM, 별도 색 평면, long-term reference 및 불완전한 slice-data 조각은 거부합니다. `test_hevc_scaling_list_decode.sh`는 기본 행렬의 비트 단위 일치와 비기본 행렬 거부를 검사합니다.

  SPS RPS 2–64개인 인터 슬라이스는 RPS 인덱스, VA DPB 기반 참조 재구성, active-reference 목록 및 나머지 헤더를 모두 검증한 경우에만 합성 SPS용 inline 문법으로 재작성합니다. 하드웨어 검증은 3개 RPS의 인덱스 2를 선택하는 단일 참조 P 패턴(640×360 2프레임, 5/5회)에 한정됩니다. 3-set inline-RPS 파싱/재작성은 단위검사만 했습니다. 그 밖의 개수·인덱스·참조/list 배치는 미검증입니다. IDR은 DPB가 비어 있어야 합니다. 사용하지 않는 RPS 14개가 있는 2-slice 640×360 IDR은 5/5회 일치했습니다.

  기존 두-set 하드웨어 패턴은 더 좁습니다. P는 인덱스 0을 선택해 직전 POC 하나만 참조하며, B는 인덱스 0과 검증된 패턴(직전 POC를 L0/L1의 단일 참조로 사용하거나, POC 4에서 음수 current reference 4개, TMVP/SAO, 양쪽 list 재정렬 `[1,2,3,0]`)에 일치해야 합니다. Luma/chroma weighted P/B 테이블은 문법·값 범위·헤더 경계를 확인합니다. SAO P, TMVP 없는 B-SAO, 비기본 가중 B-SAO, 단일 참조 저지연 B-TMVP 및 네 참조 B를 검증했으며 다른 두-set B-SAO 조합과 weight/RPS 배치는 미검증입니다. temporal MVP, active-reference override, list modification과 각 슬라이스는 단위·하드웨어 시험의 구체 패턴에 일치해야 합니다. 그 밖의 슬라이스 도구는 거부합니다. 640×360 HM 2-slice IDR+P+P 및 IDR+BBB는 5/5회 일치했습니다. 단일 슬라이스 inline-RPS weighted P(24프레임), B3(32프레임)도 일치했습니다. 임의의 멀티슬라이스 인터 스트림은 미검증입니다. 기존 단일 슬라이스 하드웨어 시험은 640×360 P 30프레임, 1280×720@60 B3/B6 각 64프레임, 1920×1080@60 B6 64프레임, 3840×2160@60 B3 16프레임에서 일치했습니다. 스트림의 60fps 타임스탬프만으로 처리량을 단정할 수 없습니다. 16프레임 4K B3 샘플을 20회 반복한 여섯 세션은 65.04~67.09fps였습니다. 이어서 새 세션 5개에서 각각 100회 반복(1,600프레임)한 결과는 73.26~73.36fps였고, 약 2분 동안 CPU/DDR 온도는 약 4.3°C 올랐습니다. 두 시험 모두 하드웨어 프레임 다운로드를 포함합니다. 이 짧은 측정은 장시간 열 안정성이나 화면 표시 성능을 입증하지 않습니다.
- HEVC inline-RPS 경로는 활성 L0 참조 8개까지 검증했습니다. Wave521은 이 경우 마지막 프레임까지 출력하려면 디코더 frame buffer 17개가 필요합니다. SPS DPB 크기 9인 Level 4.1, 64프레임 스트림을 `test_hevc_main_decode.sh`에서 새 VPU 세션 5회로 확인합니다.
- temporal-MVP 범위: 하드웨어 검증은 위 두 P 패턴과 두 개의 구체적인 B 패턴으로 한정됩니다. 단일 참조 B fixture는 양쪽 리스트에 직전 참조 하나, `collocated_from_l0_flag=0`, SAO를 사용합니다. 네 참조 fixture도 명시된 RPS/list 조합만 검증하며 임의의 TMVP 참조·collocation 배치를 보장하지 않습니다.
- HEVC 두 SPS RPS 세트의 weighted P/B 문법은 단위 및 값 범위 검사에 포함됩니다. 두 SPS RPS와 중립, 비중립 luma offset, Cb/Cr offset +8 chroma 테이블을 결합한 2-slice HM 스트림은 후보 드라이버에서 P 패턴 3프레임과 B 패턴 4프레임 모두 소프트웨어와 비트 단위로 일치했고, 각 사례를 5회 반복 통과했습니다. 별도로 단일 참조 B에서 SAO와 L0/L1 luma offset +8 가중치를 결합한 2프레임 스트림도 5회 일치했습니다. 다른 가중치 값과 two-RPS B-SAO 플래그 조합은 미검증입니다. 실제 참조 리스트 재정렬은 위에서 명시한 fixture 패턴만 검증했습니다. 단일 참조인 경우 재정렬 문법이 생기지 않으면 PPS capability flag만 설정된 입력을 허용합니다. 640×360 IDR+BBB 2-slice HM 스트림은 후보 드라이버에서 새 VPU 세션 5회 모두 소프트웨어와 비트 단위로 일치했습니다. 임의의 멀티슬라이스 인터 스트림을 의미하지 않습니다.
- HEVC EncSlice는 Main 8-bit 4:2:0, CBR/VBR/CQP, I/P 프레임, VAAPI로 제출하는 프레임당 전체 단일 슬라이스로 제한됩니다. CQP는 그림별 유효 QP 0~51을 사용하고 target bitrate를 무시하며 HRD 파라미터를 지원하지 않습니다. `entropy_coding_sync_enabled_flag`는 VPU 시작 전 SDK의 고정 `wpp_enable` 설정으로 전달되며, 한 컨텍스트에서 변경할 수 없습니다. 640×360 IDR 뒤 단일 참조 P를 잇는 I/P WPP 스트림은 각 프레임에서 dependent 행 segment 5개를 출력했고, 5회 새 세션 모두 소프트웨어 및 하드웨어 재디코드가 비트 단위로 일치했습니다. B 프레임 WPP와 다른 WPP 해상도는 미검증입니다. 인코더 초기화 시 SDK의 HEVC SAO 제어를 명시적으로 0으로 설정하며 API 호출 실패 시 초기화를 중단합니다. SDK 헤더는 이 제어를 인코딩 지원·디코딩 미지원으로 표시합니다. 이는 VLD 비트스트림 디코드 지원과 별개입니다. SDK의 HEVC crop 설정은 SPS를 바꾸지 않아 오른쪽/아래 크롭에는 coded 크기를 보존하는 표준 SPS conformance-window 문법을 추가합니다. 각 크롭은 16픽셀 미만이고 4:2:0 crop 단위에 맞아야 하며 CTU 격자가 같고 coded buffer에 추가 문법을 넣을 공간도 있어야 합니다. coded 640×368에서 visible 640×360, 638×360, 638×362, 626×354의 64프레임 Level 4.1 스트림은 모두 하드웨어 재디코드가 소프트웨어와 비트 단위로 일치했습니다. 3840×2160 Level 5.1 무-B CBR 16프레임은 30fps와 60fps 설정 모두 인코딩 후 하드웨어 재디코드가 비트 단위로 일치했습니다. 이는 짧은 출력 정확도 시험이며 지속 4K60 처리량이나 모든 기하 조합을 입증하지 않습니다.
- H.264 VLD 슬라이스 파라미터는 `slice_data_offset`, `slice_data_size`, `ALL/BEGIN/MIDDLE/END` 플래그를 반영합니다. 여러 요소를 가진 파라미터 버퍼 1개와 데이터 버퍼 1개, 또는 요소 1개씩인 동일 개수의 순서 대응 버퍼 쌍을 지원하며, 모호한 조합과 범위 밖 데이터는 거부합니다. 640×360 High, 그림당 2슬라이스, 6연속 B-frame인 64프레임 스트림을 새 VPU 세션 5회 모두 소프트웨어와 비트 단위로 일치시켰습니다.
- H.264 인코딩은 CBR/VBR/CQP를 지원하며 프레임당 전체 I/P 슬라이스 하나로 제한됩니다(최대 슬라이스 수 1). CQP의 유효 QP는 `pic_init_qp + slice_qp_delta`이며 0~51이어야 합니다. CQP에서는 target bitrate를 무시하고 HRD 파라미터는 거부합니다. Constrained Baseline/Main/High 모두 직접 VA 요청 QP 0의 크롭 all-intra 출력과 QP 1/26/37/51의 정렬·크롭·30000/1001fps 인코드 및 전 프레임 소프트웨어 해시 일치를 확인했습니다. HEVC Main도 CBR/VBR/CQP I/P 인코딩을 지원합니다. 직접 VA 시험에서 유효 QP 경계, 그림별 I/P QP, WPP와 잘못된 QP 거부를 확인했고, FFmpeg QP 1/26/51 출력은 하드웨어/소프트웨어 재디코드가 일치했습니다. HEVC CQP 역시 target bitrate를 무시하고 HRD 파라미터를 거부합니다. H.264/HEVC는 MediaCodec 외부 프레임 입력을 사용합니다. 호환되는 연속 NV12 VA 표면은 직접 제출하고, 크롭/패딩 또는 호환되지 않는 레이아웃은 컨텍스트별 hbmem scratch 버퍼로 스테이징합니다. SDK 입력 소비 콜백이 올 때까지 원본 표면 소유권을 유지합니다. RDK-X5 [MediaCodec 문서](https://developer.d-robotics.cc/x5_sdk_doc_v2.0.0/en/multimedia_development/10-MediaCodec_API_zh_CN.html)는 GOP preset 1과 9만 지원한다고 명시하므로 `ip_period`는 1이어야 하고 `max_num_ref_frames`는 1을 넘을 수 없습니다. 참조 프레임 수가 0이면 all-intra GOP preset 1을 선택하고 I-slice만 허용하며, 참조 프레임 1개면 단일 참조 I/P 패턴용 preset 9를 선택합니다. H.264/HEVC 인코더 설정은 이전 프레임 방향 예측만 광고합니다. B/SP/SI 슬라이스, 여러 슬라이스, 부분 프레임 슬라이스, 매크로블록 맵과 미지원 인코더 버퍼/misc 파라미터는 거부합니다. VA HRD/VBV 버퍼 크기는 SDK의 10~3000ms VBV 시간 창으로 변환합니다. 설치된 SDK에는 VA 초기 CPB fullness 제어 항목이 없어 해당 값은 적용하지 못합니다. H.264 레벨과 지원되는 시퀀스 VUI 항목은 VPU 시작 전에 고정되며 변경하려면 새 VA 컨텍스트가 필요합니다. JPEG VLD는 Baseline 순차형 8-bit 3성분 YUV 4:2:0 및 단일 전체 프레임 스캔을 지원하며 VA 컨텍스트마다 회전값 0/90/180/270도를 고정합니다. 90/270도에서는 출력 표면의 가로·세로를 입력과 바꾸며, JPU 메타데이터에는 코딩 크기가 보고되므로 회전된 NV12 plane 레이아웃을 별도로 검증합니다. 그 밖의 샘플링, progressive/multi-scan 입력과 미지원 VA 필드는 거부합니다. 네 회전값 모두 소프트웨어 NV12 기준과 비교해 각 5회 반복 검증했습니다. JPEG 인코딩은 Baseline 순차형 8-bit 3성분, 프레임별 품질값(1~100), 전달된 양자화표와 restart interval을 지원합니다. 사용자 지정 Huffman 표 등 미지원 인코더 설정은 거부합니다. `VA_ATTRIB_NOT_SUPPORTED` 속성은 지원되지 않습니다.
- DMA-BUF 표면 내보내기는 DRM PRIME 2를 지원합니다. 가져오기는 현재 프로세스에서 HBmem 메타데이터를 확인할 수 있는 경계 검증된 선형 단일 객체 NV12 descriptor만 허용합니다. 다중 객체, 분리 레이어, 비선형 modifier 또는 메타데이터를 확인할 수 없는 DMA-BUF 레이아웃은 거부합니다. 검증되지 않은 다중-FD 디코더 출력과 레거시 DRM PRIME 가져오기도 거부합니다.
- `vaSyncSurface2`는 드라이버 mutex 획득부터 디코더 대기까지 단조 시계 deadline을 적용하며, 출력 dequeue/recycle 호출도 남은 시간으로 제한합니다. 시간 초과 시 진행 중인 디코드와 버퍼 소유권을 보존해 재시도할 수 있고, `VA_TIMEOUT_INFINITE`는 기존 `vaSyncSurface` 경로를 사용합니다.
- `vaSyncBuffer`는 버퍼 ID를 검증하고 timeout에 드라이버 mutex 획득 시간을 포함합니다. VA 입력 버퍼는 `vaRenderPicture`에서 동기적으로 소비되며, 인코딩 출력은 성공한 `vaEndPicture` 반환 전에 드라이버 소유 coded buffer로 복사됩니다.
- `vaDeriveImage`는 직접 CPU 매핑 가능한 NV12 backing을 제공하는 경우에만 성공하며 staging 복사본을 만들지 않습니다. 검증된 연속 단일-FD 레이아웃만 허용하고, `vaMapBuffer`/`vaUnmapBuffer`와 `vaLockSurface`/`vaUnlockSurface`의 CPU 접근 구간에서 DMA 캐시 무효화·flush를 수행합니다. 캐시 flush 실패 상태는 이미지 버퍼가 아니라 surface 단위로 보존·재시도하며, CPU 매핑 중 export와 surface 복사는 busy로 거부됩니다. 파생 이미지가 살아 있는 동안 원본 표면은 잠기며, 원본 표면을 대상으로 하는 `vaGetImage`/`vaPutImage`는 `VA_STATUS_ERROR_SURFACE_BUSY`를 반환합니다. 다른 표면과의 복사는 지원합니다.
- `vaQuerySurfaceError` 상세 조회는 구현되지 않았습니다. Wave521 SDK는 손상 매크로블록 개수와 오류 원인은 제공하지만 `VASurfaceDecodeMBErrors`에 필요한 오류 매크로블록의 시작/끝 주소는 제공하지 않습니다. 따라서 위치를 임의로 만들어 보고하지 않으며, 실패 프레임은 `vaSyncSurface`가 `VA_STATUS_ERROR_DECODING_ERROR`로 알립니다. Subpicture, display attribute 조작, image palette, `vaPutSurface`도 구현되지 않았으며 호출 시 성공을 가장하지 않고 오류를 반환합니다. `vaPutImage`는 CPU NV12 staging 경로에서 동작하며 하드웨어 블릿을 제공하지 않습니다.
- `vaAcquireBufferHandle`/`vaReleaseBufferHandle`은 검증된 연속 단일-FD DMA-BUF를 backing으로 사용하는 파생 NV12 이미지에 한해 DRM PRIME FD 수명을 지원합니다. acquire 시 복제된 FD는 release까지 유효하고, acquire 중에는 충돌하는 표면 작업을 막습니다. 일반 malloc 버퍼와 검증되지 않은 레이아웃 또는 다중-FD 레이아웃은 거부합니다. multi-frame context, processing-rate query, 일반 `vaCopy` VA backend 확장은 여전히 구현되지 않았습니다. `VAEncCodedBufferType`의 `vaBufferSetNumElements`도 지원하지 않으며 coded-buffer 용량은 `vaCreateBuffer`에서 고정됩니다.
- 단위 테스트와 빌드 성공은 실제 보드의 재생·인코딩 성능 검증을 대체하지 않습니다. `/dev/dri`와 Wave521 하드웨어가 있는 대상 보드에서 스트림 회귀 테스트를 별도로 수행해야 합니다. 헤드리스 VA 테스트는 display DRM master 점유를 피하도록 기본 `/dev/dri/renderD128`을 사용하며, `HOBOT_DRM_DEVICE`로 바꿀 수 있습니다. KMS가 필요한 화면 출력/DirectVIV 테스트는 `/dev/dri/card0`을 계속 사용합니다.
- `tools/test_va_surface_state.c`는 멀티미디어 호출을 모의해 버퍼 소유권/teardown, PRIME2 import, 외부 인코더 소비 콜백, watchdog 격리, 디코드 결과 FAIL/미정의/경고 처리, API 경계, 인코더와 JPEG 설정, H.264 참조/GOP/DPB/slice 문법, HEVC Main 허용 범위, 2–64-set SPS-RPS 인덱스 검사와 DPB 기반 inline 재작성, inline RPS 및 DPB 참조 재구성/P·B 목록/SAO, 가중 P/B 헤더 경계, HEVC 슬라이스 순서, 동시성·제한 시간, PRIME export와 NV12 plane 경계를 검사합니다. 이는 하드웨어 회귀를 대체하지 않습니다.
- `tools/test_va_surface_export.c`는 VA-API로 입력 전체를 디코드하고 각 VPU 프레임을 1ms `vaSyncSurface2` timeout으로 동기화합니다. timeout인 경우 무한 timeout으로 재시도한 뒤 NV12/separate-layer DRM PRIME 2 export와 plane/객체 경계를 검증하고, 각 프레임의 파생 이미지 DRM PRIME buffer-handle acquire/release를 검사합니다. `HOBOT_DRM_DEVICE`로 DRM 장치를 선택할 수 있습니다.
- `tools/test_va_surface_import.c`는 새 NV12 VA 표면과 PRIME2 재수입 표면에서 파생 이미지 DRM PRIME buffer-handle acquire/release, FD 닫힘, acquire 중 표면 잠금을 검사합니다. 테스트 패턴을 기록하고 composed-layer DRM PRIME 2로 재수입한 뒤 Y/UV 데이터와 물리 레이아웃을 비교하며, export FD와 원본 표면을 닫은 뒤에도 데이터가 유지되는지 확인합니다. 남은 imported 표면의 `vaPutImage` 패치로 수정된 Y/UV와 주변 픽셀 보존도 검사합니다.
- `tools/test_va_jpeg_encode.c`는 품질값(1~100)을 picture parameter로 전달해 JPEG Baseline 하드웨어 인코딩을 수행하고, coded buffer에 0 및 무한 timeout `vaSyncBuffer`를 호출한 뒤 결과 파일을 저장합니다. 서로 다른 품질값으로 실행해 크기 차이와 디코드 가능성을 확인할 수 있습니다.
- `tools/test_va_jpeg_decode.sh`는 Baseline 4:2:0 JPEG를 생성해 한 VA 컨텍스트에서 반복 디코드하고, 출력 NV12를 full-range 소프트웨어 기준과 PSNR 비교합니다. 기본 5회이며 `HOBOT_JPEG_DECODE_CYCLES`로 횟수를 바꿀 수 있고, 하드웨어 디코드 제한 시간은 기본 20초이며 `HOBOT_JPEG_DECODE_TIMEOUT`으로 조정합니다.
- `tools/test_va_jpeg_rotation.sh`는 비대칭 Baseline JPEG를 0/90/180/270도로 디코드하고 회전된 출력 크기와 NV12 레이아웃을 확인한 뒤 소프트웨어 회전 기준과 PSNR 비교합니다. 기본 각도별 5회이며 `HOBOT_JPEG_ROTATION_CYCLES`로 횟수를 바꿀 수 있습니다. 후보 드라이버는 스크립트 인수로 전달합니다.
- `tools/test_hevc_main_decode.sh`는 HEVC Main 8-bit 4:2:0 스트림을 640×360, 720p60, 1080p60 Level 5.0, 4K60 Level 5.1 및 8192×4096 Level 6.0(1fps)으로 생성합니다. 추가로 SPS DPB/RPS/L0 active count를 검사하는 Level 4.1 64프레임 8-reference 케이스를 기본 5회 실행하며 `HOBOT_HEVC_REF8_CYCLES`로 횟수를 바꿀 수 있습니다. `HOBOT_HEVC_REF8_ONLY=1`은 8-reference 회귀만 실행합니다. 8K 샘플도 기본 5개 새 VPU 세션에서 실행하며 `HOBOT_HEVC_8K_CYCLES`로 횟수를 바꿀 수 있습니다. 이는 8K60 처리량 시험이 아닙니다. P/B-frame 패턴과 레벨을 확인하고, non-default luma offset이 실제 포함된 no-B weighted-P 및 B3 weighted-bipred 케이스도 하드웨어 NV12 프레임과 소프트웨어 결과를 `framemd5`로 비교합니다. `HOBOT_HEVC_WEIGHTED_ONLY=1`은 두 가중 예측 회귀만, `HOBOT_HEVC_WEIGHTED_P_ONLY=1`은 P 회귀만 실행합니다.
- `tools/test_vpu_concurrent_decode.sh <h264.mp4> <hevc.mp4> [프레임 수]`는 H.264와 HEVC 디코더를 별도 프로세스에서 동시에 실행하고 각 하드웨어 NV12 프레임을 소프트웨어 결과와 비교합니다. 기본 64프레임이며, 두 입력 모두 8-bit `yuv420p`이어야 합니다. 제한 시간은 `HOBOT_CONCURRENT_DECODE_TIMEOUT`으로 설정하며 로그와 해시는 `/tmp`에 보존합니다.
- `tools/test_vpu_multicontext_decode.sh <h264.mp4> <hevc.mp4> [프레임 수]`는 두 스트림을 하나의 컨테이너로 묶어 단일 FFmpeg 프로세스에서 두 VA decoder context를 실행하고, 각각의 하드웨어 NV12 결과를 소프트웨어 기준과 비교합니다. `HOBOT_MULTICONTEXT_DECODE_CYCLES`로 반복 횟수, `HOBOT_MULTICONTEXT_DECODE_TIMEOUT`으로 회차별 제한 시간을 설정합니다.
- `tools/test_hevc_multislice_b_decode.sh`는 640×360 HM 샘플의 IDR+BBB 저지연 B 패턴과 그림당 2슬라이스를 확인합니다. B의 L0/L1은 바로 이전 POC 하나를 공통 참조로 사용하며, 기본 5개 새 VPU 세션에서 모든 NV12 프레임을 소프트웨어와 비교합니다.
- `tools/test_hevc_three_slice_decode.sh`는 HM 16.20 640×360 Main fixture의 IDR+P 4개 그림에서 각 그림의 독립 슬라이스 3개(CTU 0/20/40), WPP 비활성화를 확인합니다. 후보 드라이버의 전체 NV12 프레임을 소프트웨어와 비트 단위로 비교하며 기본 5개 새 VPU 세션에서 통과했습니다. 임의의 멀티슬라이스 인터 스트림 지원을 뜻하지는 않습니다.
- `tools/test_hevc_wpp_encode.sh`는 직접 VAAPI 클라이언트로 640×360 HEVC Main WPP IDR/P 쌍을 인코딩하고, `trace_headers`에서 I/P 순서와 각 그림의 dependent 행 segment를 확인한 뒤 소프트웨어와 후보 드라이버의 하드웨어 디코드 해시를 비교합니다. 단일 참조 P 패턴은 새 세션 5회 통과했습니다. B 프레임 WPP와 다른 해상도는 미검증입니다.
- `tools/test_hevc_wpp_decode.sh`는 타일을 끄고 WPP를 켠 640×360 HEVC Main 스트림 32프레임을 생성해 `trace_headers`로 entry-point 문법을 확인합니다. 후보 드라이버의 전체 NV12 결과를 소프트웨어와 비교하며 기본 5개 새 VPU 세션으로 반복합니다. 다른 WPP 배치는 검증하지 않았습니다.
- `tools/test_hevc_tiles_decode.sh`는 HM 18.0의 640×360 HEVC Main 균일 2×2 타일 IDR fixture를 확인하고 후보 드라이버의 NV12 출력이 소프트웨어와 비트 단위로 같은지 기본 5개 새 VPU 세션에서 검사합니다. 이 단일 슬라이스 레이아웃만 검증했으며 비균일 타일, 다중 슬라이스 및 WPP+타일은 미지원 또는 미검증입니다.
- `tools/test_hevc_tiles_nonuniform_decode.sh`는 HM 18.0의 640×360, 8프레임 P 스트림에서 explicit 비균일 2×2 타일(CTB 열 6/4, 행 4/2)과 3개 entry point를 확인하고 후보 드라이버 NV12 출력을 소프트웨어와 비교합니다. 기본 5개 새 VPU 세션 모두 비트 단위로 일치했습니다. 다른 배치는 미검증입니다.
- `tools/test_hevc_two_rps_weighted_decode.sh`는 기존 2-RPS HM 샘플에 중립, 비중립 luma offset, Cb/Cr offset +8 weighted P/B 테이블을 넣고 필드를 검증합니다. 중립 테이블은 변환 전후 소프트웨어 출력이 같고, luma/chroma offset 테이블은 출력이 실제로 바뀌는지 확인한 뒤 후보 드라이버와 비교합니다(P/B 패턴 각각 3/4프레임). 기본 5회 반복이며 `HOBOT_HEVC_TWO_RPS_WEIGHTED_CYCLES`로 횟수를 바꿀 수 있습니다. 다른 weight/RPS 조합은 미검증입니다.
- `tools/test_hevc_two_rps_tmvp_decode.sh`는 temporal MVP가 켜진 2-SPS-RPS P 스트림의 선택형 RPS 인덱스 0과 inline RPS 변형을 만들고, 문법·소프트웨어 결과를 확인해 각 형식을 후보 드라이버 새 VPU 세션 5회와 비교합니다.
- `tools/test_hevc_two_rps_b_tmvp_decode.sh`는 HM 16.20의 IDR+저지연 B 4프레임 640×360 fixture를 사용합니다. RPS 인덱스 0, 양쪽 리스트의 단일 직전 참조, TMVP와 SAO 활성화를 확인하고 새 VPU 세션 5회에서 모든 NV12 프레임을 소프트웨어와 비트 단위 비교합니다. 다른 B-slice TMVP 참조/collocation 배치는 검증 범위가 아닙니다.
- `tools/test_hevc_two_rps_multiref_b_decode.sh`는 8프레임 fixture의 POC 4 B 슬라이스가 선택 RPS 인덱스 0에서 음수 current reference 4개와 L0/L1 재정렬 `[1,2,3,0]`, TMVP 및 SAO를 사용하는지 확인합니다. 전체 NV12 출력을 후보 드라이버 새 VPU 세션 5회에서 소프트웨어와 비트 단위 비교합니다. 다른 RPS 인덱스와 참조 배치는 검증 범위가 아닙니다.
- `tools/test_hevc_three_rps_decode.sh`는 FFmpeg/libx265로 640×360 2프레임 단일 참조 P 스트림 두 개를 만들며, 하나는 SPS RPS 3개 중 인덱스 2를 선택하고 다른 하나는 최대 64개 중 인덱스 63을 선택합니다. `trace_headers`와 ffprobe로 문법·프로파일·크기·프레임 수를 확인하고, 변환 전후 소프트웨어 NV12 해시가 같은지 검사한 뒤 각 후보 드라이버 출력을 기본 5회 비교합니다. 하드웨어 검증 범위는 이 두 패턴뿐입니다. 횟수는 `HOBOT_HEVC_THREE_RPS_CYCLES`로 바꿀 수 있습니다.
- `tools/test_hevc_main_encode.sh`는 VAAPI로 HEVC Main 무-B 스트림 64프레임(CBR 기본, VBR/CQP 선택 가능)을 인코딩하고 profile/level/visible·coded 크기/프레임 수, SPS의 SAO 비활성화와 crop offset을 확인하며 모든 하드웨어 재디코드 프레임을 소프트웨어와 비교합니다. 인코더는 초기화 시 SDK HEVC SAO 제어를 명시적으로 0으로 설정합니다. SDK 헤더는 이 제어를 인코딩 지원·디코딩 미지원으로 표시하며, 이는 위 VLD SAO 시험과 별개입니다. 640×360, 638×360, 638×362, 626×354에서 각각 64프레임 재디코드가 일치했고, 최대 14픽셀 크롭까지 확인했습니다. 별도로 3840×2160 Level 5.1 무-B 16프레임 스트림도 30fps와 60fps 설정에서 재디코드가 일치했습니다. 이는 짧은 출력 정확도 시험이며 지속 4K60 인코딩 성능은 확인되지 않았습니다. 다른 크롭 형식도 미검증입니다.
- `tools/test_hevc_cqp_encode.sh`는 직접 VA 호출로 HEVC Main CQP 유효 QP 경계, 그림별 I/P QP와 slice QP delta, WPP 출력, 비트스트림 QP, 잘못된 QP 거부, 소프트웨어/하드웨어 재디코드 일치를 검사합니다. 유효 QP 0/37/51 조합과 WPP I=26/P=37을 확인했으며 임의 스트림이나 지속 처리량을 입증하지는 않습니다.
- `tools/test_vbr_encode.sh`는 H.264/HEVC VA VBR 인코딩, HEVC 프레임별 target/max/window 갱신, 스트림 메타데이터와 하드웨어/소프트웨어 재디코드 일치를 확인합니다. 임의 영상에서 target bitrate 정확도를 보장하는 시험은 아닙니다.
- `tools/test_h264_bframe_decode.sh`는 6개 이상 연속 B-frame을 가진 64프레임 H.264 High 샘플을 생성하고, ffprobe로 패턴을 확인한 뒤 하드웨어 NV12 디코드 결과를 소프트웨어 결과와 프레임별 해시 비교합니다.
- `tools/test_h264_encode_profiles.sh`는 Constrained Baseline, Main, High H.264 스트림을 각각 64프레임 인코딩하고 프로파일·레벨·픽셀 형식을 검사합니다. Constrained Baseline은 `trace_headers`로 constraint 플래그, CAVLC, 고프로파일 PPS 확장 문법의 부재 및 가중 예측 비활성화를 확인하며, 세 프로파일 모두 모든 하드웨어 재디코드 프레임을 소프트웨어와 해시 비교합니다.
- `tools/test_h264_encode.sh`는 정렬된 720p60, coded 640x368 안의 visible 640x360, 30000/1001fps H.264 VAAPI 인코딩을 반복합니다. `HOBOT_H264_ENCODE_PROFILE`은 `constrained_baseline`, `main`, `high` 중 하나를 선택하며 기본값은 `high`입니다. 기본 CBR이며 `HOBOT_H264_ENCODE_RC_MODE=VBR` 또는 `CQP`로 모드를 선택하고 `HOBOT_H264_ENCODE_QP`로 QP 1~51을 지정합니다(기본 26). FFmpeg의 `-qp 0`이 VA에 기본 QP를 전달하므로 QP 0은 직접 VA 클라이언트를 사용하는 아래 크롭 회귀에서 `HOBOT_H264_CROP_ENCODE_QP=0`으로 검사합니다. CQP 출력의 `pic_init_qp + slice_qp_delta`가 요청 QP와 같은지 검사하고 모든 하드웨어 디코드 프레임을 소프트웨어와 해시 비교합니다. 기본 3회이며 `HOBOT_H264_ENCODE_CYCLES`로 조정할 수 있습니다.
- `tools/test_h264_crop_encode.sh`는 참조 프레임 0개인 638x360 VA 표면을 640x368 H.264 context에 넣어 Constrained Baseline, Main, High 프로파일로 각각 같은 VPU context에서 3프레임 연속 인코딩합니다. SPS의 좌·상단 offset, 참조 수 0, 세 I-picture를 확인한 뒤 모든 visible 프레임을 원본 NV12와 PSNR 비교합니다. `HOBOT_H264_CROP_ENCODE_QP=0..51`을 지정하면 직접 VA CQP 파라미터를 제출하고 출력 QP도 검사합니다.

> Hobot codec SDK의 `mc_video_frame_buffer_info_t.vstride`는 세로 라인 수가 아니라 크로마 바이트 pitch입니다. 드라이버는 이를 공개 `hobot_surface_info.vstride`(정렬된 세로 라인 수)와 분리해 처리합니다.

### 이전 후보 검증 (2026-10-08)

후보 드라이버 SHA-256 `ff7dee2bf5b64ff4a61f1ac7f652d679a0b1f43010256ee41e641818b7e7059d`에서 `tools/test_*.sh` 하드웨어 회귀 스크립트 37개를 모두 통과했습니다. 여기에는 HEVC Main 8192×4096 Level 6.0 2프레임 디코드 5/5회, 단일 프로세스 H.264/HEVC 동시 VA context 디코드(스트림별 64프레임, 3/3회), 별도 프로세스 동시 디코드(스트림별 64프레임), HEVC 독립 슬라이스 3개 IDR+P(새 VPU 세션 5회), H.264 reference B-slice B-pyramid 64프레임, H.264 Baseline/Main/High 및 HEVC Main 하드웨어 인코드·재디코드, JPEG 품질 90 인코드·FFmpeg 디코드, JPU 회전 4방향(각 5회, 76.14 dB PSNR)이 포함됩니다. 별도 PRIME2 export 시험은 3840×2160p60 HEVC B3 16프레임의 NV12 두 export 레이아웃, plane/object 경계 및 `vaSyncSurface2` timeout 8회 재시도를 확인했습니다. 4K60 HEVC B3 20회 반복 시험은 최신 실행에서 320프레임을 66.53fps로 처리했으며, 이 후보의 네 실행 범위는 66.53–67.37fps였습니다. 상태 회귀 테스트는 AddressSanitizer와 UndefinedBehaviorSanitizer에서도 통과했습니다. GCC `-fanalyzer`가 드라이버 소유 구조체에 보관되는 `coded_segment.buf`, `surface.raw_data` 두 할당을 누수로 경고했으나, 두 항목 모두 버퍼/표면 정리 경로에서 해제됨을 추적 확인했습니다. 검증 구간의 커널 journal에는 warning/error 항목이 없었습니다. 시험은 제한된 fixture와 짧은 실행이므로 임의 스트림이나 장시간 열·화면 출력 성능을 보장하지 않습니다. 후보는 `/tmp`에서 빌드·시험했으며 시스템에 설치하지 않았습니다.

### 현재 드라이버 검증 (2026-10-08)

설치 후보 SHA-256 `e3138675c740ed95d18a6a1513e1cb0bd36e9ce54fb1eaae6fe14e13fee4fec9`는 후보 경로에서 `tools/test_*.sh` 하드웨어 회귀 39개를 모두 통과했습니다. H.264/HEVC CBR·VBR·CQP 인코드와 하드웨어/소프트웨어 재디코드, HEVC 프레임별 VBR target/max/window 갱신, JPEG 회전, 동시·다중 컨텍스트 디코드가 포함됩니다. 4K60 처리량 시험은 320프레임에서 67.65fps였습니다. `test_va_config`와 `test_va_surface_state`도 통과했으며, state 시험은 VBR config 허용과 VA AVBR 거부를 확인합니다. 같은 SHA의 후보를 `/usr/lib/aarch64-linux-gnu/dri/hobot_drv_video.so`에 원자적으로 설치한 뒤 설치 경로에서 capability 검사와 전체 VBR 인코드/재디코드 회귀를 다시 통과했습니다. 로그와 산출물은 `/mnt/data/hobot-vpu-verify/vbr-20261008/`에 보존했습니다. 시험 구간의 커널 journal에는 warning 이상 항목이 없었습니다.

교체 전 드라이버는 `/var/backups/hobot_drv_video.so.20261008-pre-vbr`에 보존했습니다(SHA-256 `24c737351ef95e15d18a6a1513e1cb0bd36e9ce54fb1eaae6fe14e13fee4fec9`). 검증은 대상 fixture와 제한된 하드웨어 실행에 한정되며 임의 스트림이나 장시간 열·화면 출력 성능을 보장하지 않습니다.

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
- **DRM/GBM (하드웨어 DirectVIV 직결)**: `test_va_directviv_bench.c`는 입력 영상과 같은 크기의 GBM 표면/viewport에 VPU NV12 표면을 CPU 프레임 복사 없이 렌더링하고 EOF 지연 프레임과 출력 프레임 수를 확인합니다. 2026-10-07 후보 드라이버에서 1920×1080p60 H.264 High B6 64프레임은 92.43 FPS로 렌더링됐습니다. 선택적 5점 framebuffer readback도 비검정/색상 변화 검사를 통과했습니다(91.63 FPS). 640×360 B6 샘플도 640×360 타깃에 64프레임을 렌더링하고 같은 스모크 검사를 통과했습니다. 0.16초 실행은 처리량 측정으로 취급하지 않습니다. 이 검사는 소프트웨어와 픽셀 단위 일치나 실제 디스플레이 표시를 입증하지 않습니다. 앞서 기록된 185.90/111.91 FPS는 64×64 GBM 표면에 1920×1080 viewport를 설정한 측정이라 전체 프레임 결과로는 철회합니다. 실제 키오스크 경로의 Moonlight에서는 60fps 스트림이 약 30 표시 FPS로 관측됐습니다. Vsync, 합성기 정책, 디스플레이 모드는 VPU 디코드 처리량과 별개입니다.
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
   `libva-hobot`은 `error_reason`이 마스킹되어도 SDK 디코드 결과를 확인하고 `err_mb_in_frame_display`도 검사합니다. 실패 및 미정의 결과는 폐기하며, `SUCCESS_WITH_WARNING`은 폴백 watchdog에 전달하되 손상 필드가 허용하면 표시할 수 있습니다:
   ```c
   if ((decode_result != HOBOT_DECODE_RESULT_SUCCESS &&
        decode_result != HOBOT_DECODE_RESULT_SUCCESS_WITH_WARNING) ||
       err_mb > 0 || (err_reason & 0x00020000)) {
       fprintf(stderr, "[HOBOT-VA][DROP] Dropping failed/corrupt frame\n");
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
| **JPEG JPU 디코드 경로** | ✅ 제한 지원 | Baseline 순차형 8-bit YUV 4:2:0 VLD 및 컨텍스트 고정 0/90/180/270도 회전; 다른 샘플링은 미지원 |
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
