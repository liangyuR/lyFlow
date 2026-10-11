import argparse
import json
import struct
from pathlib import Path


def varint(n):
    out = bytearray()
    while n > 127:
        out.append((n & 127) | 128)
        n >>= 7
    out.append(n)
    return bytes(out)


def field(n, value):
    if isinstance(value, int):
        return varint(n << 3) + varint(value)
    if isinstance(value, str):
        value = value.encode()
    return varint((n << 3) | 2) + varint(len(value)) + value


def value_info(name, dims):
    shape = b''.join(field(1, field(2 if isinstance(d, str) else 1, d)) for d in dims)
    return field(1, name) + field(2, field(1, field(1, 1) + field(2, shape)))


def model(node, inputs, outputs, initializer=b''):
    graph = field(1, node) + field(2, 'lyflow_synthetic_release')
    if initializer:
        graph += field(5, initializer)
    graph += field(11, inputs) + field(12, outputs)
    return field(1, 8) + field(2, 'lyflow-release-fixture') + field(7, graph) + field(8, field(2, 13))


def generate(output):
    output.mkdir(parents=True, exist_ok=True)
    pads = field(1, 6) + field(2, 7) + field(8, 'pads') + field(9, struct.pack('<6q', 0, 0, 0, 0, 2, 0))
    node = field(1, 'input') + field(1, 'pads') + field(2, 'output') + field(4, 'Pad')
    (output / 'channels-6-to-8.onnx').write_bytes(model(node, value_info('input', ['N', 6, 1280]), value_info('output', ['N', 8, 1280]), pads))
    (output / 'provenance.json').write_text(json.dumps({'origin': 'repository-generated synthetic fixtures', 'customerData': False, 'nativeTestModel': 'channels-6-to-8.onnx'}, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', type=Path, required=True)
    generate(parser.parse_args().output_dir)
