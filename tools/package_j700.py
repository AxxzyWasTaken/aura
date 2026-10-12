#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Assemble generic Stage 1 and ESP Stage 2 from pinned build output folders."""

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess


REPO = Path(__file__).resolve().parents[1]
REQUIRED = ("#define RELEASE", "#define CHAINLOADING")


def build_identity(folder: Path, commit: str, stage: str) -> tuple[str, str]:
    info = (folder / "BUILD-INFO.txt").read_text()
    fields = dict(line.split(": ", 1) for line in info.splitlines() if ": " in line)
    if fields.get("kind") != "m1n1" or fields.get("commit") != commit:
        raise ValueError(f"{folder}: source commit mismatch")
    if fields.get("dirty-files") != "0":
        raise ValueError(f"{folder}: dirty build")
    cfg = (folder / "build_cfg.h").read_text()
    if any(define not in cfg.splitlines() for define in REQUIRED):
        raise ValueError(f"{folder}: missing release/chainloading define")
    if "#define BUILTIN_LOGO aurora" not in cfg.splitlines() or \
            "BUILTIN_LOGO=aurora" not in fields.get("make-vars", "").split() or \
            any(var.startswith("LOGO=") for var in fields.get("make-vars", "").split()):
        raise ValueError(f"{folder}: expected linked Aurora logo without appended payload")
    image = (folder / "aura.bin").read_bytes()
    stage2 = stage == "stage2"
    if ("#define ESP_STAGE2" in cfg.splitlines()) != stage2:
        raise ValueError(f"{folder}: wrong Stage 1/ESP Stage 2 build role")
    if not image.endswith(b"STACKBOT") or image.count(b"AURORA-S1-CFG01\0") != (0 if stage2 else 1):
        raise ValueError(f"{folder}: not a bare image of the requested stage")
    if not (folder / "aura-raw.elf").is_file():
        raise ValueError(f"{folder}: raw ELF missing")
    return fields["build_tag"], fields["make-vars"]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage1-out", type=Path, required=True)
    parser.add_argument("--stage2-out", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip()
    tag1, vars1 = build_identity(args.stage1_out, commit, "stage1")
    tag2, vars2 = build_identity(args.stage2_out, commit, "stage2")
    if tag1 != tag2:
        raise ValueError("Stage 1 and Stage 2 build tags differ")
    if args.output.exists():
        raise ValueError("output directory already exists")
    args.output.mkdir(parents=True)

    for stage, folder in (("stage1", args.stage1_out), ("stage2", args.stage2_out)):
        shutil.copy2(folder / "aura.bin", args.output / f"m1n1-{stage}.bin")
        shutil.copy2(folder / "aura-raw.elf", args.output / f"m1n1-{stage}-raw.elf")
        shutil.copy2(folder / "BUILD-INFO.txt", args.output / f"BUILD-INFO-{stage}.txt")
        shutil.copy2(folder / "build_cfg.h", args.output / f"build_cfg-{stage}.h")

    shutil.copy2(REPO / "docs/j700-install.md", args.output / "ESP-AND-INSTALL.md")
    shutil.copy2(REPO / "LICENSE", args.output / "LICENSE")
    shutil.copytree(REPO / "LICENSES", args.output / "LICENSES")
    shutil.copytree(REPO / "3rdparty_licenses", args.output / "3rdparty_licenses")

    (args.output / "SOURCE-AND-BUILDS.txt").write_text(
        f"source commit: {commit}\nbuild tag: {tag1}\n"
        f"stage1: bb-build.sh m1n1 {REPO} {args.stage1_out.name} {vars1}\n"
        f"stage2: bb-build.sh m1n1 {REPO} {args.stage2_out.name} {vars2}\n"
    )
    lines = []
    for path in sorted(args.output.rglob("*")):
        if path.is_file():
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            lines.append(f"{digest}  {path.relative_to(args.output)}")
    (args.output / "SHA256SUMS").write_text("\n".join(lines) + "\n")
    print(f"{args.output}: {len(lines)} pinned files, source {commit[:12]}, tag {tag1}")


if __name__ == "__main__":
    main()
