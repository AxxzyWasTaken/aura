#!/usr/bin/env python3
"""Read the opt-in T8122 ADT metadata export from Linux. Output stays local.

Raw ADT cells are little-endian, unlike ordinary FDT cells. Numeric views are
explicitly untyped: consult the ADT schema before assigning units or addresses.
This tool never reads registers, physical memory or firmware images.
"""
import argparse
import json
from pathlib import Path


def read_export(root):
    if (root / 'schema-version').read_bytes() != b'\0\0\0\1':
        raise ValueError('Unsupported diagnostic schema')
    if (root / 'encoding').read_bytes() != b'raw-adt-little-endian\0':
        raise ValueError('Unexpected diagnostic encoding')
    if (root / 'complete').read_bytes() != b'':
        raise ValueError('Invalid completion marker')
    nodes = {}
    total = 0
    for node in sorted(root.iterdir()):
        if not node.is_dir():
            continue
        props = {}
        for prop in sorted(node.glob('adt,*')):
            data = prop.read_bytes()
            total += len(data)
            if len(data) > 32768 or total > 49152:
                raise ValueError('Diagnostic data exceeds export bounds')
            item = {'bytes': len(data), 'hex': data.hex()}
            if prop.name in ('adt,compatible', 'adt,segment-names'):
                item['strings'] = data.rstrip(b'\0').decode('ascii').split('\0')
            else:
                for size in (4, 8):
                    if data and len(data) % size == 0:
                        item[f'u{size * 8}_le'] = [
                            int.from_bytes(data[i:i + size], 'little')
                            for i in range(0, len(data), size)
                        ]
            props[prop.name.removeprefix('adt,')] = item
        nodes[node.name] = {
            'adt_path': (node / 'adt-path').read_bytes().rstrip(b'\0').decode('ascii'),
            'properties': props,
        }
    if not {'sgx', 'gfx-asc'} <= nodes.keys():
        raise ValueError('Required GPU ADT nodes absent')
    result = {'schema_version': 1, 'nodes': nodes}
    revision_path = root / 'exporter-revision'
    if revision_path.exists():
        revision = revision_path.read_bytes()
        if len(revision) != 4 or int.from_bytes(revision, 'big') != 2:
            raise ValueError('Unsupported exporter revision')
        usable = (root / 'usable-memory').read_bytes()
        if len(usable) != 16:
            raise ValueError('Invalid usable-memory extent')
        base = int.from_bytes(usable[:8], 'big')
        size = int.from_bytes(usable[8:], 'big')
        if not size or base + size > 1 << 64:
            raise ValueError('Invalid usable-memory bounds')
        result.update(exporter_revision=2, usable_memory={'base': base, 'size': size})
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(
        '/sys/firmware/devicetree/base/chosen/asahi,t8122-gpu-diagnostics'))
    args = parser.parse_args()
    try:
        result = read_export(args.root)
    except (OSError, ValueError) as error:
        parser.exit(1, f'T8122 diagnostics unavailable or invalid: {error}\n')
    print(json.dumps(result, indent=2))
