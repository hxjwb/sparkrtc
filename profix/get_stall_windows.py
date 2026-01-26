#!/usr/bin/env python3
import argparse
import os
import re


BLOCK_START = "[STALL_HEADER]"
BLOCK_SECTIONS = {
    "[STALL_HEADER]",
    "[WINDOW_SUMMARY]",
    "[FRAME_DIGEST]",
    "[PACKET_DIGEST]",
    "[REPAIR_DIGEST]",
}

INDEX_LINE_RE = re.compile(r"^\s*\d+\)\s+")


def is_block_line(line: str) -> bool:
    stripped = line.strip()
    if not stripped:
        return False
    if stripped in BLOCK_SECTIONS:
        return True
    if INDEX_LINE_RE.match(stripped):
        return True
    if "=" in stripped:
        # key=value lines used by the block sections
        return True
    return False


def extract_blocks(lines):
    blocks = []
    current = []
    in_block = False
    saw_repair_header = False

    for line in lines:
        # Strip log prefixes like "[time][pid] (file:line): "
        stripped = line.strip()
        if "): " in stripped:
            stripped = stripped.split("): ", 1)[1].strip()
        if stripped == BLOCK_START:
            if current:
                blocks.append(current)
                current = []
            in_block = True
            saw_repair_header = False
            current.append(stripped)
            continue

        if not in_block:
            continue

        if stripped in BLOCK_SECTIONS:
            if stripped == "[REPAIR_DIGEST]":
                saw_repair_header = True
            current.append(stripped)
            continue

        if is_block_line(stripped):
            current.append(stripped)
            continue

        # End block once we passed repair section and hit non-block line.
        if saw_repair_header:
            if current:
                blocks.append(current)
            current = []
            in_block = False
            saw_repair_header = False

    if current:
        blocks.append(current)
    return blocks


def main():
    parser = argparse.ArgumentParser(
        description="Extract stall report blocks from log.")
    parser.add_argument(
        "--log",
        default="logs/send_0",
        help="Path to send log file (default: logs/send_0)",
    )
    parser.add_argument(
        "--out-dir",
        default="logs/stall_windows",
        help="Output directory for extracted blocks",
    )
    args = parser.parse_args()

    with open(args.log, "r", encoding="utf-8", errors="ignore") as f:
        lines = f.readlines()

    blocks = extract_blocks(lines)
    os.makedirs(args.out_dir, exist_ok=True)

    for idx, block in enumerate(blocks, start=1):
        out_path = os.path.join(args.out_dir, f"stall_window_{idx}.txt")
        with open(out_path, "w", encoding="utf-8") as out:
            out.write("\n".join(block))
            out.write("\n")

    print(f"Extracted {len(blocks)} stall windows to {args.out_dir}")


if __name__ == "__main__":
    main()
