# HEVC test fixtures

These checked-in streams are inputs for the hardware regressions in `../`. The tests validate only the syntax and reference patterns present in each fixture; they do not establish general support for the corresponding HEVC tool.

| Fixture group | Files | Regression |
|---|---|---|
| Inline RPS and list modification | `hevc_main_640x360_inline_rps_refmod_{p,b}.hevc` | `test_hevc_inline_refmod_decode.sh` |
| Single-/multi-SPS RPS, SAO and TMVP | `hevc_main_640x360_single_sps_rps*`, `hevc_main_640x360_two_slices_single_sps_rps_p3.hevc`, `hevc_main_640x360_two_sps_rps_*.hevc` | `test_hevc_single_rps_*.sh`, `test_hevc_two_rps_*.sh` |
| Independent slices | `hevc_main_640x360_two_slices_*.hevc`, `hevc_main_640x360_three_slices_idr_p_p_p.hevc` | `test_hevc_multislice*.sh`, `test_hevc_three_slice_decode.sh` |
| Tiles | `hevc_main_640x360_*tiles*.hevc` | `test_hevc_tiles*.sh` |
| PCM | `hevc_main_64x64_pcm_*.hevc` | `test_hevc_pcm_decode.sh` |

Some streams were produced with test-only HM encoder changes. Those encoder patches and source inputs are not included. See the regression scripts for syntax checks, fixture hashes, and exact hardware-test conditions.
