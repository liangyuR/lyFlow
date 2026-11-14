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


def value_info(name, dims, element_type=1):
    shape = b''.join(field(1, field(2 if isinstance(d, str) else 1, d)) for d in dims)
    return field(1, name) + field(2, field(1, field(1, element_type) + field(2, shape)))


def model(node, inputs, outputs, initializer=b''):
    nodes = node if isinstance(node, list) else [node]
    input_values = inputs if isinstance(inputs, list) else [inputs]
    output_values = outputs if isinstance(outputs, list) else [outputs]
    graph = b''.join(field(1, value) for value in nodes) + field(2, 'lyflow_synthetic_release')
    if initializer:
        initializers = initializer if isinstance(initializer, list) else [initializer]
        graph += b''.join(field(5, value) for value in initializers)
    graph += b''.join(field(11, value) for value in input_values)
    graph += b''.join(field(12, value) for value in output_values)
    return field(1, 8) + field(2, 'lyflow-release-fixture') + field(7, graph) + field(8, field(2, 13))


def generate(output):
    output.mkdir(parents=True, exist_ok=True)
    pads = field(1, 6) + field(2, 7) + field(8, 'pads') + field(9, struct.pack('<6q', 0, 0, 0, 0, 2, 0))
    node = field(1, 'input') + field(1, 'pads') + field(2, 'output') + field(4, 'Pad')
    (output / 'channels-6-to-8.onnx').write_bytes(model(node, value_info('input', ['N', 6, 1280]), value_info('output', ['N', 8, 1280]), pads))
    (output / 'channels-6-to-8-static1.onnx').write_bytes(model(node, value_info('input', [1, 6, 1280]), value_info('output', [1, 8, 1280]), pads))
    first = field(1, 'input') + field(2, 'first') + field(4, 'Identity')
    second = field(1, 'input') + field(2, 'second') + field(4, 'Identity')
    (output / 'multi-output.onnx').write_bytes(model([first, second], value_info('input', [1, 6, 1280]), [value_info('first', [1, 6, 1280]), value_info('second', [1, 6, 1280])]))
    def tensor(name, dims, dtype, raw):
        return b''.join(field(1, d) for d in dims) + field(2, dtype) + field(8, name) + field(9, raw)
    def integer_attribute(name, value):
        return field(1, name) + field(3, value) + field(20, 2)
    def onnx_node(kind, inputs, outputs, attribute=b''):
        return b''.join(field(1, item) for item in inputs) + b''.join(field(2, item) for item in outputs) + field(4, kind) + (field(5, attribute) if attribute else b'')
    initializers = [tensor('index', [], 7, struct.pack('<q', 0)), tensor('bias', [1], 1, struct.pack('<f', 3)), tensor('one', [1], 7, struct.pack('<q', 1))]
    dynamic_head = [onnx_node('Flatten', ['input'], ['flat'], integer_attribute('axis', 1)),
                    onnx_node('Gather', ['flat', 'index'], ['pixel'], integer_attribute('axis', 1)),
                    onnx_node('Add', ['pixel', 'bias'], ['sizefloat']),
                    onnx_node('Cast', ['sizefloat'], ['sizeint'], integer_attribute('to', 7)),
                    onnx_node('Concat', ['one', 'sizeint'], ['size'], integer_attribute('axis', 0)),
                    onnx_node('ConstantOfShape', ['size'], ['output'], field(1, 'value') + field(5, tensor('', [1], 1, struct.pack('<f', .5))) + field(20, 4))]
    (output / 'lying-direction.onnx').write_bytes(model(dynamic_head, value_info('input', ['N', 3, 48, 192]), value_info('output', ['N', 2]), initializers))
    cast_to = field(1, 'to') + field(3, 7) + field(20, 2)
    cast = field(1, 'input') + field(2, 'output') + field(4, 'Cast') + field(5, cast_to)
    (output / 'cast-to-int64.onnx').write_bytes(model(cast, value_info('input', ['N', 6, 1280]), value_info('output', ['N', 6, 1280], 7)))
    (output / 'provenance.json').write_text(json.dumps({'origin': 'repository-generated synthetic fixtures', 'customerData': False, 'nativeTestModel': 'channels-6-to-8.onnx', 'staticBatchModel': 'channels-6-to-8-static1.onnx', 'negativeTestModels': ['cast-to-int64.onnx', 'multi-output.onnx', 'lying-direction.onnx']}, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', type=Path, required=True)
    generate(parser.parse_args().output_dir)
