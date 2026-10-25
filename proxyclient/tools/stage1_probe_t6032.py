#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Stage 1 probe for the Mac Studio (M3 Ultra, T6032): read values that only a running loader can read.

Usage: stage1_probe_t6032.py OUTDIR [--power-gpu]
       stage1_probe_t6032.py --selftest

Reads only, with one exception: --power-gpu turns on the GPU power domain through the loader's
pmgr call, so the hardware-id words can be read. That setting is lost at the next restart.
Without --power-gpu a failed read is reported and nothing is changed. Every read is guarded:
a bus fault is counted and reported, it does not stop the probe. It never writes a GPU register
and never starts firmware.

Writes under OUTDIR: adt.bin (the device tree as the loader holds it; PRIVATE), adt.txt (same, text; PRIVATE),
gpu-text.bin (the GPU firmware image iBoot loaded; PRIVATE), probe.txt (the report), SHA256SUMS.
"""
import hashlib
import os
import struct
import sys

HWID = (0xD04000, 0xD04010, 0xD04014, 0xD04018, 0xD0401C)  # sgx offsets, see the layout notes
TAG = b"GKTS"


def decode_hwid(w):
    """Words at sgx+0xd04000/10/14/18/1c -> readable facts (kext readChipInfo reading; INFERENCE until measured)."""
    v, c = w[0], w[1]
    return {
        "family": v >> 24,
        "variant": (v >> 16) & 0xFF,
        "per_die": (c >> 8) & 0xFF,
        "dies": (c >> 16) & 0xF,
    }


def text_digests(text):
    """sha256 of the image, and of the image with the 8 bytes after each 'GKTS' tag zeroed (one line per tag)."""
    out = [("raw", hashlib.sha256(text).hexdigest())]
    pos = text.find(TAG)
    while pos >= 0:
        b = bytearray(text)
        b[pos + len(TAG):pos + len(TAG) + 8] = bytes(8)
        out.append((f"tag@{pos:#x} zeroed", hashlib.sha256(bytes(b)).hexdigest()))
        pos = text.find(TAG, pos + 1)
    return out


def segments(raw):
    """gfx-asc 'segment-ranges': 32-byte entries (phys, iova, phys, flags<<32 | size)."""
    return [struct.unpack_from("<4Q", raw, o) for o in range(0, len(raw) - 31, 32)]


def selftest():
    assert decode_hwid([0x07040000, 0x00021000]) == {"family": 7, "variant": 4, "per_die": 0x10, "dies": 2}
    raw = struct.pack("<4Q", 0x1000, 0xFFFFFC0000000000, 0x1000, 0x100060000)
    assert segments(raw)[0][3] & 0xFFFFFFFF == 0x60000
    t = b"a" * 10 + TAG + bytes(range(1, 9)) + b"z"
    d = text_digests(t)
    assert len(d) == 2 and d[0][1] != d[1][1]
    assert d[1][1] == hashlib.sha256(b"a" * 10 + TAG + bytes(8) + b"z").hexdigest()
    print("selftest ok")


def main():
    if "--selftest" in sys.argv:
        return selftest()
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    from m1n1.proxy import GUARD, M1N1Proxy, UartInterface
    from m1n1.proxyutils import ProxyUtils, bootstrap_port

    # Not m1n1.setup: that module writes the PMU panic counter at import time.
    iface = UartInterface()
    p = M1N1Proxy(iface, debug=False)
    bootstrap_port(iface, p)
    u = ProxyUtils(p)
    rep = open(os.path.join(out, "probe.txt"), "w")

    def log(s):
        print(s)
        rep.write(s + "\n")
        rep.flush()

    log("probe: stage1_probe_t6032 v1")
    log(f"loader base: {u.base:#x}")
    adt_raw = u.get_adt()
    open(os.path.join(out, "adt.bin"), "wb").write(adt_raw)
    dt = u.adt
    open(os.path.join(out, "adt.txt"), "w").write(str(dt))
    log(f"model: {dt.model}  target: {dt.target_type}  adt bytes: {len(adt_raw)}")

    # 1. GPU firmware image iBoot loaded (plain memory read, no GPU register involved)
    asc = dt["/arm-io/gfx-asc"]
    segs = segments(asc._properties["segment-ranges"])  # set by iBoot at run time; not in the IPSW tree
    log("gfx-asc segments (phys, iova, size): " + "; ".join(f"{s[0]:#x} {s[1]:#x} {s[3] & 0xFFFFFFFF:#x}" for s in segs))
    text = iface.readmem(segs[0][0], segs[0][3] & 0xFFFFFFFF)
    open(os.path.join(out, "gpu-text.bin"), "wb").write(text)
    for name, dg in text_digests(text):
        log(f"gpu text {name}: {dg}")

    # 2. hardware-id words: a bus fault here is reported, not fatal
    base = dt["/arm-io/sgx"].get_reg(0)[0]
    log(f"sgx base: {base:#x}")
    if "--power-gpu" in sys.argv:
        log("power-gpu: pmgr_adt_power_enable('/arm-io/sgx') (lost at restart)")
        try:
            log(f"  returned {p.pmgr_adt_power_enable('/arm-io/sgx')}")
        except Exception as e:
            log(f"  failed: {e}")
    words = []
    p.set_exc_guard(GUARD.MARK | GUARD.SILENT)
    for off in HWID:
        before = p.get_exc_count()
        v = p.read32(base + off)
        faulted = p.get_exc_count() != before
        words.append(None if faulted else v)
        log(f"sgx+{off:#x}: " + ("BUS FAULT" if faulted else f"{v:#010x}"))
    p.set_exc_guard(GUARD.OFF)
    if None not in words:
        log(f"decoded (inference): {decode_hwid(words)}")
    else:
        log("hardware-id words not read; rerun with --power-gpu if that was not used")

    rep.close()
    os.system(f"cd '{out}' && shasum -a 256 * > SHA256SUMS 2>/dev/null || sha256sum * > SHA256SUMS")


if __name__ == "__main__":
    main()
