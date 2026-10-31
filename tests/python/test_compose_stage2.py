# SPDX-License-Identifier: GPL-2.0-only
"""A kernel DT refresh must replace embedded bytes, with reproducible provenance."""

import gzip
import hashlib
import json
from pathlib import Path
import struct
import sys

import pytest

from tools import compose_stage2 as tool


@pytest.fixture
def inputs(tmp_path):
    aura = tmp_path / "aura.bin"
    aura.write_bytes(b"bare-stage2-STACKBOT")
    uboot = tmp_path / "u-boot-nodtb.bin"
    uboot.write_bytes(bytes(56) + b"ARM\x64" + bytes(32))
    # Minimal root node, strings block and reservation terminator.
    tree = struct.pack(">I", 1) + bytes(4) + struct.pack(">II", 2, 9)
    dtb = tmp_path / "board.dtb"
    dtb.write_bytes(struct.pack(">10I", 0xd00dfeed, 56 + len(tree), 56,
                               56 + len(tree), 40, 17, 16, 0, 0, len(tree))
                    + bytes(16) + tree)
    return aura, dtb, uboot


def test_exact_dtbs_and_deterministic_uboot(inputs):
    aura, dtb, uboot = inputs
    other = dtb.with_name("other.dtb")
    other.write_bytes(dtb.read_bytes())
    args = aura, [dtb, other], uboot, "a" * 40, b"chosen.bootargs=test\n"
    image, manifest = tool.compose(*args)
    assert tool.compose(*args) == (image, manifest)
    assert manifest["kernel_commit"] == "a" * 40
    assert manifest["sha256"] == hashlib.sha256(image).hexdigest()
    for part, source in zip(manifest["parts"], (aura, dtb, other, uboot)):
        data = image[part["offset"]:part["offset"] + part["size"]]
        assert hashlib.sha256(data).hexdigest() == part["sha256"]
        assert (gzip.decompress(data) if part["kind"] == "uboot-gzip" else data) == source.read_bytes()
    assert image.endswith(b"chosen.bootargs=test\n")
    assert not image.endswith(bytes(4))


def test_fresh_dt_replaces_old_set(inputs):
    aura, dtb, uboot = inputs
    old, _ = tool.compose(aura, [dtb], uboot, "a" * 40)
    new = bytearray(dtb.read_bytes())
    # boot_cpuid_phys is a valid header field; change it to distinguish builds.
    struct.pack_into(">I", new, 28, 1)
    dtb.write_bytes(new)
    image, manifest = tool.compose(aura, [dtb], uboot, "b" * 40)
    part = manifest["parts"][1]
    assert image != old
    assert image[part["offset"]:part["offset"] + part["size"]] == new
    assert manifest["kernel_commit"] == "b" * 40


@pytest.mark.parametrize("field,value", [(0, 0), (4, 99999), (8, 99999), (12, 99999),
                                         (16, 99999), (20, 16), (24, 18), (32, 99999), (36, 99999)])
def test_reject_bad_fdt_header(inputs, field, value):
    aura, dtb, uboot = inputs
    data = bytearray(dtb.read_bytes())
    struct.pack_into(">I", data, field, value)
    dtb.write_bytes(data)
    with pytest.raises(ValueError):
        tool.compose(aura, [dtb], uboot, "a" * 40)


@pytest.mark.parametrize("variables", [b"x=y\0\n", b"x=y", b"invalid\n", b"=value\n", b"x=\xff\n"])
def test_reject_hidden_or_invalid_variables(inputs, variables):
    aura, dtb, uboot = inputs
    with pytest.raises(ValueError):
        tool.compose(aura, [dtb], uboot, "a" * 40, variables)


def test_reject_appended_or_stage1_aura(inputs):
    aura, dtb, uboot = inputs
    for data in (b"AURORA-S1-CFG01\0STACKBOT", b"STACKBOTold-dtb-set"):
        aura.write_bytes(data)
        with pytest.raises(ValueError):
            tool.compose(aura, [dtb], uboot, "a" * 40)


def test_reject_missing_or_duplicate_dtbs_and_unpinned_commit(inputs):
    aura, dtb, uboot = inputs
    for trees, commit in (([], "a" * 40), ([dtb, dtb], "a" * 40), ([dtb], "main")):
        with pytest.raises(ValueError):
            tool.compose(aura, trees, uboot, commit)


def test_reject_truncated_dtb_and_wrong_uboot(inputs):
    aura, dtb, uboot = inputs
    uboot.write_bytes(b"not raw arm64")
    with pytest.raises(ValueError):
        tool.compose(aura, [dtb], uboot, "a" * 40)
    dtb.write_bytes(b"short")
    with pytest.raises(ValueError):
        tool.dtb_bytes(dtb)


def test_cli_manifest_and_no_overwrite(inputs, monkeypatch, tmp_path):
    aura, dtb, uboot = inputs
    output = tmp_path / "stage2.bin"
    monkeypatch.setattr(sys, "argv", ["compose_stage2.py", "--aura", str(aura),
                                    "--dtb", str(dtb), "--uboot", str(uboot),
                                    "--kernel-commit", "a" * 40, "--output", str(output)])
    tool.main()
    receipt = json.loads(output.with_suffix(".bin.json").read_text())
    assert receipt["sha256"] == hashlib.sha256(output.read_bytes()).hexdigest()
    original = output.read_bytes()
    with pytest.raises(SystemExit, match="overwrite"):
        tool.main()
    assert output.read_bytes() == original
