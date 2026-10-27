"""Regenerate Shark-authored, MIT-licensed offline glTF source fixtures.

This does not download data or run during ordinary tests. Binary values and the
one-pixel PNG are literal fixture inputs, not importer-generated output.
"""

import base64
import hashlib
import json
from pathlib import Path
import struct
import zlib


ROOT = Path(__file__).resolve().parent
triangle = struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0)
sparse = bytes((1, 0, 0, 0)) + struct.pack("<3f", 1, 2, 3)


def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


# One authored grayscale/alpha pixel: filter0, gray127, alpha255. Explicit PNG
# headers and CRCs make the source independently reproducible without a codec.
png = (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 4, 0, 0, 0)) +
       png_chunk(b"IDAT", zlib.compress(bytes((0, 127, 255)), 9)) + png_chunk(b"IEND", b""))


def document(buffer):
    return {
        "asset": {"version": "2.0", "generator": "Shark authored offline fixture"},
        "buffers": [buffer],
        "bufferViews": [{"buffer": 0, "byteLength": 36}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3,
                       "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
        "nodes": [{"mesh": 0}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
    }


def text(value):
    return json.dumps(value, separators=(",", ":")) + "\n"


external = document({"uri": "triangle.bin", "byteLength": 36})
embedded = document({"uri": "data:application/octet-stream;base64," +
                     base64.b64encode(triangle).decode("ascii"), "byteLength": 36})
sparse_document = document({"uri": "sparse.bin", "byteLength": 16})
sparse_document["bufferViews"] = [
    {"buffer": 0, "byteOffset": 0, "byteLength": 1},
    {"buffer": 0, "byteOffset": 4, "byteLength": 12},
]
sparse_document["accessors"] = [{
    "componentType": 5126, "count": 3, "type": "VEC3",
    "min": [0, 0, 0], "max": [1, 2, 3],
    "sparse": {"count": 1, "indices": {"bufferView": 0, "componentType": 5121},
               "values": {"bufferView": 1}},
}]
repaired_indices = triangle + struct.pack("<3H", 0, 1, 99) + bytes((2, 0)) + struct.pack("<H", 2)
repaired_document = document({"uri": "repaired-indices.bin", "byteLength": 46})
repaired_document["bufferViews"] = [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 6},
    {"buffer": 0, "byteOffset": 42, "byteLength": 1},
    {"buffer": 0, "byteOffset": 44, "byteLength": 2},
]
repaired_document["accessors"].append({
    "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR",
    "sparse": {"count": 1, "indices": {"bufferView": 2, "componentType": 5121},
               "values": {"bufferView": 3}},
})
repaired_document["meshes"][0]["primitives"][0]["indices"] = 1
glb_json = text(document({"byteLength": 36})).rstrip("\n").encode("utf-8")
glb_json += b" " * (-len(glb_json) % 4)
glb = (b"glTF" + struct.pack("<II", 2, 12 + 8 + len(glb_json) + 8 + len(triangle)) +
       struct.pack("<II", len(glb_json), 0x4E4F534A) + glb_json +
       struct.pack("<II", len(triangle), 0x004E4942) + triangle)

files = {
    "minimal.gltf": text({"asset": {"version": "2.0"}}).encode("utf-8"),
    "external.gltf": text(external).encode("utf-8"),
    "embedded.gltf": text(embedded).encode("utf-8"),
    "sparse.gltf": text(sparse_document).encode("utf-8"),
    "repaired-indices.gltf": text(repaired_document).encode("utf-8"),
    "triangle.glb": glb,
    "triangle.bin": triangle,
    "sparse.bin": sparse,
    "repaired-indices.bin": repaired_indices,
    "pixel.png": png,
}
for name, data in files.items():
    (ROOT / name).write_bytes(data)
manifest = {name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            for name, data in sorted(files.items())}
(ROOT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(json.dumps(manifest, indent=2))
