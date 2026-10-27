# 12b: Static glTF geometry and hierarchy

Date: 2026-10-27. **12b is PASS under the current-machine scope.** Full REQ-12
remains NOT_RUN pending 12c-12e. Base: `8ed892b`, initially clean `main`.
Changes remain unstaged/uncommitted; the user owns staging, commits and pushing.
**Next: 12c, images and textures.**

Follow the [five checkpoints](../roadmap/gltf-plan.md),
[ADR 0029](../architecture/0029-bounded-gltf-geometry.md),
[geometry contract](../gltf-geometry.md), [source boundary](../gltf-source.md)
and [receipt](../coverage/evidence/12b-gltf-geometry.json).

## Delivered behavior

`convert_gltf_geometry` consumes the same immutable `.gltf`/`.glb` input and
explicit external path/AssetId/byte snapshots as inspection. The existing owner
runs every 12a source/container/resource/accessor/parser check before conversion.
No second parser or filesystem callback is introduced, and cgltf data readers
are never used. All effective component reads are bytewise, including sparse
overrides, implicit zero bases, normalized integer values and repaired invalid
base bytes. New public headers expose engine-owned values and Core math only.

The owned result contains triangle vertices/indices, shared meshes, selected
hierarchy, source correspondence, canonical local TRS, full world matrices,
inverse-transpose normal matrices, per-instance reflection flags and bounds.
An explicit scene index overrides the source default, then scene zero is the
fallback. No scenes gives an empty result; loose content is not implicitly
imported. Only selected reachable content undergoes geometry conversion, while
complete source storage/reference/hierarchy validation is retained.

Nodes preserve depth-first root/child source order with parents before children.
Reachable meshes are converted once in source-index order, and node instances
refer to the same mesh. Dense output references remain distinct from explicit
source indices and persistent AssetIds. Triangle strips/fans expand deterministically;
zero-area faces are diagnosed/discarded without changing strip parity. Invalid
counts, reserved maximum indices, normalized indices, incorrect buffer targets,
nonfinite vertex values and unsupported semantics reject with source context.

Positions/normals/tangents use core float storage; UV0/UV1 and COLOR_0 also decode
normalized unsigned bytes/shorts. Core texture links require available supported
UV sets. Missing normals generate flat face normals and split corners. Supplied
tangents without supplied normals are ignored. Missing normal-map tangents use
the documented per-triangle UV-derivative/Gram-Schmidt basis and texture UV set,
splitting corners and retaining mirrored UV sign. Degenerate UVs use a finite
deterministic fallback with a diagnostic. MikkTSpace equivalence is not claimed;
assets may supply their baker's explicit tangent basis. Final PBR acceptance is 26.

Local transforms reuse Core's existing validators and decomposition. Negative
and nonuniform scales are retained, local shear/singularity reject, and valid
hierarchical world shear remains a full matrix. Mirror state belongs to each
node; shared indices and tangent W never change for a different instance.
Primitive/mesh bounds are tight over drawn vertices. Each node's own mesh has
conservative world bounds from eight local-bound corners; selected-scene bounds
union the instances, excluding invented bounds on meshless nodes.

Fixed independent conversion limits are 262,144 output vertices, 786,432 expanded
indices, 32 MiB logical owned output and 8,388,608 scalar/topology work items.
Conservative counts are charged before their allocations/loops, including repeated
primitive references and metadata. Mesh instances share converted data. Graph walks
are iterative and cancellation cooperative. Every inherited source limit and
allocator sweep remains unchanged; logical output limits do not claim total RSS.

Four original MIT-licensed saved geometry inputs cover external JSON/binary,
embedded JSON and GLB, with generator, provenance and hash inventory. Forty-five
independent geometry cases have at least 988 assertions. The eleventh native
FeatureWorld case checks actual dense/sparse geometry, mirrored shared instances,
transforms, bounds, identities, repeats and rejections. It preserves the same
AuthoredScene owner and four saved IDs. Independent inventories now contain 42 CPU
surfaces and one external GPU command; FeatureWorld has 29 process checks.

## Qualification

