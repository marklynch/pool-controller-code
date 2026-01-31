#!/usr/bin/env python3
"""
verify_checksums.py

Treat EACH LINE as one candidate frame. Do NOT reassemble across lines.

Strict framing rules:
- A VALID frame line MUST start with 0x02 and end with 0x03.
- If it starts with 0x02 but does not end with 0x03 => BROKEN (incomplete)
- If it does not start with 0x02 => BROKEN (no_start)
- If it starts with 0x02 and ends with 0x03 BUT is too short to even contain
  the required header fields (src/cmd/sub) => BROKEN (too_short_header)

IMPORTANT:
- BROKEN frames are NOT included in the SRC/CMD/SUB analysis table.

Checksum:
- If len == 12 AND framing OK => OK (NO_DATA_SECTION_OK)
- Else if len < 13 => framed-but-not-checkable (framed_too_short_checksum) [included in analysis]
- Else checksum = sum(bytes 11 .. (len-2)) mod 256, stored in 2nd last byte

Summaries:
- Group by SRC (bytes 2-3), CMD (byte 8), SUB (byte 9) for VALID frames only.
- Count broken lines separately.
- Count valid frames separately.

Field positions (0-based):
- Source address: idx 1,2   (requires len >= 3)
- Command:        idx 7     (requires len >= 8)
- Subcommand:     idx 8     (requires len >= 9)
So to include in analysis, we require len >= 9 (indices 0..8 exist).

NEW:
1) CMD/SUB label table (like SRC labels) so you can add known opcodes over time.
2) Reduced width of the SRC label column in summary output.
"""

from __future__ import annotations

import argparse
import os
from dataclasses import dataclass
from collections import defaultdict
from typing import Iterable, List, Optional, Tuple, Dict


# ----------------------------
# Label tables (edit as needed)
# ----------------------------

# Source label mapping (bytes 2-3)
SRC_LABELS: Dict[Tuple[int, int], str] = {
    (0x00, 0x50): "Controller",
    (0x00, 0x62): "Temperature",
    (0x00, 0x90): "Chemistry",
    (0x00, 0xF0): "Internet Gateway",
}

