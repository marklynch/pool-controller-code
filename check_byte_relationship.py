#!/usr/bin/env python3
"""
check_byte_relationship.py

Scans for messages matching pattern `02 00 50 FF FF 80 00 38`
and checks if byte 8 + 8 = byte 9

Using 0-based indexing:
- Pattern bytes: frame[0:8] = [02, 00, 50, FF, FF, 80, 00, 38]
- Byte to check: frame[8] (SUB field)
- Byte to compare: frame[9] (REG field)
- Condition: frame[8] + 8 == frame[9]

Example: 0F + 8 = 17 (15 + 8 = 23)

Note: If you meant different byte positions, you can easily adjust
CHECK_BYTE_A and CHECK_BYTE_B below.
"""

from __future__ import annotations

import argparse
import os
from typing import List, Tuple
from collections import defaultdict


# Pattern to search for (first 8 bytes)
SEARCH_PATTERN = [0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x38]

# Byte positions to check (0-based indexing)
CHECK_BYTE_A = 8  # Position of byte A (SUB field)
CHECK_BYTE_B = 9  # Position of byte B (REG field)
OFFSET = 8        # Check if byte_A == byte_B + OFFSET

# Register groups (start, end, slot/data_type, label) - inclusive ranges
# slot=None means any data type matches this group
REGISTER_GROUPS = [
    (0x31, 0x38, 0x03, "Favourite labels"),
    (0x6C, 0x73, 0x02, "Channel Types"),
    (0x7C, 0x83, 0x02, "Channel Labels"),
    (0xC0, 0xC7, 0x01, "Light Zone State"),
    (0xD0, 0xD1, 0x02, "Valve Labels"),
    (0xD0, 0xD7, 0x01, "Light Zone Color"),
    (0xE0, 0xE7, 0x01, "Light Zone Active"),
]


def get_register_group(register_id: int, data_type: int = None) -> str:
    """Get the group label for a register ID and data type, or 'Other' if no group matches."""
    for start, end, slot, label in REGISTER_GROUPS:
        if start <= register_id <= end:
            # If slot is None, match any data type
            # If slot is specified, it must match the data type
            if slot is None or slot == data_type:
                return label
    return "Other"


def parse_hex_bytes_from_line(line: str) -> List[int]:
    """Parse hex bytes from a line of text."""
    out: List[int] = []
    for t in line.strip().split():
        t = t.strip().lower()
        if t.startswith("0x"):
            t = t[2:]
        if 1 <= len(t) <= 2 and all(c in "0123456789abcdef" for c in t):
            out.append(int(t, 16))
    return out


def fmt_frame(frame: List[int]) -> str:
    """Format frame bytes as hex string."""
    return " ".join(f"{b:02X}" for b in frame)


def matches_pattern(frame: List[int]) -> bool:
    """Check if frame starts with the search pattern."""
    if len(frame) < len(SEARCH_PATTERN):
        return False
    return frame[:len(SEARCH_PATTERN)] == SEARCH_PATTERN


def check_byte_relationship(frame: List[int]) -> Tuple[bool, int, int]:
    """
    Check if frame[CHECK_BYTE_A] + OFFSET == frame[CHECK_BYTE_B]

    Returns:
        (relationship_holds, byte_a_value, byte_b_value)
    """
    if len(frame) <= max(CHECK_BYTE_A, CHECK_BYTE_B):
        return False, -1, -1

    byte_a = frame[CHECK_BYTE_A]
    byte_b = frame[CHECK_BYTE_B]
    relationship_holds = ((byte_a + OFFSET) & 0xFF == byte_b)

    return relationship_holds, byte_a, byte_b


def iter_files(root: str, exts: Tuple[str, ...], recursive: bool):
    """Iterate over files matching extensions."""
    if os.path.isfile(root):
        yield root
        return

    if recursive:
        for dirpath, _, filenames in os.walk(root):
            for fn in filenames:
                if exts and not fn.lower().endswith(exts):
                    continue
                yield os.path.join(dirpath, fn)
    else:
        for fn in os.listdir(root):
            path = os.path.join(root, fn)
            if not os.path.isfile(path):
                continue
            if exts and not fn.lower().endswith(exts):
                continue
            yield path


