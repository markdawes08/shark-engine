# Original offline geometry fixtures

These glTF and binary inputs were authored for Shark on 2026-10-27 and use the
MIT license in `LICENSE`. No third-party model, texture or downloaded data is
included. `generate.py` reproduces the four saved inputs and their SHA-256 inventory
in `manifest.json`; normal tests read the saved files and never run a downloader.

`shared-strip.gltf` contains one four-vertex indexed triangle strip instanced by
two nodes. The first has scale `[-2,3,1]`; the second translates by `[4,0,0]`.
Independent expected output is one shared mesh, indices `[0,1,2,2,1,3]`, local
bounds `[0,0,0]` through `[1,1,0]`, and world bounds `[-2,0,0]` through `[5,3,0]`.
`embedded-strip.gltf` stores those exact source bytes in a base64 URI;
`shared-strip.glb` stores them in an aligned GLB BIN chunk. Both have the same
independent geometry and transform expectations without external mappings.
Tests also author bounded binary/JSON fixtures in memory for individual malformed,
sparse, normalized, transform and allocation-failure cases. Their source is
`tests/gltf_geometry_tests.cpp`, with the same original engine-authored provenance.