# Command/Subcommand label mapping (byte 8 / byte 9)
# Key = (CMD, SUB, REG). Add entries as you identify them.
CMD_SUB_LABELS: Dict[Tuple[int, int, int], str] = {
    # Controller
    (0x05, 0x0D, 0xE2): "Light (active) (maybe)", # 0 off, 1 on - TBC
    (0x06, 0x0E, 0xE4): "Light config",
    (0x0B, 0x25, 0x00): "Channel Status",
    (0x0D, 0x0D, 0x5B): "Channels",

    (0x14, 0x0D, 0xF1): "Pool/Spa mode",
    (0x17, 0x10, 0xF7): "Pool and Spa Setpoints",

    (0x26, 0x0E, 0x04): "Temperature Scale C/F",

    # 02 00 50 FF FF 80 00 38 0F 17 C0 01 00 C1 03
    # 02 00 50 FF FF 80 00 38 0F 17 C1 01 00 C2 03
    # 02 00 50 FF FF 80 00 38 0F 17 C2 01 00 C3 03
    # 02 00 50 FF FF 80 00 38 0F 17 C3 01 00 C4 03
    # 02 00 50 FF FF 80 00 38 0F 17 D0 01 05 D6 03
    # 02 00 50 FF FF 80 00 38 0F 17 D1 01 05 D7 03
    # 02 00 50 FF FF 80 00 38 0F 17 D2 01 05 D8 03
    # 02 00 50 FF FF 80 00 38 0F 17 D3 01 05 D9 03
    # 02 00 50 FF FF 80 00 38 0F 17 E0 01 00 E1 03
    # 02 00 50 FF FF 80 00 38 0F 17 E1 01 00 E2 03
    # 02 00 50 FF FF 80 00 38 0F 17 E2 01 00 E3 03
    # 02 00 50 FF FF 80 00 38 0F 17 E3 01 00 E4 03
    (0x38, 0x0F, 0x17): "Register State Data", 

    # These labels are strange as the data holds the channel reference - but I can't figure out the CMD/SUB/REG that identifies the channel label itself
    (0x38, 0x12, 0x1A): "Spa Label",
    (0x38, 0x13, 0x1B): "Multi Label", # 80: Jets
                                       # 31: Pool
                                       # 0A, 0B, 0C, 0D, 0E, 0F, 10, 11, 12, 13, 14, 15, 16, 17 : Null

    (0x38, 0x15, 0x1D): "Channel 6 Label", # Blower
    (0x38, 0x16, 0x1E): "Valve Label",  #. D0: Valve 1, D1: Valve 2. Gas multiple valves keyed by data (valve 1, valve 2, etc)
    (0x38, 0x17, 0x1F): "Channel 2 Label", # Cleaning
    
    (0x38, 0x1A, 0x22): "Channel 1 Label", # Filter Pump
    (0xFD, 0x0F, 0xDC): "Controller Time",

    # Temperature sensor commands
    (0x12, 0x0F, 0x03): "Heater State",
    (0x16, 0x0E, 0x06): "Temp Reading",  # 02 00 62 FF FF 80 00 16 0E 06 19 00 19 03

    # Chemistry
    (0x1D, 0x0F, 0x3C): "Chlorinator Setpoints", # 01 pH setpoint, 02 ORP setpoint
    (0x1F, 0x0F, 0x3E): "Chlorinator Readings", # 01 pH reading, 02 ORP reading


    # Internet Gateway
    (0x0A, 0x0E, 0x88): "IG Version (maybe)",  # Data: 05 01 -> version 5.1?
    (0x12, 0x0F, 0x91): "IG Version (maybe)",  # Data: 05 01 06 -> version 5.1.6?
    
    (0x39, 0x0E, 0xB7): "IG Request Config (maybe)",    # Data: 02 00 F0 FF FF 80 00 39 0E B7 E0 01 E1 03 - response of 02 00 50 FF FF 80 00 38 0F 17 E0 01 00 E1 03
    (0x3A, 0x0F, 0xB9): "IG Lights (maybe)",    # Data: 02 00 F0 FF FF 80 00 3A 0F B9 C1 01 02 C4 03

    (0x37, 0x11, 0xB8): "IG Serial number",    # Data: 02 00 F0 FF FF 80 00 37 11 B8 04 A3 15 21 00 DD 03

    # 02 00 F0 FF FF 80 00 37 15 BC 01 00 00 03 00 00 00 00 00 04 03 - not configured
    # 02 00 F0 FF FF 80 00 37 15 BC 01 01 01 07 C0 A8 00 17 2B B4 03 - configured and connected with wifi
    (0x37, 0x15, 0xBC): "IG Gateway IP",    # Data: 02 00 F0 FF FF 80 00 37 15 BC 01 01 01 03 00 00 00 00 00 06 03


    # 32769 - comunicating with server
    # = 80 01 or 01 80
    # 02 00 F0 FF FF 80 00 37 0F B6 02 01 80 83 03
    (0x37, 0x0F, 0xB6): "IG GW to Server Comm",    # Data: 02 00 F0 FF FF 80 00 37 0F B6 02 80 01 83 03

}


def src_label(src_hi: int, src_lo: int) -> str:
    return SRC_LABELS.get((src_hi, src_lo), "Unknown")


def cmdsub_label(cmd: int, sub: int, register: int) -> str:
    return CMD_SUB_LABELS.get((cmd, sub, register), "Unknown")


# Output formatting tweak: reduce SRC label column width
SRC_LABEL_COL_WIDTH = 18
CMD_LABEL_COL_WIDTH = 26


@dataclass
class FrameCheckResult:
    ok: bool
    reason: str
    stored: Optional[int]
    computed: Optional[int]
    frame_bytes: List[int]


def parse_hex_bytes_from_line(line: str) -> List[int]:
    out: List[int] = []
    for t in line.strip().split():
        t = t.strip().lower()
        if t.startswith("0x"):
            t = t[2:]
        if 1 <= len(t) <= 2 and all(c in "0123456789abcdef" for c in t):
            out.append(int(t, 16))
    return out


def iter_files(root: str, exts: Tuple[str, ...], recursive: bool) -> Iterable[str]:
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


def fmt_frame(frame: List[int]) -> str:
    return " ".join(f"{b:02X}" for b in frame)


def get_src_cmd_sub_reg(frame: List[int]) -> Tuple[int, int, int, int, int]:
    # Caller must ensure len(frame) >= 10
    return frame[1], frame[2], frame[7], frame[8], frame[9]


def verify_frame_checksum_framed(frame: List[int]) -> FrameCheckResult:
    """
    Assumes framing already validated (starts with 0x02, ends with 0x03).
    """
    if len(frame) == 12:
        return FrameCheckResult(True, "NO_DATA_SECTION_OK", None, None, frame)

    if len(frame) < 13:
        # Still "valid" for analysis, but no checksum can be computed
        return FrameCheckResult(False, f"framed_too_short_checksum(len={len(frame)})", None, None, frame)

    stored = frame[-2]
    start_idx = 10
    end_idx = len(frame) - 3  # inclusive
    if end_idx < start_idx:
        return FrameCheckResult(False, "checksum_range_empty", stored, None, frame)

    computed = sum(frame[start_idx : end_idx + 1]) & 0xFF
    ok = computed == stored
    return FrameCheckResult(ok, "ok" if ok else "mismatch", stored, computed, frame)


