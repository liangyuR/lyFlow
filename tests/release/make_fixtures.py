import argparse
import json
import math
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
    node = field(1, 'input') + field(2, 'output') + field(4, 'Identity')
    (output / 'identity.onnx').write_bytes(model(node, value_info('input', [1, 1, 8, 8]), value_info('output', [1, 1, 8, 8])))
    pads = field(1, 6) + field(2, 7) + field(8, 'pads') + field(9, struct.pack('<6q', 0, 0, 0, 0, 2, 0))
    node = field(1, 'input') + field(1, 'pads') + field(2, 'output') + field(4, 'Pad')
    (output / 'channels-6-to-8.onnx').write_bytes(model(node, value_info('input', ['N', 6, 1280]), value_info('output', ['N', 8, 1280]), pads))
    for broken in (False, True):
        pixels = bytearray()
        for y in range(560):
            for x in range(640):
                dx, dy = x - 150, y - 500
                along = dx * .5 - dy * math.sqrt(3) / 2
                across = dx * math.sqrt(3) / 2 + dy * .5
                v = 190 + .06 * (x - 320) - .04 * (y - 280) if across < 45 else 45 + .02 * (x - 320)
                if 0 <= along <= 470 and abs(across) <= 10 and not (broken and 234 <= along <= 274):
                    v = 70
                if dx * dx + dy * dy <= 35 * 35:
                    v = 30
                pixels.append(round(v))
        (output / ('broken.raw' if broken else 'bead.raw')).write_bytes(pixels)
    board = bytearray()
    for y in range(480):
        for x in range(640):
            bx, by = x - 120, y - 80
            v = 120
            if -40 <= bx <= 440 and -40 <= by <= 320:
                v = 220
                if 0 <= bx < 400 and 0 <= by < 280:
                    v = 30 if (bx // 40 + by // 40) % 2 == 0 else 220
            board.append(v)
    (output / 'board.raw').write_bytes(board)
    (output / 'provenance.json').write_text(json.dumps({'origin': 'repository-generated synthetic fixtures', 'customerData': False, 'nativeTestModel': 'channels-6-to-8.onnx', 'runtimeTestModel': 'identity.onnx'}, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', type=Path, required=True)
    generate(parser.parse_args().output_dir)
