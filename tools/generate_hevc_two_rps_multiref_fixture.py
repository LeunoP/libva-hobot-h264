#!/usr/bin/env python3
"""Select a four-reference inline B-slice RPS from a two-entry SPS table."""

import argparse
import sys
from pathlib import Path

from generate_hevc_two_rps_sao_fixture import (
    FixtureError,
    bits_of,
    bytes_of,
    ebsp_to_rbsp,
    parse_trace,
    rbsp_to_ebsp,
    slice_field,
    split_annexb,
    ue_bits,
    unique_field,
)


def explicit_rps(block, all_used):
    negative_count = slice_field(block, "num_negative_pics")[1]
    positive_count = slice_field(block, "num_positive_pics")[1]
    if negative_count != 4 or positive_count != 0:
        raise FixtureError("target B slice must use four negative references")

    syntax = ue_bits(negative_count) + ue_bits(positive_count)
    for index in range(negative_count):
        delta = slice_field(block, f"delta_poc_s0_minus1[{index}]")[1]
        used = slice_field(block, f"used_by_curr_pic_s0_flag[{index}]")[1]
        if used != 1:
            raise FixtureError("target RPS must use all four references")
        syntax += ue_bits(delta) + [1 if all_used else 0]
    return syntax


def add_two_sps_rps(nal, count_position, long_term_position, rps0, rps1):
    bits = bits_of(ebsp_to_rbsp(nal[2:]))
    count_start = count_position - 16
    rps_end = long_term_position - 16
    if count_start < 0 or rps_end > len(bits):
        raise FixtureError("SPS trace offsets exceed the RBSP")
    stop = len(bits) - 1
    while stop >= 0 and bits[stop] == 0:
        stop -= 1
    if stop < rps_end or bits[stop] != 1:
        raise FixtureError("invalid SPS rbsp_trailing_bits")

    count, count_end = read_ue(bits, count_start)
    if count != 0 or count_end != rps_end:
        raise FixtureError("source SPS must have an empty RPS table")
    body = bits[:count_start] + ue_bits(2) + rps0 + rps1 + bits[rps_end:stop]
    body.append(1)
    body.extend([0] * ((-len(body)) % 8))
    nal[2:] = rbsp_to_ebsp(bytes_of(body))


def read_ue(bits, position):
    start = position
    while position < len(bits) and bits[position] == 0:
        position += 1
    if position >= len(bits):
        raise FixtureError("truncated SPS Exp-Golomb value")
    leading_zeroes = position - start
    position += 1
    if position + leading_zeroes > len(bits):
        raise FixtureError("truncated SPS Exp-Golomb suffix")
    suffix = 0
    for bit in bits[position:position + leading_zeroes]:
        suffix = (suffix << 1) | bit
    return (1 << leading_zeroes) - 1 + suffix, position + leading_zeroes


def select_sps_rps(nal, block):
    flag_position, flag_value = slice_field(
        block, "short_term_ref_pic_set_sps_flag")
    temporal_position, temporal_value = slice_field(
        block, "slice_temporal_mvp_enabled_flag", 1)
    if flag_value != 0 or temporal_value != 1:
        raise FixtureError("target slice must start with inline RPS and TMVP")
    if slice_field(block, "slice_type", 0)[1] != 0:
        raise FixtureError("target slice must be B type")
    if slice_field(block, "slice_pic_order_cnt_lsb", 4)[1] != 4:
        raise FixtureError("target slice must have POC LSB 4")
    if slice_field(block, "num_ref_idx_active_override_flag", 0)[1] != 0:
        raise FixtureError("target B slice must use SPS active-reference defaults")
    for list_name in ("l0", "l1"):
        if slice_field(block, f"ref_pic_list_modification_flag_{list_name}", 1)[1] != 1:
            raise FixtureError(f"target B slice must modify {list_name.upper()}")
        entries = [slice_field(block, f"list_entry_{list_name}[{i}]")[1]
                   for i in range(4)]
        if entries != [1, 2, 3, 0]:
            raise FixtureError(f"unexpected reordered {list_name.upper()} entries: {entries}")
    slice_field(block, "collocated_from_l0_flag", 0)

    start = flag_position - 16
    end = temporal_position - 16
    alignment_position, _ = slice_field(
        block, "alignment_bit_equal_to_one", 1)
    alignment_position -= 16
    zero_fields = block.get("alignment_bit_equal_to_zero", [])
    if any(value != 0 for _, value in zero_fields):
        raise FixtureError("source B slice has nonzero alignment padding")
    old_data_end = (max((position for position, _ in zero_fields),
                        default=alignment_position + 16) + 1) - 16
    if (start < 0 or end <= start or old_data_end <= alignment_position or
            old_data_end % 8):
        raise FixtureError("invalid target slice RPS/alignment range")

    bits = bits_of(ebsp_to_rbsp(nal[2:]))
    if old_data_end > len(bits):
        raise FixtureError("trace offsets exceed target slice payload")
    header = bits[:start] + [1, 0] + bits[end:alignment_position]
    header.append(1)
    header.extend([0] * ((-len(header)) % 8))
    output = header + bits[old_data_end:]
    output.extend([0] * ((-len(output)) % 8))
    nal[2:] = rbsp_to_ebsp(bytes_of(output))


