# Offline glTF source fixtures

Owner: assignment 12a. All files are original Shark test data released under the
adjacent [MIT license](LICENSE); there are no downloaded models or third-party
content licenses. Origin: `tests/fixtures/gltf_source/generate.py`, Shark
contributors, 2026. Normal tests read these local versioned files and use no
network access.

The positions are the literal triangle `(0,0,0), (1,0,0), (0,1,0)`. Its source
buffer has exactly 36 little-endian float bytes. The sparse fixture has three
logical zero-initialized VEC3 elements and replaces index 1 with `(1,2,3)` using
one unsigned-byte index and one aligned float record. The one-pixel PNG stores
gray 127/alpha 255 with explicit PNG headers and CRCs; 12a inspects its reference/signature and does not claim
image decoding. Its color/data usage and decoding belong to later checkpoints.

`minimal.gltf` supplies only the glTF 2.0 asset declaration. `external.gltf`,
`embedded.gltf` and `triangle.glb` describe the same one-node/one-mesh scene using
external, base64 and binary-chunk storage respectively. `sparse.gltf` exercises
sparse source storage. `repaired-indices.gltf` has base indices `(0,1,99)` and a
sparse override of index 2 to value 2. The effective triangle `(0,1,2)` is valid;
checking only the unpatched base buffer would reject it incorrectly. Its C++
negative control changes the actual sparse value to 3 and requires rejection.
Malformed variants are produced in memory by the C++
tests, leaving the reference fixtures unchanged.

[manifest.json](manifest.json) records independent byte counts and SHA-256
digests. To regenerate these authored bytes explicitly, run:

```text
python tests/fixtures/gltf_source/generate.py
```

Regeneration is a maintainer action, never an automatic response to a failure.
