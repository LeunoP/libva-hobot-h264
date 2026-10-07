# HEVC Decode Fixtures

`hevc_main_640x360_single_sps_rps_sao_p.hevc` is a two-picture, 640x360, 8-bit HEVC Main
stream with SAO enabled and exactly one negative SPS short-term RPS. The P slice selects that
SPS RPS and enables luma and chroma SAO. It exercises the single-RPS slice-header rewrite with
the inserted RPS index before the SAO fields; `../test_hevc_single_rps_sao_decode.sh` checks the
syntax and compares candidate-driver output with software over repeated VPU sessions.

`hevc_main_640x360_single_sps_rps_p32.hevc` is a 32-picture, 640x360, 8-bit HEVC Main
stream generated with x265 using a closed GOP, one reference, no B frames, and one negative
SPS short-term RPS. Every P slice selects that SPS RPS. This is the regression fixture for the
previously observed Wave521 single-RPS decode corruption; `../test_hevc_single_rps_decode.sh`
compares all frames bit-exactly against software in five fresh VPU sessions by default.

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

`hevc_main_640x360_two_slices_single_sps_rps_p3.hevc` is a four-picture,
640x360, 8-bit HEVC Main stream based on HM 16.20. It contains one IDR and
three P pictures; each picture has two independent slices at CTU addresses 0
and 30, and each P slice selects the single negative SPS short-term RPS. WPP,
tiles, TMVP, and SAO are disabled. The test-only HM encoder change that emits
independent slice segments and omits its unused trailing empty RPS is not part
of the driver repository. Software decoding is bit-exact with HM reconstruction;
`../test_hevc_single_rps_multislice_decode.sh` checks the syntax and compares
candidate-driver VPU output with software.

`hevc_main_640x360_two_sps_rps_sao_b.hevc` is a two-picture, 640x360, 8-bit
HEVC Main stream generated with HM 16.20. It contains one IDR followed by one
B picture. The SPS has two short-term RPS entries; the B slice selects index 0,
uses only the immediately previous POC in both reference lists, and enables
luma/chroma SAO. Temporal MVP, tiles, WPP, and weighted prediction are disabled.
The two-RPS P and B SAO cases are exercised by
`../test_hevc_two_rps_sao_decode.sh`.

That regression also generates a temporary variant with weighted biprediction enabled and
non-default L0/L1 luma offsets (+8), preserving the B slice's SAO flags, then checks its decoded
NV12 output against FFmpeg software output.

`hevc_main_640x360_two_sps_rps_tmvp_b.hevc` is a five-picture 640x360, 8-bit HEVC Main
stream generated with the unmodified HM 16.20 encoder. It contains an IDR followed by four
low-delay B pictures. The SPS has two short-term RPS entries; each B slice selects index 0,
whose sole negative reference is the immediately previous POC, and uses that POC as the only
reference in both L0 and L1. Temporal MVP and luma/chroma SAO are enabled; active-reference
override is disabled, and `collocated_from_l0_flag=0` selects L1 for the collocated reference.
Other B-slice TMVP layouts are not represented by this fixture.
`../test_hevc_two_rps_b_tmvp_decode.sh` traces these fields and compares all five
candidate-driver frames against software across fresh VPU sessions.

`hevc_main_640x360_two_sps_rps_multiref_b.hevc` is an eight-picture 640x360, 8-bit HEVC Main
fixture derived from the HM 16.8 inline-RPS B list-modification stream. It adds two SPS RPS entries
and converts the POC-4 B slice to select RPS index 0 with four negative current references; both
active lists are reordered as `[1,2,3,0]`, with TMVP and luma/chroma SAO enabled. The remaining
slices retain inline RPS syntax adjusted for the SPS table. The HM source changes are not included.
`../test_hevc_two_rps_multiref_b_decode.sh` checks the syntax and compares all eight candidate-driver
frames against software across fresh VPU sessions.

`hevc_main_64x64_pcm_loop_filter_on.hevc` and `hevc_main_64x64_pcm_loop_filter_off.hevc` are
single-picture, 8-bit 4:2:0 HEVC Main fixtures generated with HM 18.0. Each contains actual
32x32 PCM coding units and differs in `pcm_loop_filter_disabled_flag`. The source pixels decode
losslessly; temporary reference-encoder instrumentation confirmed PCM CU selection. The fixture
generation source modifications and input video are not included. `../test_hevc_pcm_decode.sh`
checks both SPS settings and compares the candidate VPU output with software.

`hevc_main_64x64_pcm_8x8.hevc` and `hevc_main_64x64_pcm_16x16.hevc` are single-picture,
8-bit 4:2:0 Main fixtures generated with HM 18.0 from deterministic noisy input at QP 0.
Temporary encoder instrumentation confirmed that all selected PCM CUs are respectively 8x8
(64 CUs) and 16x16 (16 CUs). The 8x8 stream enables PCM loop filtering; the 16x16 stream
disables it. `../test_hevc_pcm_decode.sh` validates their SPS bounds and compares candidate
VPU output with software. The generation input and temporary encoder instrumentation are not
included. The regression script pins each fixture's SHA-256 so these CU-size claims remain
bound to the exact instrumentation-verified bitstreams.

`hevc_main_640x360_three_slices_idr_p_p_p.hevc` is a four-picture, 640x360, 8-bit
HEVC Main stream generated with HM 16.20. Each picture has three independent
slices beginning at CTU addresses 0, 20, and 40. WPP, tiles, TMVP, and SAO are
disabled. `../test_hevc_three_slice_decode.sh` verifies the syntax and compares
candidate-driver VPU output with software NV12 hashes over fresh sessions.

`hevc_main_640x360_uniform_tiles_2x2_i.hevc` is a single-picture 640x360, 8-bit
HEVC Main IDR stream generated with HM 18.0 using a uniform 2x2 tile grid and one
whole-picture slice. `../test_hevc_tiles_decode.sh` checks its tile/entry-point
syntax and compares candidate-driver NV12 output with software over fresh VPU sessions.

`hevc_main_640x360_nonuniform_tiles_2x2_p.hevc` is an eight-picture, 640x360,
8-bit HEVC Main stream generated with HM 18.0. Its explicit non-uniform 2x2 grid
uses 6/4 CTB columns and 4/2 CTB rows; each picture is one whole-picture slice
with three tile entry points. `../test_hevc_tiles_nonuniform_decode.sh` validates
the PPS syntax and compares candidate-driver NV12 output with software.

`hevc_main_640x360_uniform_tiles_2x2_multislice_p.hevc` is an eight-picture,
640x360, 8-bit HEVC Main stream generated with HM 18.0. It uses a uniform 2x2
tile grid and four independent slices per picture, starting at CTU addresses
0, 5, 30, and 35; each slice has zero entry-point offsets. The corresponding
decode regression is `../test_hevc_tiles_multislice_decode.sh`.

`hevc_main_640x360_nonuniform_tiles_2x2_multislice_b.hevc` is an eight-picture,
640x360, 8-bit HEVC Main B/P stream generated with HM 18.0. It uses an explicit
non-uniform 2x2 tile grid with 6/4 CTB columns and 4/2 CTB rows. Four independent
slices begin at CTU addresses 0, 6, 40, and 46; each has zero entry-point offsets.
The corresponding decode regression is
`../test_hevc_tiles_nonuniform_multislice_decode.sh`.