def add_inline_prediction_flag(nal, block):
    flag_position, flag_value = slice_field(
        block, "short_term_ref_pic_set_sps_flag", 0)
    alignment_position, _ = slice_field(
        block, "alignment_bit_equal_to_one", 1)
    alignment_position -= 16
    zero_fields = block.get("alignment_bit_equal_to_zero", [])
    if any(value != 0 for _, value in zero_fields):
        raise FixtureError("inline slice has nonzero alignment padding")
    old_data_end = (max((position for position, _ in zero_fields),
                        default=alignment_position + 16) + 1) - 16
    insertion_position = flag_position + 1 - 16
    if flag_value != 0 or old_data_end <= alignment_position or \
            old_data_end % 8 or insertion_position >= alignment_position:
        raise FixtureError("invalid inline RPS/prediction-flag range")

    bits = bits_of(ebsp_to_rbsp(nal[2:]))
    if old_data_end > len(bits):
        raise FixtureError("trace offsets exceed inline slice payload")
    header = bits[:insertion_position] + [0] + bits[insertion_position:alignment_position]
    header.append(1)
    header.extend([0] * ((-len(header)) % 8))
    output = header + bits[old_data_end:]
    output.extend([0] * ((-len(output)) % 8))
    nal[2:] = rbsp_to_ebsp(bytes_of(output))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if not args.source.is_file() or args.output.exists():
        raise FixtureError("source must exist and output must not already exist")

    fields, slices = parse_trace(args.source)
    if {value for _, value in fields.get("num_short_term_ref_pic_sets", [])} != {0}:
        raise FixtureError("source SPS must have no short-term RPS entries")
    count_position = unique_field(fields, "num_short_term_ref_pic_sets", 0)
    long_term_position = unique_field(fields, "long_term_ref_pics_present_flag", 0)

    targets = [block for block in slices
               if block.get("slice_type") and block["slice_type"][0][1] == 0 and
               block.get("slice_pic_order_cnt_lsb") and
               block["slice_pic_order_cnt_lsb"][0][1] == 4]
    if len(targets) != 1:
        raise FixtureError(f"expected one B slice at POC LSB 4, found {len(targets)}")
    target = targets[0]
    rps0 = explicit_rps(target, True)
    rps1 = [0] + explicit_rps(target, False)

    leading, units = split_annexb(args.source.read_bytes())
    vcl_units = [unit for unit in units if ((unit[1][0] >> 1) & 0x3F) <= 31]
    if len(vcl_units) != len(slices):
        raise FixtureError("trace/NAL slice count mismatch")

    patched_sps = 0
    patched_slice = 0
    for _, nal in units:
        if ((nal[0] >> 1) & 0x3F) == 33:
            add_two_sps_rps(nal, count_position, long_term_position, rps0, rps1)
            patched_sps += 1
    for block, (_, nal) in zip(slices, vcl_units):
        if block is target:
            select_sps_rps(nal, block)
            patched_slice += 1
        elif "short_term_ref_pic_set_sps_flag" in block and \
                block["short_term_ref_pic_set_sps_flag"][0][1] == 0:
            add_inline_prediction_flag(nal, block)
    if patched_sps == 0 or patched_slice != 1:
        raise FixtureError(f"expected SPS and one selected B slice; patched {patched_sps}/{patched_slice}")

    args.output.write_bytes(leading + b"".join(
        prefix + bytes(nal) for prefix, nal in units))
    new_fields, new_slices = parse_trace(args.output)
    if {value for _, value in new_fields.get("num_short_term_ref_pic_sets", [])} != {2}:
        raise FixtureError("output SPS does not declare two RPS entries")
    selected = [block for block in new_slices
                if block.get("short_term_ref_pic_set_sps_flag", [(0, 0)])[0][1] == 1]
    if len(selected) != 1:
        raise FixtureError("output must contain exactly one SPS-selected slice")
    selected_block = selected[0]
    for name, expected in (
            ("slice_type", 0), ("slice_pic_order_cnt_lsb", 4),
            ("short_term_ref_pic_set_idx", 0),
            ("slice_temporal_mvp_enabled_flag", 1),
            ("num_ref_idx_active_override_flag", 0),
            ("ref_pic_list_modification_flag_l0", 1),
            ("ref_pic_list_modification_flag_l1", 1)):
        slice_field(selected_block, name, expected)
    for list_name in ("l0", "l1"):
        entries = [slice_field(selected_block, f"list_entry_{list_name}[{i}]")[1]
                   for i in range(4)]
        if entries != [1, 2, 3, 0]:
            raise FixtureError(f"output reordered {list_name.upper()} entries changed")

    print(f"generated verified four-reference two-RPS B fixture: {args.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FixtureError, OSError, ValueError) as exc:
        print(f"fixture generation failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