| Selection | Passing CTest outcomes | Extent |
| --- | ---: | --- |
| Windows MSVC Debug | 117 | Full inventory |
| Windows MSVC Release | 26 | Tools selection |
| Windows MSVC ASan | 26 | Tools selection |
| WSL Ubuntu GCC Debug | 110 | Full inventory |
| WSL Ubuntu Clang ASan/UBSan | 26 | Tools selection |
| Standalone Windows Tools | 26 | Tools selection |
| Standalone WSL Tools | 26 | Tools selection |

All 357 fresh outcomes pass: 227 from complete client Debug inventories and
130 from complete declared Tools selections. The 52 standalone Tools outcomes
are a subset of the 130. Complete/selected runs use disjoint functional/I/O
partitions with original per-test JUnit hashes, timing and provenance; these
are not described as uninterrupted full runs. Every earlier family, label,
timeout, WILL_FAIL property and RNG seed is preserved.

The receipt pins 527 source inputs, implemented manifest, configuration,
historical inventories and ten execution helpers. All fifteen public CPU examples
compile under MSVC/GCC/Clang (45 compiles), with compiler header-dependency scans.
Actual ASan symbols are verified in sixteen native objects; ASan/UBSan symbols in
sixteen WSL objects, including the new geometry tests and existing parser source.
The private parser-only MSVC `/EHc-` ordering remains checked across native builds.

All seven Tools-bearing profiles preserve 41 source cases/1,794 assertions,
all earlier authoring/persistence/asset workloads and the old 59 synchronous/
113 asynchronous interruption checks. Within their respective fixtures,
35 synchronous and 49 asynchronous snapshots reproduce bytes and root keys;
async snapshots also reproduce dependency keys. The independent qualification
retains 19 connected checks and all 15 rejection controls.

Core/Client/Server/Runtime implementation, dependency lock and protected boundary
policies are unchanged. The receipt explicitly revalidates and inherits 12a's
136 full Server executions; none is counted as a fresh 12b execution. Current
protected configurations/builds/closures/packages are refreshed. The 35-file
native Runtime package passes Unicode relocation under an actual standard-user
token with system-only PATH. The 12-file Server package and CPU Runtime launch
in the retained SDK-free, network-isolated WSL userspace. The existing current-lock
shader bundle verifies and is reused; no graphics tooling rebuild is needed.
Successful temporary deployment copies are cleaned, with Server bytes and original
paths verified in an archive before exact expanded-copy removal. Canonical
builds/dependency caches and retained WSL userspaces are reused. No host/resource/
kernel settings change and no new GPU qualification is claimed for CPU conversion.

Before the frozen run, compiler preflights passed on MSVC Release/ASan and GCC/
Clang ASan/UBSan. Initial MSVC local-shadow warnings and a GCC test-loop copy
warning were corrected without relaxing warning policy. The first focused
scene-qualification check rejected a newly mapped contract path before that
document existed; after the contract was added all six focused families passed.
The original focused raw log/XML and failed WSL compile log remain under
`out/12b-work/preflight-initial` and `preflight-wsl`; recovered preflight evidence
is separate. Development attempts are not counted among the 357 frozen results.
Fresh independent code and evidence-helper reviews found no remaining issues.

## Commands and next checkpoint

The receipt records exact helper/config hashes, commands, inventories and raw logs:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/12b-work/native.py
wsl.exe -d Ubuntu -u root --cd / -- bash /mnt/c/Users/mrkda/Projects/shark/out/12b-work/wsl.sh
```

For focused versioned checks, list the configured inventory first:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset tools-debug --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset tools-debug --show-only
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset tools-debug -R ^shark.tools.gltf-geometry$ --output-on-failure
```

12c implements bounded PNG/JPEG decoding, semantic color/data texture usage,
samplers and mip generation after pinning the decoder. Preserve every current
case, source/conversion limit, allocation control and independent FeatureWorld
mapping. Complete materials, identity-only external source integration, cache/job
publication and usable optional-extension fallback remain 12d; full REQ-12 closes
only in 12e. Source conversion returns C++ values, not a serialized payload or
graphical scene renderer. Terminal/editor adapters remain 15/16, cooked output
31 and animation M06.

Suggested commit: `feat(tools): convert static glTF geometry and hierarchy`.
