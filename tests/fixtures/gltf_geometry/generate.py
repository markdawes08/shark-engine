"""Reproduce Shark's original, offline 12b geometry fixture; no downloads."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parent
positions = (0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0)
normals = (0, 0, 1) * 4
binary = struct.pack('<24f4H', *(positions + normals + (0, 1, 2, 3)))
document = {
    'asset': {'version': '2.0', 'generator': 'Shark original offline geometry fixture'},
    'buffers': [{'uri': 'shared-strip.bin', 'byteLength': len(binary)}],
    'bufferViews': [{'buffer': 0, 'byteOffset': 0, 'byteLength': 48},
                    {'buffer': 0, 'byteOffset': 48, 'byteLength': 48},
                    {'buffer': 0, 'byteOffset': 96, 'byteLength': 8}],
    'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 4,
                   'type': 'VEC3', 'min': [0, 0, 0], 'max': [1, 1, 0]},
                  {'bufferView': 1, 'componentType': 5126, 'count': 4, 'type': 'VEC3'},
                  {'bufferView': 2, 'componentType': 5123, 'count': 4, 'type': 'SCALAR'}],
    'meshes': [{'name': 'Shared strip', 'primitives': [
        {'attributes': {'POSITION': 0, 'NORMAL': 1}, 'indices': 2, 'mode': 5}]}],
    'nodes': [{'name': 'Mirrored', 'mesh': 0, 'scale': [-2, 3, 1]},
              {'name': 'Offset', 'mesh': 0, 'translation': [4, 0, 0]}],
    'scenes': [{'nodes': [0, 1]}], 'scene': 0,
}
(ROOT / 'shared-strip.bin').write_bytes(binary)
(ROOT / 'shared-strip.gltf').write_text(json.dumps(document, indent=2) + '\n',
                                       encoding='utf-8', newline='\n')
embedded = copy.deepcopy(document)
embedded['buffers'][0]['uri'] = 'data:application/octet-stream;base64,' + base64.b64encode(binary).decode('ascii')
(ROOT / 'embedded-strip.gltf').write_text(json.dumps(embedded, indent=2) + '\n',
                                        encoding='utf-8', newline='\n')
glb_document = copy.deepcopy(document)
del glb_document['buffers'][0]['uri']
glb_json = json.dumps(glb_document, separators=(',', ':')).encode('utf-8')
glb_json += b' ' * ((-len(glb_json)) % 4)
glb = struct.pack('<5I', 0x46546c67, 2, 28 + len(glb_json) + len(binary), len(glb_json), 0x4e4f534a)
glb += glb_json + struct.pack('<2I', len(binary), 0x004e4942) + binary
(ROOT / 'shared-strip.glb').write_bytes(glb)
inputs = []
for name in ('shared-strip.bin', 'shared-strip.gltf', 'embedded-strip.gltf', 'shared-strip.glb'):
    data = (ROOT / name).read_bytes()
    inputs.append({'path': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
(ROOT / 'manifest.json').write_text(json.dumps({
    'origin': 'Original Shark engine-authored offline data, 2026-10-27',
    'license': 'MIT; see LICENSE', 'generator': 'generate.py', 'inputs': inputs,
    'expected': {'meshes': 1, 'primitives': 1, 'nodes': 2, 'vertices': 4,
                 'indices': [0, 1, 2, 2, 1, 3],
                 'local_bounds': [[0, 0, 0], [1, 1, 0]],
                 'world_bounds': [[-2, 0, 0], [5, 3, 0]]}}, indent=2) + '\n',
    encoding='utf-8', newline='\n')
