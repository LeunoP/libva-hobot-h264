# HEVC Decode Fixtures

`hevc_main_640x360_single_sps_rps_sao_p.hevc` is a two-picture, 640x360, 8-bit HEVC Main
stream with SAO enabled and exactly one negative SPS short-term RPS. The P slice selects that
SPS RPS and enables luma and chroma SAO. It exercises the single-RPS slice-header rewrite with
the inserted RPS index before the SAO fields; `../test_hevc_single_rps_sao_decode.sh` checks the
syntax and compares candidate-driver output with software over repeated VPU sessions.

`hevc_main_640x360_single_sps_rps_p32.hevc` is a 32-picture, 640x360, 8-bit HEVC Main
stream generated with x265 using a closed GOP, one reference, no B frames, and one negative
SPS short-term RPS. Every P slice selects that SPS RPS. The stream reproduces the Wave521
single-RPS decode corruption and is checked bit-exactly against software by
`../test_hevc_single_rps_decode.sh`.

`hevc_main_640x360_inline_rps_refmod_p.hevc` is an 8-picture, 640x360, 8-bit HEVC Main
stream with zero short-term RPS sets in the SPS and actual L0 list modification `[1,0]` in its
inline-RPS P pictures. It was generated with test-only changes to HM 16.8; those encoder source
changes are not included. FFmpeg header tracing and software decoding were checked, and the
candidate-driver output is compared bit-exactly by `../test_hevc_inline_refmod_decode.sh`.

`hevc_main_640x360_inline_rps_refmod_b.hevc` is an 8-picture HEVC Main stream generated with
test-only changes to HM 16.8. It has zero SPS short-term RPS sets and B-slice list modifications
for both L0 and L1; the POC 3 picture uses three current references and indices `[1,2,0]` in each
list. Temporal MVP is enabled. The HM source changes are not included. The same regression script
checks this syntax and compares all eight candidate-driver frames against software in fresh VPU
sessions.

`hevc_main_640x360_two_slices_idr_p_p.hevc` is a 640x360, 8-bit HEVC Main
stream generated with HM 16.20. It contains an IDR picture followed by two P
pictures. Each picture has two independent slices at CTU addresses 0 and 30;
WPP, tiles, TMVP, SAO, and multiple P references are disabled.

The fixture is exercised by `../test_hevc_multislice_decode.sh`, which checks
the slice layout with FFmpeg's `trace_headers` bitstream filter and compares
candidate-driver VPU output with FFmpeg software-decoded NV12 frame hashes.

`hevc_main_640x360_two_slices_idr_bbb.hevc` is a 640x360, 8-bit HEVC Main
stream generated with HM 16.20. It contains one IDR followed by three B
pictures, each with two independent slices at CTU addresses 0 and 30. Each B
slice uses the same immediately previous POC as its sole L0 and L1 reference;
WPP, tiles, TMVP, SAO, and weighted prediction are disabled.

The fixture is exercised by `../test_hevc_multislice_b_decode.sh`, which checks
the slice layout with FFmpeg's `trace_headers` bitstream filter and compares
candidate-driver VPU output with FFmpeg software-decoded NV12 frame hashes in
fresh VPU sessions.
