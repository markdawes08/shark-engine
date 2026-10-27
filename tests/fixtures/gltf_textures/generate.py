"""Reproduce original Shark 12c image fixtures offline with Python's standard library."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parent
OUTPUTS = []


def save(name, data):
    if isinstance(data, str):
        data = data.encode('utf-8')
    (ROOT / name).write_bytes(data)
    OUTPUTS.append({'path': name, 'bytes': len(data),
                    'sha256': hashlib.sha256(data).hexdigest()})


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


def png(width, height, depth, color, raw, extra=b'', interlace=0):
    header = struct.pack('>2I5B', width, height, depth, color, 0, 0, interlace)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + extra
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


gamma = png(2, 2, 8, 6, bytes([0, 0, 0, 0, 0, 255, 255, 255, 255,
                              0, 0, 0, 0, 64, 255, 255, 255, 128]))
save('gamma-alpha.png', gamma)
save('data-npot.png', png(3, 3, 8, 6,
                         b''.join(bytes([0] + [value for x in range(3)
                                               for value in (x * 60 + y * 30,
                                                             10 + x * 30, y * 90, 255)])
                                  for y in range(3))))
save('wide-npot.png', png(5, 1, 8, 6,
                         bytes([0] + [value for x in (0, 0, 0, 0, 250)
                                      for value in (x, 0, 0, 255)])))
save('normal-pair.png', png(2, 1, 8, 6,
                           bytes([0, 255, 128, 128, 32, 128, 255, 128, 224])))
save('normal-cancel.png', png(2, 1, 8, 6,
                             bytes([0, 255, 255, 255, 64, 0, 0, 0, 192])))
values16 = (0x0102, 0x1234, 0xabcd, 0x4321, 0xfedc, 0x8765, 0x0fed, 0xffff)
save('precision16.png', png(2, 1, 16, 6, b'\0' + struct.pack('>8H', *values16)))
save('gray1.png', png(3, 1, 1, 0, b'\0\xa0'))
save('gray2.png', png(4, 1, 2, 0, b'\0\x1b'))
save('gray4.png', png(3, 1, 4, 0, b'\0\x08\xf0'))
save('gray8.png', png(2, 1, 8, 0, bytes([0, 19, 231])))
save('rgb8.png', png(2, 1, 8, 2, bytes([0, 1, 2, 3, 240, 220, 200])))
save('gray-alpha8.png', png(2, 1, 8, 4, bytes([0, 19, 37, 231, 99])))
save('palette2.png', png(3, 1, 2, 3, b'\0\x18',
                          chunk(b'PLTE', bytes([5, 15, 25, 35, 45, 55, 65, 75, 85]))
                          + chunk(b'tRNS', bytes([0, 120, 255]))))
save('rgb-transparent.png', png(2, 1, 8, 2, bytes([0, 1, 2, 3, 9, 8, 7]),
                                chunk(b'tRNS', struct.pack('>3H', 1, 2, 3))))
save('gray-transparent.png', png(2, 1, 8, 0, bytes([0, 19, 231]),
                                 chunk(b'tRNS', struct.pack('>H', 19))))
adam_pixels = [(x * 30, y * 50, x * 10 + y, 10 + x + y * 7)
               for y in range(3) for x in range(5)]
adam_raw = bytearray()
for x0, y0, dx, dy in ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8),
                       (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)):
    for y in range(y0, 3, dy):
        xs = range(x0, 5, dx)
        if len(xs):
            adam_raw.append(0)
            for x in xs:
                adam_raw.extend(adam_pixels[y * 5 + x])
save('adam7.png', png(5, 3, 8, 6, bytes(adam_raw), interlace=1))


def marker(kind, data):
    return bytes([255, kind]) + struct.pack('>H', len(data) + 2) + data


def jpeg(progressive=False):
    # Original single grayscale 8x8 block: unit quantization, DC=0 and AC EOB.
    # Each Huffman table contains only one length-one code (0 -> category/EOB 0).
    result = b'\xff\xd8' + marker(0xdb, bytes([0] + [1] * 64))
    result += marker(0xc2 if progressive else 0xc0,
                     bytes([8, 0, 8, 0, 8, 1, 1, 0x11, 0]))
    table = bytes([1] + [0] * 15 + [0])
    result += marker(0xc4, b'\0' + table + b'\x10' + table)
    if progressive:
        result += marker(0xda, bytes([1, 1, 0, 0, 0, 0])) + b'\x7f'
        result += marker(0xda, bytes([1, 1, 0, 1, 63, 0])) + b'\x7f'
    else:
        result += marker(0xda, bytes([1, 1, 0, 0, 63, 0])) + b'\x3f'
    return result + b'\xff\xd9'


save('baseline.jpg', jpeg())
save('progressive.jpg', jpeg(True))
binary = struct.pack('<15f', 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1)
document = {
    'asset': {'version': '2.0', 'generator': 'Shark original offline texture fixture'},
    'buffers': [{'uri': 'triangle.bin', 'byteLength': len(binary)}],
    'bufferViews': [{'buffer': 0, 'byteOffset': 0, 'byteLength': 36},
                    {'buffer': 0, 'byteOffset': 36, 'byteLength': 24}],
    'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3'},
                  {'bufferView': 1, 'componentType': 5126, 'count': 3, 'type': 'VEC2'}],
    'images': [{'uri': 'gamma-alpha.png'}], 'textures': [{'source': 0}],
    'materials': [{'pbrMetallicRoughness': {'baseColorTexture': {'index': 0},
                                          'metallicRoughnessTexture': {'index': 0}},
                   'normalTexture': {'index': 0}, 'occlusionTexture': {'index': 0},
                   'emissiveTexture': {'index': 0}}],
    'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'TEXCOORD_0': 1}, 'material': 0}]}],
    'nodes': [{'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0,
}
save('triangle.bin', binary)
save('external.gltf', json.dumps(document, indent=2) + '\n')
embedded = copy.deepcopy(document)
embedded['buffers'][0]['uri'] = ('data:application/octet-stream;base64,'
                                  + base64.b64encode(binary).decode('ascii'))
embedded['images'][0]['uri'] = 'data:image/png;base64,' + base64.b64encode(gamma).decode('ascii')
save('embedded.gltf', json.dumps(embedded, indent=2) + '\n')
glb_document = copy.deepcopy(document)
image_offset = len(binary)
glb_binary = binary + gamma
glb_document['buffers'] = [{'byteLength': len(glb_binary)}]
glb_document['bufferViews'].append({'buffer': 0, 'byteOffset': image_offset, 'byteLength': len(gamma)})
glb_document['images'] = [{'bufferView': 2, 'mimeType': 'image/png'}]
encoded_json = json.dumps(glb_document, separators=(',', ':')).encode('utf-8')
encoded_json += b' ' * ((-len(encoded_json)) % 4)
glb_binary += b'\0' * ((-len(glb_binary)) % 4)
glb = struct.pack('<5I', 0x46546c67, 2, 28 + len(encoded_json) + len(glb_binary),
                  len(encoded_json), 0x4e4f534a)
glb += encoded_json + struct.pack('<2I', len(glb_binary), 0x004e4942) + glb_binary
save('embedded.glb', glb)
manifest = {
    'origin': 'Original Shark engine-authored offline data, 2026-10-27',
    'license': 'MIT; see LICENSE', 'generator': 'generate.py; Python standard library only',
    'inputs': OUTPUTS,
    'expected': {
        'gamma_alpha_base_rgba': [0, 0, 0, 0, 255, 255, 255, 255,
                                 0, 0, 0, 64, 255, 255, 255, 128],
        'gamma_alpha_color_mip': [188, 188, 188, 112],
        'gamma_alpha_data_mip': [128, 128, 128, 112],
        'data_npot_mip': [90, 40, 90, 255],
        'wide_npot_mips_red': [[0, 0, 0, 0, 250], [0, 100], [50]],
        'precision16_base_components': list(values16),
        'jpeg_dimensions': [8, 8], 'jpeg_rgba_every_pixel': [128, 128, 128, 255],
        'adam7_rgba': adam_pixels,
    },
}
(ROOT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8', newline='\n')
