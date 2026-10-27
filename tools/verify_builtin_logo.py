#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check the actual gzip-compressed logos in an m1n1 binary."""

import argparse
from pathlib import Path
import zlib

ROOT = Path(__file__).resolve().parents[1]
SIZES = (48, 128, 256)


def embedded_logos(binary: bytes) -> set[bytes]:
    logos = set()
    offset = 0
    while (offset := binary.find(b"\x1f\x8b\x08", offset)) >= 0:
        try:
            stream = zlib.decompressobj(16 + zlib.MAX_WBITS)
            pixels = stream.decompress(binary[offset:])
            if stream.eof and len(pixels) in (size * size * 4 for size in SIZES):
                logos.add(pixels)
        except zlib.error:
            pass
        offset += 3
    return logos


def verify(path: Path, name: str) -> None:
    found = embedded_logos(path.read_bytes())
    expected = {size: (ROOT / "data" / f"{name}_{size}.bin").read_bytes() for size in SIZES}
    asahi = {size: (ROOT / "data" / f"bootlogo_{size}.bin").read_bytes() for size in SIZES}
    missing = [size for size, pixels in expected.items() if pixels not in found]
    unexpected = [size for size, pixels in asahi.items() if pixels in found and name != "bootlogo"]
    if missing or unexpected:
        raise SystemExit(f"{path}: missing {name} sizes {missing}; embedded Asahi sizes {unexpected}")
    print(f"{path}: verified {name} logos at 48, 128, and 256 px")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("logo", nargs="?", default="aurora")
    args = parser.parse_args()
    verify(args.binary, args.logo)