def bump_group(stats: Dict[str, int], res: FrameCheckResult) -> None:
    stats["total"] += 1
    if res.ok:
        stats["ok"] += 1
        if res.reason == "NO_DATA_SECTION_OK":
            stats["no_data"] += 1
    elif res.reason == "mismatch":
        stats["bad"] += 1
    else:
        # framed_too_short_checksum / checksum_range_empty
        stats["skipped"] += 1


def print_group_summary(title: str, agg: Dict[Tuple[int, int, int, int, int], Dict[str, int]]) -> None:
    print(f"\n==== {title} (VALID FRAMES ONLY) ====\n")

    items = sorted(
        agg.items(),
        key=lambda kv: (kv[0][0], kv[0][1], kv[0][2], kv[0][3], kv[0][4], -kv[1].get("total", 0)),
    )

    for (src_hi, src_lo, cmd, sub, register), stats in items:
        src_str = f"{src_hi:02X}{src_lo:02X}"
        src_name = src_label(src_hi, src_lo)
        cmd_str = f"{cmd:02X}"
        sub_str = f"{sub:02X}"
        register_str = f"{register:02X}"
        op_name = cmdsub_label(cmd, sub, register)

        # Trim labels to fit columns neatly
        src_name_short = (src_name[: SRC_LABEL_COL_WIDTH - 1] + "…") if len(src_name) > SRC_LABEL_COL_WIDTH else src_name
        op_name_short = (op_name[: CMD_LABEL_COL_WIDTH - 1] + "…") if len(op_name) > CMD_LABEL_COL_WIDTH else op_name

        print(
            f"SRC={src_str:<4} ({src_name_short:<{SRC_LABEL_COL_WIDTH}})  "
            f"CMD={cmd_str} {sub_str} {register_str} "
            f"({op_name_short:<{CMD_LABEL_COL_WIDTH}}) "
            f"total={stats.get('total',0):6d}  "
            f"ok={stats.get('ok',0):6d}  "
            f"no_data={stats.get('no_data',0):6d}  "
            f"bad={stats.get('bad',0):6d}  "
            f"skipped={stats.get('skipped',0):6d}"
        )


