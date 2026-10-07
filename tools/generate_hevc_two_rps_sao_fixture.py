#!/usr/bin/env python3
"""Add an equivalent duplicate SPS RPS to the checked-in SAO P fixture."""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path


TRACE_PREFIX = re.compile(r"^\[trace_headers @ [^]]+\]")
TRACE_FIELD = re.compile(
    r"^\[trace_headers @ [^]]+\]\s+(\d+)\s+([A-Za-z0-9_]+(?:\[\d+\])*)\s+"
)
TRACE_VALUE = re.compile(r"=\s*(-?\d+)\s*$")
START_CODE = re.compile(b"\x00\x00\x00\x01|\x00\x00\x01")


class FixtureError(Exception):
    pass


def parse_trace(path):
    ffmpeg = os.environ.get("FFMPEG", "ffmpeg")
    command = [
        ffmpeg, "-hide_banner", "-loglevel", "trace", "-f", "hevc",
        "-i", str(path), "-c:v", "copy", "-bsf:v", "trace_headers",
        "-f", "null", "-",
    ]
    result = subprocess.run(command, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True, check=False)
    if result.returncode != 0:
        raise FixtureError("ffmpeg trace_headers failed")

    fields = {}
    slices = []
    current = None
    for line in result.stderr.splitlines():
        if not TRACE_PREFIX.match(line):
            continue
        if "Slice Segment Header" in line:
            current = {}
            slices.append(current)
            continue
        if "nal_unit_type:" not in line and any(marker in line for marker in (
                "Packet:", "Picture Parameter Set",
                "Sequence Parameter Set", "Video Parameter Set")):
            current = None
        match = TRACE_FIELD.match(line)
        value_match = TRACE_VALUE.search(line)
        if not match or not value_match:
            continue
        position, name = int(match.group(1)), match.group(2)
        value = int(value_match.group(1))
        fields.setdefault(name, []).append((position, value))
        if current is not None:
            current.setdefault(name, []).append((position, value))
    return fields, slices


def unique_field(fields, name, expected):
    records = fields.get(name, [])
    positions = {position for position, _ in records}
    values = {value for _, value in records}
    if len(positions) != 1 or values != {expected}:
        raise FixtureError(
            f"unexpected {name}: positions={positions}, values={values}")
    return next(iter(positions))


def slice_field(block, name, expected=None):
    records = block.get(name, [])
    if len(records) != 1 or (expected is not None and records[0][1] != expected):
        raise FixtureError(f"unexpected slice field {name}: {records}")
    return records[0]


def split_annexb(data):
    matches = list(START_CODE.finditer(data))
    if not matches or data[:matches[0].start()].strip(b"\x00"):
        raise FixtureError("input is not an Annex-B HEVC stream")
    leading = data[:matches[0].start()]
    units = []
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(data)
        nal = data[match.end():end]
        if len(nal) < 2:
            raise FixtureError("truncated HEVC NAL unit")
        units.append((match.group(0), bytearray(nal)))
    return leading, units


def ebsp_to_rbsp(ebsp):
    rbsp = bytearray()
    zero_count = 0
    index = 0
    while index < len(ebsp):
        value = ebsp[index]
        if zero_count == 2 and value == 3:
            if index + 1 >= len(ebsp) or ebsp[index + 1] > 3:
                raise FixtureError("invalid emulation-prevention byte")
            zero_count = 0
            index += 1
            continue
        rbsp.append(value)
        zero_count = zero_count + 1 if value == 0 else 0
        index += 1
    return rbsp


def rbsp_to_ebsp(rbsp):
    ebsp = bytearray()
    zero_count = 0
    for value in rbsp:
        if zero_count == 2 and value <= 3:
            ebsp.append(3)
            zero_count = 0
        ebsp.append(value)
        zero_count = zero_count + 1 if value == 0 else 0
    return ebsp


def bits_of(data):
    return [(value >> shift) & 1 for value in data
            for shift in range(7, -1, -1)]