def main() -> None:
    ap = argparse.ArgumentParser(
        description=f"Scan for pattern {' '.join(f'{b:02X}' for b in SEARCH_PATTERN)} "
                    f"and check if byte[{CHECK_BYTE_A}] + {OFFSET} == byte[{CHECK_BYTE_B}]"
    )
    ap.add_argument("path", help="Folder (or file) of hex dump logs.")
    ap.add_argument(
        "--ext",
        nargs="*",
        default=[".txt", ".log"],
        help="File extensions to include (default: .txt .log).",
    )
    ap.add_argument("--no-recursive", action="store_true", help="Do not recurse into subfolders.")
    ap.add_argument("--show-all", action="store_true", help="Show all matching pattern frames, not just those meeting the condition.")
    args = ap.parse_args()

    exts = tuple(e.lower() for e in (args.ext or []) if e)
    recursive = not args.no_recursive

    # Statistics
    total_lines = 0
    matching_pattern = 0
    condition_met = 0
    condition_failed = 0

    # Track unique byte combinations
    byte_combinations = defaultdict(int)
    register_counts = defaultdict(int)  # byte 10
    data_type_counts = defaultdict(int)  # byte 11
    register_by_type = defaultdict(int)  # (register_id, data_type) tuple
    register_lengths = defaultdict(lambda: {"min": 9999, "max": 0})  # track min/max message lengths

    # Data Type 0x0B verification
    binary_type_count = 0
    binary_valid_next_byte = 0
    binary_invalid_next_byte = 0
    binary_valid_ending = 0
    binary_invalid_ending = 0

    print(f"Searching for pattern: {' '.join(f'{b:02X}' for b in SEARCH_PATTERN)}")
    print(f"Checking condition: byte[{CHECK_BYTE_A}] + {OFFSET} == byte[{CHECK_BYTE_B}]\n")

    any_files = False

    for filepath in iter_files(args.path, exts, recursive):
        any_files = True
        print(f"Processing {filepath}...")

        try:
            with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
                for lineno, line in enumerate(f, start=1):
                    frame = parse_hex_bytes_from_line(line)

                    if not frame:
                        continue

                    total_lines += 1

                    if not matches_pattern(frame):
                        continue

                    matching_pattern += 1
                    holds, byte_a, byte_b = check_byte_relationship(frame)

                    # Track this combination
                    if byte_a >= 0 and byte_b >= 0:
                        byte_combinations[(byte_a, byte_b)] += 1

                    # Extract register ID (byte 10) and data_type (byte 11)
                    register_id = frame[10] if len(frame) > 10 else None
                    data_type = frame[11] if len(frame) > 11 else None

                    if register_id is not None:
                        register_counts[register_id] += 1
                    if data_type is not None:
                        data_type_counts[data_type] += 1
                    if register_id is not None and data_type is not None:
                        key = (register_id, data_type)
                        register_by_type[key] += 1

                        # Track message length
                        msg_len = len(frame)
                        register_lengths[key]["min"] = min(register_lengths[key]["min"], msg_len)
                        register_lengths[key]["max"] = max(register_lengths[key]["max"], msg_len)

                    # Special verification for Data Type 0x0B (Binary)
                    if data_type == 0x0B:
                        binary_type_count += 1

                        # Check if byte 12 is 0 or 1
                        if len(frame) > 12:
                            next_byte = frame[12]
                            if next_byte in (0x00, 0x01):
                                binary_valid_next_byte += 1
                            else:
                                binary_invalid_next_byte += 1
                                print(
                                    f"[BINARY_CHECK] {os.path.basename(filepath)}:{lineno} "
                                    f"Data Type 0x0B but byte[12]={next_byte:02X} (expected 00 or 01) "
                                    f"{fmt_frame(frame)}"
                                )

                        # Check if last two bytes are [checksum, 0x03]
                        if len(frame) >= 2:
                            if frame[-1] == 0x03 and len(frame) >= 13:
                                # Valid ending structure
                                binary_valid_ending += 1
                            else:
                                binary_invalid_ending += 1
                                print(
                                    f"[BINARY_CHECK] {os.path.basename(filepath)}:{lineno} "
                                    f"Data Type 0x0B but invalid ending (last byte={frame[-1]:02X}) "
                                    f"{fmt_frame(frame)}"
                                )

                    if holds:
                        condition_met += 1
                        print(
                            f"[MATCH] {os.path.basename(filepath)}:{lineno} "
                            f"byte[{CHECK_BYTE_A}]={byte_a:02X} byte[{CHECK_BYTE_B}]={byte_b:02X} "
                            f"({byte_a}+{OFFSET}={byte_b}) {fmt_frame(frame)}"
                        )
                    else:
                        condition_failed += 1
                        if args.show_all:
                            expected = (byte_a + OFFSET) & 0xFF if byte_a >= 0 else -1
                            print(
                                f"[NO_MATCH] {os.path.basename(filepath)}:{lineno} "
                                f"byte[{CHECK_BYTE_A}]={byte_a:02X} byte[{CHECK_BYTE_B}]={byte_b:02X} "
                                f"expected {expected:02X} {fmt_frame(frame)}"
                            )

        except OSError as e:
            print(f"[ERROR] Could not read {filepath}: {e}")
            continue

    if not any_files:
        print(f"No matching files found at: {args.path}")
        return

    print("\n" + "=" * 80)
    print("SUMMARY")
    print("=" * 80)
    print(f"Total lines processed:          {total_lines}")
    print(f"Lines matching pattern:         {matching_pattern}")
    print(f"Condition met (byte relation):  {condition_met}")
    print(f"Condition failed:               {condition_failed}")

    if byte_combinations:
        print(f"\n{'-' * 80}")
        print(f"Byte[{CHECK_BYTE_A}] vs Byte[{CHECK_BYTE_B}] combinations found:")
        print(f"{'-' * 80}")
        for (byte_a, byte_b), count in sorted(byte_combinations.items()):
            expected = (byte_a + OFFSET) & 0xFF
            match = "✓" if expected == byte_b else "✗"
            print(
                f"  {match} byte[{CHECK_BYTE_A}]={byte_a:02X} ({byte_a:3d})  "
                f"byte[{CHECK_BYTE_B}]={byte_b:02X} ({byte_b:3d})  "
                f"expected={expected:02X} ({expected:3d})  "
                f"count={count:5d}"
            )

    if register_by_type:
        # Group registers by their category (using register_id + data_type)
        grouped_registers = defaultdict(list)
        for (register_id, dt), count in register_by_type.items():
            group = get_register_group(register_id, dt)
            grouped_registers[group].append((register_id, dt, count))

        print(f"\n{'-' * 80}")
        print(f"Register IDs (byte[10]) + Data Type (byte[11]): {len(register_by_type)} unique combinations")
        print(f"{'-' * 80}")

        # Display each group
        for start, end, slot, group_label in REGISTER_GROUPS:
            if group_label in grouped_registers:
                slot_label = f"0x{slot:02X}" if slot is not None else "Any"
                print(f"\n  {group_label} (0x{start:02X}-0x{end:02X}, slot={slot_label}):")
                for register_id, dt, count in sorted(grouped_registers[group_label]):
                    key = (register_id, dt)
                    min_len = register_lengths[key]["min"]
                    max_len = register_lengths[key]["max"]
                    print(f"    Reg 0x{register_id:02X} + Type 0x{dt:02X}: {count:5d} occurrences  (len: {min_len}-{max_len})")

        # Display "Other" registers that don't fit any group
        if "Other" in grouped_registers:
            print(f"\n  Other (ungrouped register/type combinations):")
            for register_id, dt, count in sorted(grouped_registers["Other"]):
                key = (register_id, dt)
                min_len = register_lengths[key]["min"]
                max_len = register_lengths[key]["max"]
                print(f"    Reg 0x{register_id:02X} + Type 0x{dt:02X}: {count:5d} occurrences  (len: {min_len}-{max_len})")

    if data_type_counts:
        print(f"\n{'-' * 80}")
        print(f"Data Types (byte[11]) found: {len(data_type_counts)} unique values")
        print(f"{'-' * 80}")
        for data_type, count in sorted(data_type_counts.items()):
            print(f"  Data Type 0x{data_type:02X} ({data_type:3d}): {count:5d} occurrences")

    if binary_type_count > 0:
        print(f"\n{'-' * 80}")
        print(f"Data Type 0x0B (Binary) Verification:")
        print(f"{'-' * 80}")
        print(f"  Total 0x0B messages:           {binary_type_count:5d}")
        print(f"  Valid byte[12] (0x00 or 0x01): {binary_valid_next_byte:5d}")
        print(f"  Invalid byte[12]:              {binary_invalid_next_byte:5d}")
        print(f"  Valid ending (checksum + 0x03):{binary_valid_ending:5d}")
        print(f"  Invalid ending:                {binary_invalid_ending:5d}")


if __name__ == "__main__":
    main()
