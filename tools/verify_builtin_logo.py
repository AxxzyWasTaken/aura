#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check that an aura binary links the selected boot logo at every size."""

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


def verify(path: Path, name: str, logo_dir: Path) -> None:
    found = embedded_logos(path.read_bytes())
    expected = {size: (logo_dir / f"{name}_{size}.bin").read_bytes() for size in SIZES}
    missing = [size for size, pixels in expected.items() if pixels not in found]
    if missing:
        raise SystemExit(f"{path}: missing {name} logo sizes {missing}")
    print(f"{path}: verified {name} logos at 48, 128, and 256 px")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("logo", nargs="?", default="aurora")
    parser.add_argument("--logo-dir", type=Path, default=ROOT / "data")
    args = parser.parse_args()
    verify(args.binary, args.logo, args.logo_dir if args.logo_dir.is_absolute() else ROOT / args.logo_dir)
