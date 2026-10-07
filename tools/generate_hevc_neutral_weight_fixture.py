#!/usr/bin/env python3
"""Add neutral, luma, or chroma weighted-prediction syntax to HM streams."""

import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path


TRACE_PREFIX = re.compile(r"^\[trace_headers @ [^]]+\]")
TRACE_FIELD = re.compile(
    r"^\[trace_headers @ [^]]+\]\s+(\d+)\s+"
    r"([A-Za-z0-9_]+(?:\[\d+\])*)\s+"
)
TRACE_VALUE = re.compile(r"=\s*(-?\d+)\s*$")
START_CODE = re.compile(b"\x00\x00\x00\x01|\x00\x00\x01")


class FixtureError(Exception):
    pass


def parse_trace(path):
    command = [
        "ffmpeg", "-hide_banner", "-loglevel", "trace", "-f", "hevc",
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
        if any(marker in line for marker in (
                "Packet:", "nal_unit_type:", "Picture Parameter Set",
                "Sequence Parameter Set", "Video Parameter Set")):
            current = None
            continue
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


def unique_trace_field(fields, name, expected):
    records = fields.get(name, [])
    positions = {position for position, _ in records}
    values = {value for _, value in records}
    if len(positions) != 1 or values != {expected}:
        raise FixtureError(
            f"unexpected {name} trace records: positions={positions}, values={values}")
    return next(iter(positions))


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


def bit_at(data, position):
    return (data[position // 8] >> (7 - position % 8)) & 1


def set_bit(data, position, value):
    mask = 1 << (7 - position % 8)
    if value:
        data[position // 8] |= mask
    else:
        data[position // 8] &= ~mask


def bytes_to_bits(data):
    return [(value >> shift) & 1 for value in data for shift in range(7, -1, -1)]


def bits_to_bytes(bits):
    if len(bits) % 8:
        raise FixtureError("internal error: output bitstream is not byte-aligned")
    result = bytearray(len(bits) // 8)
    for index, bit in enumerate(bits):
        result[index // 8] |= bit << (7 - index % 8)
    return result


def ue_bits(value):
    if value < 0:
        raise FixtureError("unsigned Exp-Golomb value cannot be negative")
    code_num = value + 1
    width = code_num.bit_length()
    return [0] * (width - 1) + [
        (code_num >> shift) & 1 for shift in range(width - 1, -1, -1)
    ]


def se_bits(value):
    code_num = 2 * value - 1 if value > 0 else -2 * value
    return ue_bits(code_num)


def pred_weight_table_bits(mode, profile):
    bits = ue_bits(0) + se_bits(0)  # luma and chroma weight denominators
    luma_flag = 1 if profile == "offset" else 0
    chroma_flag = 1 if profile == "chroma" else 0
    bits.extend((luma_flag, chroma_flag))  # L0 luma/chroma flags
    if luma_flag:
        bits.extend(se_bits(0))
        bits.extend(se_bits(8))
    if chroma_flag:
        for _ in range(2):
            bits.extend(se_bits(0))
            bits.extend(se_bits(8))
    if mode == "b":
        bits.extend((luma_flag, chroma_flag))  # L1 luma/chroma flags
        if luma_flag:
            bits.extend(se_bits(0))
            bits.extend(se_bits(8))
        if chroma_flag:
            for _ in range(2):
                bits.extend(se_bits(0))
                bits.extend(se_bits(8))
    return bits


def single_slice_field(block, name):
    records = block.get(name, [])
    if len(records) != 1:
        raise FixtureError(f"expected one {name} field in slice header")
    return records[0]


def require_slice_value(block, name, expected):
    records = block.get(name, [])
    if len(records) != 1 or records[0][1] != expected:
        values = [value for _, value in records]
        raise FixtureError(f"unexpected {name} values: {values}; expected {expected}")


def patch_fixture(source, mode, profile):
    fields, slice_headers = parse_trace(source)
    rps_counts = {value for _, value in fields.get("num_short_term_ref_pic_sets", [])}
    if rps_counts != {2}:
        raise FixtureError("source SPS does not declare two short-term RPS sets")

    weighted_name = "weighted_pred_flag" if mode == "p" else "weighted_bipred_flag"
    other_weighted_name = "weighted_bipred_flag" if mode == "p" else "weighted_pred_flag"
    weighted_position = unique_trace_field(fields, weighted_name, 0) - 16
    other_weighted_position = unique_trace_field(fields, other_weighted_name, 0) - 16
    if weighted_position < 0 or other_weighted_position < 0:
        raise FixtureError("PPS weighted flag offset precedes the RBSP payload")

    leading, units = split_annexb(source.read_bytes())
    vcl_units = [unit for unit in units if ((unit[1][0] >> 1) & 0x3F) <= 31]
    if len(vcl_units) != len(slice_headers):
        raise FixtureError(
            f"trace/NAL slice count mismatch: {len(slice_headers)} vs {len(vcl_units)}")

    weight_table = pred_weight_table_bits(mode, profile)
    patched_slices = 0
    for block, (_, nal) in zip(slice_headers, vcl_units):
        nal_type = (nal[0] >> 1) & 0x3F
        trace_nal_type = single_slice_field(block, "nal_unit_type")[1]
        slice_type = single_slice_field(block, "slice_type")[1]
        if nal_type != trace_nal_type:
            raise FixtureError("trace/NAL unit type mismatch")
        target_type = 1 if mode == "p" else 0
        if slice_type != target_type:
            continue

        insert_position = single_slice_field(
            block, "five_minus_max_num_merge_cand")[0] - 16
        alignment_position = single_slice_field(
            block, "alignment_bit_equal_to_one")[0] - 16
        zero_fields = block.get("alignment_bit_equal_to_zero", [])
        if insert_position < 0 or alignment_position < insert_position:
            raise FixtureError("invalid slice-header bit offsets")
        if any(value != 0 for _, value in zero_fields):
            raise FixtureError("source slice has nonzero alignment padding")
        old_data_end = (max((pos for pos, _ in zero_fields),
                            default=alignment_position + 16) + 1) - 16
        if old_data_end <= alignment_position or old_data_end % 8:
            raise FixtureError("source slice-data boundary is not byte-aligned")

        rbsp = ebsp_to_rbsp(nal[2:])
        bits = bytes_to_bits(rbsp)
        if old_data_end > len(bits) or alignment_position >= old_data_end:
            raise FixtureError("trace offsets exceed slice NAL payload")
        header = bits[:insert_position] + weight_table + bits[insert_position:alignment_position]
        header.append(1)
        header.extend([0] * ((-len(header)) % 8))
        rebuilt = header + bits[old_data_end:]
        nal[2:] = rbsp_to_ebsp(bits_to_bytes(rebuilt))
        patched_slices += 1

    if patched_slices == 0:
        raise FixtureError(f"no {mode.upper()} inter slices found")

    patched_pps = 0
    for _, nal in units:
        if ((nal[0] >> 1) & 0x3F) != 34:
            continue
        rbsp = ebsp_to_rbsp(nal[2:])
        if len(rbsp) * 8 <= max(weighted_position, other_weighted_position):
            raise FixtureError("PPS weighted flag offset exceeds payload")
        if bit_at(rbsp, weighted_position) != 0 or bit_at(rbsp, other_weighted_position) != 0:
            raise FixtureError("source PPS already has a weighted prediction flag")
        set_bit(rbsp, weighted_position, 1)
        nal[2:] = rbsp_to_ebsp(rbsp)
        patched_pps += 1
    if patched_pps == 0:
        raise FixtureError("no PPS NAL units found")

    return leading + b"".join(prefix + bytes(nal) for prefix, nal in units), patched_slices


def verify_fixture(path, mode, profile, expected_slices):
    fields, slice_headers = parse_trace(path)
    rps_counts = {value for _, value in fields.get("num_short_term_ref_pic_sets", [])}
    if rps_counts != {2}:
        raise FixtureError("output SPS no longer declares two short-term RPS sets")
    weighted_name = "weighted_pred_flag" if mode == "p" else "weighted_bipred_flag"
    other_weighted_name = "weighted_bipred_flag" if mode == "p" else "weighted_pred_flag"
    unique_trace_field(fields, weighted_name, 1)
    unique_trace_field(fields, other_weighted_name, 0)
    target_type = 1 if mode == "p" else 0
    matching = [block for block in slice_headers
                if single_slice_field(block, "slice_type")[1] == target_type]
    if len(matching) != expected_slices:
        raise FixtureError("output slice count changed unexpectedly")
    expected_luma_flag = 1 if profile == "offset" else 0
    expected_chroma_flag = 1 if profile == "chroma" else 0
    for block in matching:
        require_slice_value(block, "luma_log2_weight_denom", 0)
        require_slice_value(block, "delta_chroma_log2_weight_denom", 0)
        for reference_list in (("l0",) if mode == "p" else ("l0", "l1")):
            require_slice_value(
                block, f"luma_weight_{reference_list}_flag[0]", expected_luma_flag)
            require_slice_value(
                block, f"chroma_weight_{reference_list}_flag[0]", expected_chroma_flag)
            if expected_luma_flag:
                require_slice_value(
                    block, f"delta_luma_weight_{reference_list}[0]", 0)
                require_slice_value(
                    block, f"luma_offset_{reference_list}[0]", 8)
            if expected_chroma_flag:
                for component in range(2):
                    require_slice_value(
                        block,
                        f"delta_chroma_weight_{reference_list}[0][{component}]", 0)
                    require_slice_value(
                        block,
                        f"chroma_offset_{reference_list}[0][{component}]", 8)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("p", "b"), required=True)
    parser.add_argument("--weights", choices=("neutral", "offset", "chroma"),
                        default="neutral")
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source = args.source.resolve()
    output = args.output.resolve()
    if source == output or not source.is_file():
        parser.error("source must be an existing file distinct from output")
    if output.exists():
        parser.error("output already exists")

    try:
        data, patched_slices = patch_fixture(source, args.mode, args.weights)
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=output.name + ".",
                                         delete=False) as temporary:
            temporary.write(data)
            temporary.flush()
            os.fsync(temporary.fileno())
            temporary_path = Path(temporary.name)
        try:
            os.link(temporary_path, output)
        except FileExistsError:
            raise FixtureError("output was created concurrently; refusing overwrite")
        finally:
            temporary_path.unlink(missing_ok=True)
        verify_fixture(output, args.mode, args.weights, patched_slices)
    except (FixtureError, OSError, subprocess.SubprocessError) as error:
        output.unlink(missing_ok=True)
        parser.exit(1, f"fixture generation failed: {error}\n")

    print(f"PASS: generated {args.weights} weighted-{args.mode.upper()} fixture "
          f"({patched_slices} slices): {output}")


if __name__ == "__main__":
    main()
