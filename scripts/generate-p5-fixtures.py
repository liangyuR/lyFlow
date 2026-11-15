import argparse
import hashlib
import json
import re
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT = 64, 48
FX, FY, CX, CY = 120.0, 120.0, 31.5, 23.5


def png(path, width, height, channels, pixels):
    color = {1: 0, 3: 2, 4: 6}[channels]
    def chunk(name, data):
        return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name + data) & 0xffffffff)
    rows = b''.join(b'\0' + bytes(pixels[y * width * channels:(y + 1) * width * channels]) for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, color, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


def write_json(path, data):
    text = json.dumps(data, ensure_ascii=False, indent=2)
    def compact(match):
        try:
            value = json.loads(match.group(0))
        except ValueError:
            return match.group(0)
        if not isinstance(value, list) or any(isinstance(item, (dict, list)) for item in value):
            return match.group(0)
        rendered = json.dumps(value, ensure_ascii=False)
        prefix = match.start() - text.rfind('\n', 0, match.start()) - 1
        return rendered if prefix + len(rendered) <= 100 else match.group(0)
    text = re.sub(r'\[\n(?:[^\[\]\n]*\n)+?[ \t]*\]', compact, text)
    text = re.sub(r'(?<=\d)e([+-])0+(\d+)', r'e\1\2', text)
    path.write_text(text + '\n', encoding='utf-8', newline='\n')


def color(x, y):
    return ((x * 7) % 256, (y * 9) % 256, ((x + y) * 3) % 256)


def sample(x, y, z, orthographic=False):
    r, g, b = color(x, y)
    xyz = ((x - CX) * .01, (y - CY) * .01, z) if orthographic else ((x - CX) * z / FX, (y - CY) * z / FY, z)
    return (*xyz, x + y / 100.0, 0., 0., -1., (r << 16) | (g << 8) | b)


def pcd(path, points):
    header = ['# .PCD v0.7', 'VERSION 0.7', 'FIELDS x y z intensity normal_x normal_y normal_z rgb', 'SIZE 4 4 4 4 4 4 4 4', 'TYPE F F F F F F F U', 'COUNT 1 1 1 1 1 1 1 1', f'WIDTH {len(points)}', 'HEIGHT 1', 'VIEWPOINT 0 0 0 1 0 0 0', f'POINTS {len(points)}', 'DATA ascii']
    lines = [' '.join(format(struct.unpack('<f', struct.pack('<f', value))[0], '.9g') for value in point[:-1]) + ' ' + str(point[-1]) for point in points]
    path.write_text('\n'.join(header + lines) + '\n', encoding='ascii', newline='\n')


def camera(width, height, fx, fy, cx, cy):
    return {'type': 'vision.CameraCalib.v1', 'data': {'imageSize': [width, height], 'cameraMatrix': [fx, 0, cx, 0, fy, cy, 0, 0, 1], 'distortion': [], 'frame': 'camera', 'unit': 'm'}}


def circle(path, center, normal, radius):
    width, height, focal = 640, 480, 500.
    pixels = bytearray([255]) * (width * height)
    offset = sum(normal[i] * center[i] for i in range(3))
    for y in range(height):
        for x in range(width):
            ray = [(x - 320) / focal, (y - 240) / focal, 1.]
            denominator = sum(normal[i] * ray[i] for i in range(3))
            if abs(denominator) < 1e-12:
                continue
            distance = offset / denominator
            if distance <= 0:
                continue
            delta = [distance * ray[i] - center[i] for i in range(3)]
            if sum(value * value for value in delta) <= radius * radius:
                pixels[y * width + x] = 0
    png(path, width, height, 1, pixels)
    return {'centerM': center, 'unorientedNormal': normal, 'radiusM': radius, 'centerToleranceM': .004, 'normalCosineMinimum': .98, 'surface': 'filled circular aperture bounded by the known radius'}


def generate(output):
    output.mkdir(parents=True, exist_ok=True)
    scene, plane, ortho = [], [], []
    holes = [(x, y) for y in range(22, 25) for x in range(30, 33)]
    up = [(x, y) for y in range(30, 36) for x in range(44, 51)]
    down = [(x, y) for y in range(10, 16) for x in range(10, 16)]
    for y in range(HEIGHT):
        for x in range(WIDTH):
            plane.append(sample(x, y, 1.))
            if (x, y) in holes:
                continue
            offset = .02 if (x, y) in up else (-.015 if (x, y) in down else 0.)
            scene.append(sample(x, y, 1. + offset))
            ortho.append(sample(x, y, offset, True))
    selected_count = len(scene)
    scene.extend([sample(20, 20, 1.25), (2., 0., 1., 99., 0., 0., -1., 0xff00ff), (0., 0., -1., 98., 0., 0., -1., 0x00ffff)])
    pcd(output / 'p5-scene.pcd', scene)
    pcd(output / 'p5-plane.pcd', plane)
    pcd(output / 'p5-orthographic.pcd', ortho)
    write_json(output / 'p5-camera.json', camera(WIDTH, HEIGHT, FX, FY, CX, CY))
    write_json(output / 'p5-circle-camera.json', camera(640, 480, 500., 500., 320., 240.))
    pixels = bytearray()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            pixels.extend((*color(x, y), 50 + (x * 3 + y * 5) % 150))
    image = '1700000001000-p5-scene.png'
    png(output / image, WIDTH, HEIGHT, 4, pixels)
    items = []
    for identifier, label, category, box in [('patch-up', '凸起区域', 1, [44, 30, 7, 6]), ('patch-down', '凹陷区域', 2, [10, 10, 6, 6]), ('hole-region', '缺测区域', 3, [29, 21, 5, 5])]:
        x, y, w, h = box
        polygon = [[x, y], [x + w - 1, y], [x + w - 1, y + h - 1], [x, y + h - 1]]
        items.append({'id': identifier, 'label': label, 'classId': category, 'bbox': box, 'confidence': 1., 'pickable': False, 'occluded': False, 'parts': [{'polygon': polygon, 'holes': []}]})
    write_json(output / 'p5-annotations.json', {'schemaVersion': 1, 'width': WIDTH, 'height': HEIGHT, 'frame': 'image', 'instances': items})
    write_json(output / 'p5-annotations-index.json', {'schemaVersion': 1, 'samples': [{'timestamp': '1700000001000', 'image': image, 'annotation': 'p5-annotations.json'}]})
    circles = {'front': circle(output / 'p5-circle-front.png', [.01, -.008, .4], [0., 0., -1.], .05), 'tilted': circle(output / 'p5-circle-tilted.png', [0., 0., .4], [0., .6, -.8], .05)}
    names = ['p5-scene.pcd', 'p5-plane.pcd', 'p5-orthographic.pcd', 'p5-camera.json', 'p5-circle-camera.json', image, 'p5-annotations.json', 'p5-annotations-index.json', 'p5-circle-front.png', 'p5-circle-tilted.png']
    truth = {'origin': 'repository-generated mathematical fixtures; no customer data', 'generator': 'scripts/generate-p5-fixtures.py', 'units': 'm', 'imageFrame': 'image', 'cloudFrame': 'camera', 'size': [WIDTH, HEIGHT], 'channels': ['intensity', 'normals', 'rgb'], 'scene': {'sourcePoints': len(scene), 'visiblePoints': selected_count, 'holes': len(holes), 'occludedPixel': [20, 20], 'occludedPointIndex': selected_count, 'outsidePointIndex': selected_count + 1, 'behindPointIndex': selected_count + 2, 'upPixels': len(up), 'downPixels': len(down), 'plane': [0., 0., 1., -1.], 'areaModel': 'observed depth pixel footprint orthogonally projected onto reference plane', 'referenceCellAreaM2': 1. / (FX * FY), 'upCellAreaM2': 1.02 ** 2 / (FX * FY), 'downCellAreaM2': .985 ** 2 / (FX * FY), 'upHeightM': .02, 'downHeightM': -.015, 'upAreaM2': len(up) * 1.02 ** 2 / (FX * FY), 'downAreaM2': len(down) * .985 ** 2 / (FX * FY), 'upVolumeM3': len(up) * .02 * 1.02 ** 2 / (FX * FY), 'downVolumeM3': len(down) * .015 * .985 ** 2 / (FX * FY), 'heightToleranceM': 5e-7}, 'orthographic': {'frame': 'workspace', 'resolutionM': [.01, .01], 'pixelZeroCenterM': [-CX * .01, -CY * .01], 'sourcePoints': len(ortho), 'validZeroHeightPoints': selected_count - len(up) - len(down)}, 'circles': circles, 'files': {name: {'bytes': (output / name).stat().st_size, 'sha256': hashlib.sha256((output / name).read_bytes()).hexdigest()} for name in names}}
    assert truth['scene']['visiblePoints'] == 3063 and len(scene) == 3066
    write_json(output / 'p5-fixtures.json', truth)
    print(json.dumps({'fixtures': len(names), 'sourcePoints': len(scene), 'visiblePoints': selected_count, 'holes': len(holes), 'upPixels': len(up), 'downPixels': len(down)}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'examples')
    generate(parser.parse_args().output_dir)