def bytes_of(bits):
    if len(bits) % 8:
        raise FixtureError("internal error: RBSP is not byte-aligned")
    result = bytearray(len(bits) // 8)
    for index, bit in enumerate(bits):
        result[index // 8] |= bit << (7 - index % 8)
    return result


def ue_bits(value):
    code_num = value + 1
    width = code_num.bit_length()
    return [0] * (width - 1) + [
        (code_num >> shift) & 1 for shift in range(width - 1, -1, -1)
    ]


def read_ue(bits, position):
    start = position
    while position < len(bits) and bits[position] == 0:
        position += 1
    if position >= len(bits):
        raise FixtureError("truncated Exp-Golomb value in SPS")
    leading_zeroes = position - start
    position += 1
    if position + leading_zeroes > len(bits):
        raise FixtureError("truncated Exp-Golomb suffix in SPS")
    suffix = 0
    for bit in bits[position:position + leading_zeroes]:
        suffix = (suffix << 1) | bit
    return (1 << leading_zeroes) - 1 + suffix, position + leading_zeroes


def insert_duplicate_rps_in_sps(nal, count_position, rps_end):
    rbsp = ebsp_to_rbsp(nal[2:])
    bits = bits_of(rbsp)
    count_start = count_position - 16
    rps_end -= 16
    original_count, count_end = read_ue(bits, count_start)
    if original_count != 1 or count_end != count_start + len(ue_bits(1)):
        raise FixtureError("SPS RPS count is not the expected single-set value")
    if count_end - count_start != len(ue_bits(2)):
        raise FixtureError("replacement SPS RPS count changes field width")
    bits[count_start:count_end] = ue_bits(2)

    stop = len(bits) - 1
    while stop >= 0 and bits[stop] == 0:
        stop -= 1
    if stop < rps_end or bits[stop] != 1:
        raise FixtureError("invalid SPS rbsp_trailing_bits")
    body = bits[:stop]
    if rps_end > len(body):
        raise FixtureError("duplicate RPS insertion point exceeds SPS syntax")

    # Explicit RPS: one used negative reference at delta POC -1, no positives.
    duplicate = [0] + ue_bits(1) + ue_bits(0) + ue_bits(0) + [1]
    body[rps_end:rps_end] = duplicate
    body.append(1)
    body.extend([0] * ((-len(body)) % 8))
    nal[2:] = rbsp_to_ebsp(bytes_of(body))


def insert_rps_index_in_slice(nal, block):
    index_flag_position, _ = slice_field(
        block, "short_term_ref_pic_set_sps_flag", 1)
    if slice_field(block, "slice_type", 1)[1] != 1:
        raise FixtureError("expected one P slice selecting the SPS RPS")
    if slice_field(block, "slice_sao_luma_flag", 1)[1] != 1 or \
            slice_field(block, "slice_sao_chroma_flag", 1)[1] != 1:
        raise FixtureError("expected luma and chroma SAO in the P slice")

    alignment_position, _ = slice_field(
        block, "alignment_bit_equal_to_one", 1)
    zero_fields = block.get("alignment_bit_equal_to_zero", [])
    if any(value != 0 for _, value in zero_fields):
        raise FixtureError("source slice has nonzero alignment padding")
    old_data_end = (max((position for position, _ in zero_fields),
                        default=alignment_position + 16) + 1) - 16
    alignment_position -= 16
    insertion_position = index_flag_position + 1 - 16
    if (old_data_end <= alignment_position or old_data_end % 8 or
            insertion_position >= alignment_position):
        raise FixtureError("invalid source slice-header alignment")

    bits = bits_of(ebsp_to_rbsp(nal[2:]))
    if old_data_end > len(bits):
        raise FixtureError("trace offsets exceed slice NAL payload")
    header = bits[:insertion_position] + [0] + bits[
        insertion_position:alignment_position]
    header.append(1)
    header.extend([0] * ((-len(header)) % 8))
    nal[2:] = rbsp_to_ebsp(bytes_of(header + bits[old_data_end:]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if not args.source.is_file() or args.output.exists():
        raise FixtureError("source must exist and output must not already exist")

    fields, slice_headers = parse_trace(args.source)
    if {value for _, value in fields.get("num_short_term_ref_pic_sets", [])} != {1}:
        raise FixtureError("source SPS must declare exactly one short-term RPS")
    if {value for _, value in fields.get("sample_adaptive_offset_enabled_flag", [])} != {1}:
        raise FixtureError("source SPS must enable SAO")
    for name, expected in (
            ("num_negative_pics", 1), ("num_positive_pics", 0),
            ("delta_poc_s0_minus1[0]", 0),
            ("used_by_curr_pic_s0_flag[0]", 1)):
        if {value for _, value in fields.get(name, [])} != {expected}:
            raise FixtureError(f"source RPS does not match supported pattern: {name}")
    count_position = unique_field(fields, "num_short_term_ref_pic_sets", 1)
    used_position = unique_field(fields, "used_by_curr_pic_s0_flag[0]", 1)

    leading, units = split_annexb(args.source.read_bytes())
    vcl_units = [unit for unit in units if ((unit[1][0] >> 1) & 0x3F) <= 31]
    if len(vcl_units) != len(slice_headers):
        raise FixtureError("trace/NAL slice count mismatch")

    patched_sps = 0
    patched_slices = 0
    for _, nal in units:
        nal_type = (nal[0] >> 1) & 0x3F
        if nal_type == 33:
            insert_duplicate_rps_in_sps(nal, count_position, used_position + 1)
            patched_sps += 1
    for block, (_, nal) in zip(slice_headers, vcl_units):
        if "short_term_ref_pic_set_sps_flag" not in block:
            continue
        insert_rps_index_in_slice(nal, block)
        patched_slices += 1
    if patched_sps == 0 or patched_slices != 1:
        raise FixtureError(
            f"expected SPS headers and one P slice; patched {patched_sps}/{patched_slices}")

    output = leading + b"".join(prefix + bytes(nal) for prefix, nal in units)
    args.output.write_bytes(output)
    new_fields, new_slices = parse_trace(args.output)
    if {value for _, value in new_fields.get("num_short_term_ref_pic_sets", [])} != {2}:
        raise FixtureError("output SPS does not declare two short-term RPS sets")
    selected = [block for block in new_slices
                if "short_term_ref_pic_set_sps_flag" in block]
    if len(selected) != 1:
        raise FixtureError("output does not contain exactly one SPS-selected inter slice")
    slice_field(selected[0], "short_term_ref_pic_set_idx", 0)
    slice_field(selected[0], "slice_sao_luma_flag", 1)
    slice_field(selected[0], "slice_sao_chroma_flag", 1)
    print(f"generated verified two-RPS SAO fixture: {args.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FixtureError, OSError, subprocess.SubprocessError) as exc:
        print(f"fixture generation failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
