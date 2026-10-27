# 12c: Images and textures

Date: 2026-10-27. **12c is PASS under the current-machine scope.** Full REQ-12
remains NOT_RUN pending 12d-12e. Base: `2ca0f97`, initially clean `main`.
Changes remain unstaged/uncommitted; the user owns staging, commits and pushing.
**Next: 12d, materials and asset integration.**

Follow the [five checkpoints](../roadmap/gltf-plan.md),
[ADR 0030](../architecture/0030-bounded-gltf-textures.md),
[texture contract](../gltf-textures.md), [source boundary](../gltf-source.md),
[geometry contract](../gltf-geometry.md), [decoder review](../dependencies/stb-image.md)
and [receipt](../coverage/evidence/12c-gltf-textures.json).

## Delivered behavior

`convert_gltf_textures` consumes the same immutable glTF/GLB root and explicit
external path/AssetId/byte snapshots as inspection and geometry conversion. The
existing source owner retains complete container, URI, resource, accessor, graph,
signature and MIME validation. Texture conversion collects core material uses
from meshes reachable in the selected scene and validates each selected
primitive's referenced UV0/UV1. Unused images retain the earlier source checks;
they are not implicitly decoded. No scenes gives no texture uses.

Owned output contains the source report, selected scene, semantic image variants
and per-use source/UV/sampler metadata. The private memory-only decoder returns
top-left RGBA8 or explicit little-endian RGBA16 bytes. PNG supports its legal
low-depth, palette, transparency, sixteen-bit and Adam7 layouts. JPEG supports
the pinned eight-bit sequential/progressive Huffman profile. File I/O, HDR,
float conversion and SIMD remain disabled. No decoder pointer or include escapes
the public API, and no file/cache/Scene publication or GPU operation occurs.

Unique source images decode once. Variants share only an identical source image
index and processing semantic: base-color/emissive use sRGB color, metallic-
roughness/occlusion use linear data, and normal maps use signed-vector filtering.
Color RGB filters in linear light and re-encodes; data channels average directly;
normal mips renormalize with a deterministic +Z cancellation fallback. Alpha
filters independently with straight channels. Integer sums preserve exact data/
alpha ties at sixteen bits. The same image in color and data roles retains
separate mip chains; equal content never merges different source identities.

Every variant has a complete mip chain to 1x1. Exact integer area-overlap weights
include odd trailing texels, with floor-halved dimensions and nearest half-up
quantization. Samplers remain per use: preserve both magnification choices, all
six minification/mipmap choices and three wrapping modes on each axis. Defaults
are LINEAR magnification, LINEAR_MIPMAP_LINEAR minification and REPEAT wrapping.

PNG preflight checks full framing/CRCs, exact filtered-row and DEFLATE inventories,
Adler checksum, filters and effective palette indices before ordinary decoding.
JPEG preflight checks marker/segment framing, frame/scan/table parameters,
initial component coverage, exact restart count/order from scan MCU grids and
terminal EOI. Decoder scratch and realloc growth are zero-initialized. These are
the documented structural guards plus the pinned decoder; framing-complete
damaged JPEG entropy can still be tolerated upstream. General JPEG conformance
or authenticity is not claimed.

Before decoder allocation, header inspection precharges unique decoded bytes,
semantic variants, complete mip chains, metadata and work. Independent bounds
remain 4096 per dimension, 4,194,304 pixels per image, 32 MiB decoded bytes,
32 MiB semantic mip bytes, 36 MiB logical owned output, 4096 uses, 381 variants,
64 MiB live/256 MiB cumulative decoder allocations and 33,554,432 work items.
All earlier source/geometry bounds remain unchanged. Nullable tracked allocation,
failed-realloc preservation, RAII cleanup and cooperative cancellation return
owned contextual Errors without publishing partial results. Logical bounds do
not claim whole-process RSS or forced interruption inside a codec call.

The exact private pin is stb_image 2.30 from commit
`003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5`, with the MIT license option,
no patches and no transitive dependencies. Its 1,515,348-byte archive SHA-256 is
`a6c31950b3bdf05eb92258768ba2a11bd44449490574b1aac67028452288c40d`.
The lock records all 431 regular members; only the reviewed header compiles in
one private EngineTools translation unit. Upstream builds never execute.

Original MIT-licensed offline PNG/JPEG/glTF/GLB fixtures retain independent pixels,
mip expectations, generator and hash inventory. Forty-three texture cases have
at least 9,022 assertions, including bounded allocation/cancellation and JPEG
scratch/restart correctness regressions. All 41 source cases/1,794 assertions,
45 geometry cases/988 assertions and every earlier workload remain. The twelfth
native FeatureWorld case checks the same operation and independent mip values
without changing the AuthoredScene owner or four saved IDs. Independent coverage
now has 43 CPU surfaces and one external GPU command, with 30 process checks.

## Qualification

