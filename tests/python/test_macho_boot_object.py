"""The Mach-O boot object carries the raw image unchanged and has no __TEXT_BOOT_EXEC."""
import importlib.util
from pathlib import Path
import plistlib
import struct

import pytest

REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("macho_boot_object", REPO / "tools/macho_boot_object.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)

CONFIG_MAGIC = b"AURORA-S1-CFG01\0"


def raw_image(size=0x40000):
    raw = bytearray(size)
    raw[:8] = bytes.fromhex("090680d2b4020014")  # mov x9, '0'; b cpu_reset
    raw[0x808:0x810] = b"AURALOAD"
    raw[0x20000:0x20000 + len(CONFIG_MAGIC)] = CONFIG_MAGIC
    raw[-8:] = b"STACKBOT"
    return bytes(raw)


def commands(blob, offset=0):
    magic, cpu, subtype, filetype, count, size, flags, _ = struct.unpack_from("<8I", blob, offset)
    assert magic == 0xFEEDFACF and cpu == 0x0100000C
    position = offset + 32
    result = []
    for _ in range(count):
        cmd, length = struct.unpack_from("<2I", blob, position)
        result.append((cmd, blob[position:position + length]))
        position += length
    assert position == offset + 32 + size
    return filetype, flags, result


def segments(cmds):
    found = {}
    for cmd, body in cmds:
        if cmd == tool.LC_SEGMENT_64:
            name = body[8:24].rstrip(b"\0").decode()
            found[name] = struct.unpack_from("<4Q", body, 24)
    return found


def test_layout_is_linear_and_has_no_boot_exec_segment():
    raw = raw_image()
    blob = tool.wrap(raw)
    filetype, _, cmds = commands(blob)
    assert filetype == tool.MH_FILESET
    segs = segments(cmds)
    assert list(segs) == ["__TEXT", "__PRELINK_TEXT", "__TEXT_EXEC", "__PRELINK_INFO",
                          "__DATA", "__LINKEDIT"]
    assert "__TEXT_BOOT_EXEC" not in segs and "__DATA_SPTM" not in segs
    end = 0
    for vmaddr, vmsize, fileoff, filesize in segs.values():
        assert fileoff == vmaddr - tool.VA_BASE == end
        assert filesize == vmsize and vmsize % tool.PAGE == 0
        end += vmsize
    assert end == len(blob)
    assert segs["__TEXT"][2] == 0


def test_raw_image_and_entry_are_preserved():
    raw = raw_image()
    blob = tool.wrap(raw)
    _, _, cmds = commands(blob)
    image = segments(cmds)["__TEXT_EXEC"]
    assert blob[image[2]:image[2] + len(raw)] == raw
    (thread,) = [body for cmd, body in cmds if cmd == tool.LC_UNIXTHREAD]
    registers = struct.unpack_from("<34Q", thread, 16)
    assert registers[32] == image[0] + 0x800
    assert registers[16] == image[0]
    # A Stage 1 configuration block can still be found and filled by its magic.
    assert blob.count(CONFIG_MAGIC) == 1


def test_primary_member_and_prelink_info():
    blob = tool.wrap(raw_image())
    _, _, cmds = commands(blob)
    segs = segments(cmds)
    (entry,) = [body for cmd, body in cmds if cmd == tool.LC_FILESET_ENTRY]
    vmaddr, fileoff, name_offset = struct.unpack_from("<QQI", entry, 8)
    assert entry[name_offset:].rstrip(b"\0") == b"com.apple.kernel"
    assert (vmaddr, fileoff) == segs["__PRELINK_TEXT"][:1] + segs["__PRELINK_TEXT"][2:3]
    filetype, flags, member = commands(blob, fileoff)
    assert filetype == tool.MH_EXECUTE and flags & 0x80000000
    assert segments(member)["__TEXT_EXEC"] == segs["__TEXT_EXEC"]
    info = segs["__PRELINK_INFO"]
    prelink = plistlib.loads(blob[info[2]:info[2] + info[3]].rstrip(b"\0"))
    assert prelink["_PrelinkInfoDictionary"] == []


def test_fixup_table_is_empty():
    blob = tool.wrap(raw_image())
    _, _, cmds = commands(blob)
    (fixups,) = [body for cmd, body in cmds if cmd == tool.LC_DYLD_CHAINED_FIXUPS]
    offset, size = struct.unpack_from("<2I", fixups, 8)
    header = struct.unpack_from("<7I", blob, offset)
    count = struct.unpack_from("<I", blob, offset + header[1])[0]
    assert count == 6 and not any(struct.unpack_from(f"<{count}I", blob, offset + header[1] + 4))
    assert size == 28 + 4 + 4 * count


def test_unaligned_raw_image_is_padded():
    raw = raw_image() + b"logo"
    blob = tool.wrap(raw)
    segs = segments(commands(blob)[2])
    assert segs["__TEXT_EXEC"][1] == len(raw_image()) + tool.PAGE


@pytest.mark.parametrize("bad", [bytes(0x40000), tool.wrap(raw_image())])
def test_rejects_inputs_that_are_not_raw_aura_images(bad):
    with pytest.raises(ValueError):
        tool.wrap(bad)
