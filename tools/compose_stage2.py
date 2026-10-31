#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compose Aura + kernel-build DTBs + gzip U-Boot without reusing an old DT set."""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import struct


def digest(data):
    return hashlib.sha256(data).hexdigest()


def dtb_bytes(path):
    data = path.read_bytes()
    if len(data) < 40:
        raise ValueError(f"{path}: truncated FDT header")
    magic, total, off_struct, off_strings, off_reserve, version, last, _, strings, tree = \
        struct.unpack_from(">10I", data)
    if magic != 0xd00dfeed or total != len(data) or version != 17 or last > 17:
        raise ValueError(f"{path}: expected one complete version 17 DTB")
    if not (40 <= off_reserve <= total - 16 and 40 <= off_struct <= total - tree
            and 40 <= off_strings <= total - strings):
        raise ValueError(f"{path}: invalid FDT block bounds")
    return data


def compose(aura, dtbs, uboot, kernel_commit, variables=b""):
    if not re.fullmatch(r"[0-9a-f]{40}", kernel_commit):
        raise ValueError("kernel commit must be a full lowercase Git SHA")
    image = aura.read_bytes()
    if not image.endswith(b"STACKBOT") or b"AURORA-S1-CFG01\0" in image:
        raise ValueError("expected a bare ESP_STAGE2=1 Aura image")
    if not dtbs:
        raise ValueError("at least one freshly built DTB is required")
    if variables:
        # Stage 1 appends its handoff variables and the final terminator in RAM.
        if not variables.endswith(b"\n") or b"\0" in variables:
            raise ValueError("variables must be newline terminated without NULs")
        for line in variables.decode("ascii").splitlines():
            if "=" not in line or not line.split("=", 1)[0]:
                raise ValueError("expected key=value variable lines")
    parts = [image]
    entries = [dict(kind="aura", name=aura.name, offset=0, size=len(image), sha256=digest(image))]
    seen = set()
    for path in dtbs:
        data = dtb_bytes(path)
        if path.name in seen:
            raise ValueError(f"duplicate DTB name: {path.name}")
        seen.add(path.name)
        entries.append(dict(kind="dtb", name=path.name, offset=sum(map(len, parts)),
                            size=len(data), sha256=digest(data)))
        parts.append(data)
    raw = uboot.read_bytes()
    # The payload reader accepts a raw arm64 Image header; double compression
    # or a firmware container is not a u-boot-nodtb.bin input.
    if len(raw) < 64 or raw[0x38:0x3c] != b"ARM\x64":
        raise ValueError("expected raw arm64 u-boot-nodtb.bin")
    compressed = gzip.compress(raw, compresslevel=9, mtime=0)
    entries.append(dict(kind="uboot-gzip", name=uboot.name, offset=sum(map(len, parts)),
                        size=len(compressed), sha256=digest(compressed), raw_sha256=digest(raw)))
    parts += [compressed, variables]
    output = b"".join(parts)
    return output, dict(schema=1, kernel_commit=kernel_commit, parts=entries,
                        variables_sha256=digest(variables), size=len(output), sha256=digest(output))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aura", type=Path, required=True)
    parser.add_argument("--dtb", type=Path, action="append", required=True)
    parser.add_argument("--uboot", type=Path, required=True)
    parser.add_argument("--kernel-commit", required=True)
    parser.add_argument("--variables", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output, manifest = compose(args.aura, args.dtb, args.uboot, args.kernel_commit,
                               args.variables.read_bytes() if args.variables else b"")
    receipt = args.output.with_suffix(args.output.suffix + ".json")
    if args.output.exists() or receipt.exists():
        raise SystemExit("refusing to overwrite output or manifest")
    with args.output.open("xb") as stream:
        stream.write(output)
    with receipt.open("x") as stream:
        json.dump(manifest, stream, indent=2)
        stream.write("\n")
    print(f"{args.output}: {manifest['sha256']}")


if __name__ == "__main__":
    main()