| Selection | Passing CTest outcomes | Extent |
| --- | ---: | --- |
| Windows MSVC Debug | 119 | Full inventory |
| Windows MSVC Release | 28 | Tools selection |
| Windows MSVC ASan | 28 | Tools selection |
| WSL Ubuntu GCC Debug | 112 | Full inventory |
| WSL Ubuntu Clang ASan/UBSan | 28 | Tools selection |
| Standalone Windows Tools | 28 | Tools selection |
| Standalone WSL Tools | 28 | Tools selection |
| WSL Server Debug | 68 | Fresh full inventory |
| WSL Server ASan/UBSan | 68 | Fresh full inventory |

All 507 fresh outcomes pass: 231 from complete client Debug inventories, 140
from five complete declared Tools selections and 136 from fresh full Server
inventories. The 56 standalone Tools outcomes are a subset of 140. Release and
client sanitizer evidence covers the declared Tools selections, not their whole
client inventories. Disjoint functional/I/O partitions preserve actual per-test
JUnit hashes, timing and provenance; they are not uninterrupted full invocations.
Historical 12a/12b inventories are controls, never fresh execution counts.

The receipt pins 564 source inputs, implemented manifest, configuration,
historical inventories and ten execution helpers. The canonical profiles remain
`build/windows-debug`, `build/windows-release`, `build/windows-asan` and
`build/tools-debug`, plus retained WSL `/work/build-05a-final-client/linux-debug`,
`/work/build-05a-final-client/linux-asan-ubsan`, `/work/build-09a-tools`,
`/work/build-05a-final/server-debug` and `/work/build-05a-final/server-asan-ubsan`.
No two builds run against the same directory concurrently. A shared qualification
I/O gate serializes heavy metadata work without changing any child test limit.

The new `shark.tools.gltf-textures` family retains 90 seconds and Catch seed 1337;
`shark.tooling.image-dependencies` retains 60 seconds. Source/dependency and
geometry families retain 60 seconds and their original seeds. Every earlier
family, label, timeout, WILL_FAIL property, seed and workload remains required.
All sixteen public CPU examples compile under MSVC/GCC/Clang (48 compiles), with
header-dependency scans excluding parser/decoder/private inputs. Actual ASan
symbols qualify eighteen native objects and ASan/UBSan eighteen WSL objects,
including the private decoder and texture tests. Only the parser translation
unit retains private MSVC `/EHc-` after `/EHsc`; the decoder does not acquire it.

All seven Tools-bearing profiles retain 59 synchronous/113 asynchronous
interruption checks, 39 async children and seven abrupt-exit phases. Within
their respective fixtures, 35 synchronous and 49 asynchronous snapshots reproduce
bytes/root keys; async snapshots also reproduce dependency keys. Scene
qualification retains 19 connected checks and all 15 rejection controls.

Core/Client/Server/Runtime implementation and protected boundary policies remain
unchanged. The new global dependency-lock addition and dependency-fixture changes
require the two fresh full Server suites above. Protected configurations/builds/
closures/packages are refreshed with Tools OFF and no decoder/parser population.
The 35-file native Runtime package passes Unicode relocation under an actual
standard-user token and system-only PATH. The 12-file Server package and CPU
Runtime launch in the retained SDK-free, network-isolated WSL userspace. Server
deployment bytes and original paths are hash-verified in an archive before only
the exact expanded copy is removed; successful temporary native copies are clean.

The first graphics-tool preparation failed because environment PATH selection
chose the wrong CMake, before qualification of the tools. The separate pinned
offline preparation passes using the existing canonical cached validation-tool
build, and the current-lock shader bundle passes. Original failure evidence is
preserved. These CPU tool/artifact checks claim no new GPU execution. Canonical
builds/dependency caches and retained WSL userspaces are reused; no host, WSL
resource or kernel setting changes.

Before the frozen matrix, the missing offline image archive on the first
configure was supplied through the explicit locked archive input. Initial MSVC
C4334/C4389 and unreachable-code diagnostics were corrected without relaxing
warning policy. JPEG scratch initialization and exact restart-inventory regressions
were resolved as texture correctness fixes, then all 43 cases/9,022 assertions
passed MSVC Release/ASan and GCC/Clang preflights. Original failures remain
preserved; development/preflight attempts are separate from the 507 frozen
outcomes. The receipt is created only after every declared frozen result, public
example, actual sanitizer object and protected package gate passes.

## Commands and next checkpoint

Exact helper/config hashes, commands, inventories and raw logs are in the receipt:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/12c-work/native.py
wsl.exe -d Ubuntu -u root --cd / -- bash /mnt/c/Users/mrkda/Projects/shark/out/12c-work/wsl.sh
```

Focused versioned checks list the configured inventory first and retain original
per-test limits:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset tools-debug --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset tools-debug --show-only
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset tools-debug -R ^shark.tools.gltf-textures$ --output-on-failure
```

12d assembles core materials and typed external-resource identity integration,
then connects the existing import/cache/job owner with original revision and
publication fences. It also qualifies usable optional-extension core fallback.
Full REQ-12 closes only in 12e. Terminal/editor adapters remain 15/16, final PBR
rendering acceptance 26, cooked output 31 and animation M06. Texture output is
an owned CPU intermediate value, not a serialized payload or scene renderer.

Suggested commit: `feat(tools): decode glTF images and generate texture mips`.