def print_counts(title: str, counts: Dict[str, int]) -> None:
    print(f"\n==== {title} ====\n")
    for k in [
        "valid_total",
        "valid_checksum_ok",
        "valid_checksum_bad",
        "valid_no_data_ok",
        "valid_checksum_skipped",
        "broken_total",
        "broken_empty",
        "broken_no_start",
        "broken_incomplete",
        "broken_too_short_header",
    ]:
        print(f"{k:<24} = {counts.get(k, 0)}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("path", help="Folder (or file) of hex dump logs.")
    ap.add_argument(
        "--ext",
        nargs="*",
        default=[".txt", ".log"],
        help="File extensions to include (default: .txt .log). Use empty to include all.",
    )
    ap.add_argument("--no-recursive", action="store_true", help="Do not recurse into subfolders.")
    ap.add_argument("--show-ok", action="store_true", help="Print OK valid frames too.")
    ap.add_argument("--show-skipped", action="store_true", help="Print checksum-skipped valid frames too.")
    ap.add_argument("--show-broken", action="store_true", help="Print broken/rejected lines too.")
    args = ap.parse_args()

    exts = tuple(e.lower() for e in (args.ext or []) if e)
    recursive = not args.no_recursive

    global_agg: Dict[Tuple[int, int, int, int, int], Dict[str, int]] = defaultdict(lambda: defaultdict(int))
    global_counts: Dict[str, int] = defaultdict(int)

    any_files = False

    for filepath in iter_files(args.path, exts, recursive):
        any_files = True
        file_agg: Dict[Tuple[int, int, int, int, int], Dict[str, int]] = defaultdict(lambda: defaultdict(int))
        file_counts: Dict[str, int] = defaultdict(int)

        print(f"\n== Processing {filepath} ==")

        try:
            with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
                for lineno, line in enumerate(f, start=1):
                    frame = parse_hex_bytes_from_line(line)

                    if not frame:
                        file_counts["broken_empty"] += 1
                        file_counts["broken_total"] += 1
                        global_counts["broken_empty"] += 1
                        global_counts["broken_total"] += 1
                        if args.show_broken:
                            print(f"[BROKEN:empty] {os.path.basename(filepath)}:{lineno}  {line.rstrip()}")
                        continue

                    # Enforce strict one-line framing
                    if frame[0] != 0x02:
                        file_counts["broken_no_start"] += 1
                        file_counts["broken_total"] += 1
                        global_counts["broken_no_start"] += 1
                        global_counts["broken_total"] += 1
                        if args.show_broken:
                            print(f"[BROKEN:no_start] {os.path.basename(filepath)}:{lineno}  {fmt_frame(frame)}")
                        continue

                    if frame[-1] != 0x03:
                        file_counts["broken_incomplete"] += 1
                        file_counts["broken_total"] += 1
                        global_counts["broken_incomplete"] += 1
                        global_counts["broken_total"] += 1
                        if args.show_broken:
                            print(f"[BROKEN:incomplete] {os.path.basename(filepath)}:{lineno}  {fmt_frame(frame)}")
                        continue

                    # Must be long enough to contain SRC+CMD+SUB to be "valid for analysis"
                    # (prevents nonsense like a wrapped tail "02 88 03" being treated as SRC=8803)
                    if len(frame) < 9:
                        file_counts["broken_too_short_header"] += 1
                        file_counts["broken_total"] += 1
                        global_counts["broken_too_short_header"] += 1
                        global_counts["broken_total"] += 1
                        if args.show_broken:
                            print(f"[BROKEN:too_short_header] {os.path.basename(filepath)}:{lineno}  {fmt_frame(frame)}")
                        continue

                    # VALID frame for analysis
                    file_counts["valid_total"] += 1
                    global_counts["valid_total"] += 1

                    res = verify_frame_checksum_framed(frame)

                    if res.ok:
                        file_counts["valid_checksum_ok"] += 1
                        global_counts["valid_checksum_ok"] += 1
                        if res.reason == "NO_DATA_SECTION_OK":
                            file_counts["valid_no_data_ok"] += 1
                            global_counts["valid_no_data_ok"] += 1
                    elif res.reason == "mismatch":
                        file_counts["valid_checksum_bad"] += 1
                        global_counts["valid_checksum_bad"] += 1
                    else:
                        file_counts["valid_checksum_skipped"] += 1
                        global_counts["valid_checksum_skipped"] += 1

                    src_hi, src_lo, cmd, sub, register = get_src_cmd_sub_reg(frame)
                    key = (src_hi, src_lo, cmd, sub, register)
                    bump_group(file_agg[key], res)
                    bump_group(global_agg[key], res)

                    src_str = f"{src_hi:02X}{src_lo:02X}"
                    src_name = src_label(src_hi, src_lo)
                    cmd_str = f"{cmd:02X}"
                    sub_str = f"{sub:02X}"
                    register_str = f"{register:02X}"
                    op_name = cmdsub_label(cmd, sub, register)

                    if res.ok:
                        if args.show_ok:
                            print(
                                f"[OK:{res.reason}] {os.path.basename(filepath)}:{lineno} "
                                f"SRC={src_str} ({src_name}) "
                                f"CMD={cmd_str} SUB={sub_str} REG={register_str} ({op_name}) "
                                f"{fmt_frame(frame)}"
                            )
                    elif res.reason == "mismatch":
                        print(
                            f"[BAD] {os.path.basename(filepath)}:{lineno} "
                            f"SRC={src_str} ({src_name}) "
                            f"CMD={cmd_str} SUB={sub_str} REG={register_str} ({op_name}) "
                            f"stored={res.stored:02X} computed={res.computed:02X}  {fmt_frame(frame)}"
                        )
                    else:
                        if args.show_skipped:
                            print(
                                f"[SKIP:{res.reason}] {os.path.basename(filepath)}:{lineno} "
                                f"SRC={src_str} ({src_name}) "
                                f"CMD={cmd_str} SUB={sub_str} REG={register_str} ({op_name}) "
                                f"{fmt_frame(frame)}"
                            )

        except OSError as e:
            print(f"[ERROR] Could not read {filepath}: {e}")
            continue

        print_group_summary(f"PER-FILE SUMMARY ({os.path.basename(filepath)})", file_agg)
        print_counts(f"PER-FILE COUNTS ({os.path.basename(filepath)})", file_counts)

    if not any_files:
        print(f"No matching files found at: {args.path}")
        return

    print_group_summary("GLOBAL SUMMARY (ALL FILES)", global_agg)
    print_counts("GLOBAL COUNTS (ALL FILES)", global_counts)


if __name__ == "__main__":
    main()