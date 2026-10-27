# Original offline texture fixtures

These original images and static glTF inputs were authored for Shark on
2026-10-27 under the MIT license in `LICENSE`. They contain no downloaded or
third-party art. `generate.py` reproduces every binary/image/container and the
SHA-256 inventory in `manifest.json` with Python's standard library. Tests only
read checked-in files, never invoke this generator or download assets.

PNG inputs cover RGBA8/16, packed 1/2/4-bit grayscale, RGB, gray plus alpha,
palette plus transparency, transparent RGB/grayscale and Adam7. Every PNG uses
explicit original pixels, filter zero and a standard zlib stream. JPEG inputs
use an original hand-authored 8x8 grayscale block with DC zero, AC end-of-block,
unit quantization and tiny explicit Huffman tables. Baseline and progressive
scans both independently specify constant RGB 128 and opaque alpha.

The 2x2 gamma/alpha image is black/white in each row with alpha 0, 255, 64, 128.
Its one-pixel color mip is RGB 188, whereas the data mip is RGB 128; both have
alpha 112. Straight alpha must not darken RGB. The 3x3 and 5x1 images expose
odd-size mip coverage. The normal fixtures expose renormalization and an exact
cancelling vector pair. PNG16 includes components whose low bytes are nonzero.
All expected pixels are recorded independently in the manifest and C++ tests.

`external.gltf`, `embedded.gltf` and `embedded.glb` share one triangle and the
gamma/alpha image used in all five core roles. External paths carry caller-owned
AssetIds; embedded sources retain source indices without invented persistent IDs.
Further malformed/header/allocation/selection fixtures are authored in memory by
`tests/gltf_texture_tests.cpp` under the same original provenance and license.
