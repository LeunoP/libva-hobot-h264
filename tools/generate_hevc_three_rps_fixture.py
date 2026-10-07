#!/usr/bin/env python3

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
    read_ue,
    split_annexb,
    ue_bits,
    unique_field,
)


def explicit_rps(used_by_current):
    return ue_bits(1) + ue_bits(0) + ue_bits(0) + [used_by_current]


def add_sps_rps(nal, count_position, long_term_position, set_count,
                rps_index):
    bits = bits_of(ebsp_to_rbsp(nal[2:]))
    count_start = count_position - 16
    rps_end = long_term_position - 16
    count, count_end = read_ue(bits, count_start)
    if count != 0 or count_end != rps_end:
        raise FixtureError("SPS does not have an empty RPS section")

    stop = len(bits) - 1
    while stop >= 0 and bits[stop] == 0:
        stop -= 1
    if stop < rps_end or bits[stop] != 1:
        raise FixtureError("invalid SPS rbsp_trailing_bits")

    body = bits[:stop]
    rps_data = []
    for index in range(set_count):
        if index > 0:
            rps_data.append(0)
        if index == rps_index:
            rps_data.extend(explicit_rps(1))
        else:
            rps_data.extend(ue_bits(0) + ue_bits(0))
    body = (body[:count_start] + ue_bits(set_count) + rps_data +
            body[count_end:])
    body.append(1)
    body.extend([0] * ((-len(body)) % 8))
    nal[2:] = rbsp_to_ebsp(bytes_of(body))


def select_sps_rps_index(nal, block, set_count, rps_index):
    flag_position, flag_value = block["short_term_ref_pic_set_sps_flag"][0]
    temporal_position, temporal_value = block[
        "slice_temporal_mvp_enabled_flag"][0]
    if flag_value != 0 or temporal_value != 1:
        raise FixtureError("source P slice must use inline RPS with temporal MVP")

    for name, expected in (
            ("num_negative_pics", 1), ("num_positive_pics", 0),
            ("delta_poc_s0_minus1[0]", 0),
            ("used_by_curr_pic_s0_flag[0]", 1)):
        values = block.get(name, [])
        if len(values) != 1 or values[0][1] != expected:
            raise FixtureError(f"inline RPS does not match selected entry: {name}")

    start = flag_position - 16
    end = temporal_position - 16
    alignment_position, _ = block["alignment_bit_equal_to_one"][0]
    alignment_position -= 16
    zero_fields = block.get("alignment_bit_equal_to_zero", [])
    old_data_end = (max((position for position, _ in zero_fields),
                        default=alignment_position + 16) + 1) - 16
    if (start < 0 or end <= start or old_data_end <= alignment_position or
            old_data_end % 8):
        raise FixtureError("invalid inline slice-header alignment")

    bits = bits_of(ebsp_to_rbsp(nal[2:]))
    if old_data_end > len(bits):
        raise FixtureError("trace offsets exceed slice NAL payload")
    index_bits = (set_count - 1).bit_length()
    selected_index = [
        (rps_index >> shift) & 1 for shift in range(index_bits - 1, -1, -1)
    ]
    header = bits[:start] + [1] + selected_index + bits[end:alignment_position]
    header.append(1)
    header.extend([0] * ((-len(header)) % 8))
    nal[2:] = rbsp_to_ebsp(bytes_of(header + bits[old_data_end:]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--set-count", type=int, default=3)
    parser.add_argument("--rps-index", type=int, default=2)
    args = parser.parse_args()
    if not 2 <= args.set_count <= 64 or not 0 <= args.rps_index < args.set_count:
        raise FixtureError("set count must be 2..64 and index must be in range")
    if not args.source.is_file() or args.output.exists():
        raise FixtureError("source must exist and output must not already exist")

    fields, slice_headers = parse_trace(args.source)
    for name, expected in (
            ("num_short_term_ref_pic_sets", 0),
            ("sample_adaptive_offset_enabled_flag", 1),
            ("sps_temporal_mvp_enabled_flag", 1),
            ("num_negative_pics", 1), ("num_positive_pics", 0),
            ("delta_poc_s0_minus1[0]", 0),
            ("used_by_curr_pic_s0_flag[0]", 1)):
        if {value for _, value in fields.get(name, [])} != {expected}:
            raise FixtureError(f"unexpected source field: {name}")

    count_position = unique_field(fields, "num_short_term_ref_pic_sets", 0)
    long_term_position = unique_field(
        fields, "long_term_ref_pics_present_flag", 0)
    leading, units = split_annexb(args.source.read_bytes())
    vcl_units = [unit for unit in units if ((unit[1][0] >> 1) & 0x3F) <= 31]
    if len(vcl_units) != len(slice_headers):
        raise FixtureError("trace/NAL slice count mismatch")

    patched_sps = 0
    patched_slices = 0
    for _, nal in units:
        if ((nal[0] >> 1) & 0x3F) == 33:
            add_sps_rps(nal, count_position, long_term_position,
                        args.set_count, args.rps_index)
            patched_sps += 1
    for block, (_, nal) in zip(slice_headers, vcl_units):
        if "short_term_ref_pic_set_sps_flag" not in block:
            continue
        slice_types = block.get("slice_type", [])
        if len(slice_types) != 1 or slice_types[0][1] != 1:
            raise FixtureError("expected one P slice")
        select_sps_rps_index(nal, block, args.set_count, args.rps_index)
        patched_slices += 1
    if patched_sps == 0 or patched_slices != 1:
        raise FixtureError(
            f"expected SPS headers and one P slice; patched {patched_sps}/{patched_slices}")

    args.output.write_bytes(leading + b"".join(
        prefix + bytes(nal) for prefix, nal in units))
    new_fields, new_slices = parse_trace(args.output)
    if {value for _, value in new_fields.get(
            "num_short_term_ref_pic_sets", [])} != {args.set_count}:
        raise FixtureError("output SPS has the wrong RPS count")
    selected = [block for block in new_slices
                if "short_term_ref_pic_set_sps_flag" in block]
    if len(selected) != 1:
        raise FixtureError("output must contain exactly one inter slice")
    for name, expected in (
            ("slice_type", 1), ("short_term_ref_pic_set_sps_flag", 1),
            ("short_term_ref_pic_set_idx", args.rps_index),
            ("slice_temporal_mvp_enabled_flag", 1),
            ("slice_sao_luma_flag", 1), ("slice_sao_chroma_flag", 1)):
        values = selected[0].get(name, [])
        if len(values) != 1 or values[0][1] != expected:
            raise FixtureError(f"output slice has unexpected {name}")

    print(f"generated verified {args.set_count}-RPS index-{args.rps_index} fixture: {args.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FixtureError, OSError, ValueError) as exc:
        print(f"fixture generation failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
