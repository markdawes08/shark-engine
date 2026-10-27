# 12a: Bounded glTF import foundation

Date: 2026-10-27. **12a is PASS under the current-machine scope.** Full REQ-12
remains NOT_RUN pending 12b-12e. Base: `20b028d` on initially clean `main`.
Changes remain unstaged/uncommitted; the user owns staging, commits and pushing.
**Next: 12b, static geometry and hierarchy.**

Follow the [five checkpoints](../roadmap/gltf-plan.md),
[ADR 0028](../architecture/0028-bounded-gltf-sources.md),
[source contract](../gltf-source.md), [dependency review](../dependencies/cgltf.md)
and [receipt](../coverage/evidence/12a-gltf-foundation.json).

## Delivered behavior

`inspect_gltf_source` consumes one immutable `.gltf`/`.glb` snapshot and explicit
external path/AssetId/byte mappings. It returns owned counts, selected-scene
information, buffer/image references, digests and bounded diagnostics. It performs
no filesystem/network/cache/Scene operation and exposes no parser pointer.
Workers keep the existing captured-byte boundary; ProjectFiles raw reads remain
private to the existing import owner. Public headers need no cgltf/JSON includes.

Validation covers container/chunk arithmetic, source version policy, required
extensions, portable percent-decoded relative URIs, exact external mappings,
buffer/accessor packing/ranges, sparse indices and effective primitive indices.
URI encoding/components are validated before dot normalization can erase them.
Iterative hierarchy checks reject cycles/multiple parents. Required unsupported
extensions and skin/animation/morph content fail explicitly. Optional extensions
produce source diagnostics; complete usable material/image fallback remains with 12d/12e,
including a valid core image alongside extension-only compressed resources.

Fixed budgets retain 1 MiB per encoded source, 16 MiB supplied/resolved totals,
127 resource references, 4,096 entries per table and 16,384 combined entries,
262,144 elements per accessor and 4,194,304 aggregate elements. Sparse-index
storage is bounded to 16 MiB; effective index maxima are cached once per accessor.
JSON depth 32, 65,536 JSON nodes and a 32 MiB live parser arena have separate bounds.
Earlier source-codec limits
are unchanged. See the contract for exact semantics and diagnostics.

cgltf 1.15 is pinned at commit `360db1a95480fe102ae9c69b27c5d101167ff5ba`, with
the exact bounded archive, complete source inventory and MIT license verified.
It is a private authoring dependency. No upstream build executes. Existing
source-document and storage dependency fixtures retain their earlier cases and add
exact private-parser population/compile checks. Protected targets acquire no
parser or JSON. No dependency rejection or previous timeout is weakened.

Actual failure injection exposed unsafe upstream cleanup after a null allocation.
The private C++ allocator now aborts through an owned exception boundary; an
intrusive arena releases partial blocks without walking incomplete structures.
Only `gltf_source.cpp` adds MSVC `/EHc-` after ordinary `/EHsc`, permitting internal
C-linkage unwinding. The intentional option override can emit command-line
D9025; source warning flags remain enabled. An independent fixture checks exact
ordering and absence from other sources. Debug/Release/ASan sweeps all pass with
zero outstanding allocations. Metadata-only cgltf validation follows engine
bytewise checks, avoiding unaligned casts and sparse-base-only false rejection.

The tenth native FeatureWorld case uses explicit authored triangle bytes,
independent expected counts/digest and real valid/invalid inspections. It keeps
the original AuthoredScene, all four saved entity IDs and source files intact.
All nine earlier cases remain. Independent mappings now have 41 CPU surfaces and
one external GPU command; FeatureWorld has 28 process checks. Forty-one new Catch
cases retain seed 1337 and at least 1,794 assertions. Ten licensed original fixture
inputs cover JSON/GLB, external/embedded/sparse data and image-source bytes.

## Qualification

| Selection | Passing CTest outcomes | Extent |
| --- | ---: | --- |
| Windows MSVC Debug | 116 | Full inventory |
| Windows MSVC Release | 25 | Tools selection |
| Windows MSVC ASan | 25 | Tools selection |
| WSL Ubuntu GCC Debug | 109 | Full inventory |
| WSL Ubuntu Clang ASan/UBSan | 25 | Tools selection |
| Standalone Windows Tools | 25 | Tools selection |
| Standalone WSL Tools | 25 | Tools selection |
| Isolated WSL Server Debug | 68 | Full inventory |
| Isolated WSL Server ASan/UBSan | 68 | Full inventory |

All 486 outcomes pass: 225 from full client Debug inventories, 125 from Tools
selections and 136 from fresh full Server inventories. The 50 standalone Tools
outcomes are a subset of the 125 Tools selection outcomes.
Full and selected runs use disjoint functional/I/O partitions with original
per-test JUnit provenance; these are not described as uninterrupted full runs.
Earlier family names/labels/timeouts/WILL_FAIL/RNG seeds are preserved. No failed
result is relabeled, test omitted or limit increased. The receipt pins 515 source
inputs, implemented manifest, configuration, archive and ten execution helpers.

All 14 public examples compile under MSVC/GCC/Clang (42 compiles), with compiler
dependency scans proving public-header separation. Actual sanitizer symbols are
verified in 30 objects, including both new parser/test sources. Seven selections
that include Tools preserve the old 59 synchronous and 113 asynchronous interruption
checks. Within each fixture, 35 synchronous and 49 asynchronous snapshots reproduce
bytes and root keys; asynchronous snapshots also reproduce dependency keys.

Protected Runtime/Server configurations, dependency/link/package checks and
launches pass. The 35-file native Runtime package launches after Unicode relocation
as a standard user with system-only PATH. The 12-file Server package passes in a
read-only SDK-free, network-isolated WSL userspace. Successful native temporary
deployments are cleaned; Server files are hash-verified in an archive with original
paths before exact expanded-copy removal. Canonical builds/archive caches and
retained WSL userspaces are reused. No host/resource/kernel settings change.

The native audit initially rejected the historical shader bundle because its
receipt referenced the earlier dependency lock. That failed driver/audit remains
unchanged and explicitly recorded. A separate recovery prepared validation tools
offline, regenerated the bundle under the current lock and verified identical
compiled shader bytes from unchanged sources/recipe/flags. The fresh Runtime
package then passed relocation checks. No guard was weakened, historical bundle
overwritten, passing CTest repeated, or frozen source/harness changed for recovery.

The first recorded matrix was deliberately aborted after a late review found
vendor integer conversions outside their checked range. Its original ledger,
partial results, code and execution helpers are preserved in a verified archive
under `out/12a-work`; none is relabeled as this green selection. Private checked
integer hooks and sampler-domain validation close that gap, with a new dedicated
case covering overflowing attribute suffixes and sampler fields. All prior forty
cases remain. The current receipt uses a fresh immutable ledger and output path.

Historical preflight failures remain distinct: the initial allocation 10 SIGSEGV
and first stale FeatureWorld expectation have raw traces. A second stale
last-filesystem-case assumption was observed in the tool transcript; its raw
report was overwritten before retention and is identified as transcript-only.
Both fixture assumptions were corrected while retaining every old rejection
control. A WSL preflight script filename typo failed before tests; the corrected
preflight passed. These development attempts are not part of the 486 passing
frozen selections. Source/compiler preflights preceded the final freeze.

No new GPU qualification is claimed for this CPU foundation. Full geometry,
decoded pixels, material payloads, importer cache publication and final optional
fallback acceptance remain assigned to 12b-12e. Renderer 13, PBR 26 and skeletal
animation in M06 remain later work.

## Commands and next checkpoint

The receipt pins exact commands, helper/config hashes, inventories and raw logs:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/12a-verified/native.py
wsl.exe -d Ubuntu -u root --cd / -- bash /mnt/c/Users/mrkda/Projects/shark/out/12a-verified/wsl.sh
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/12a-verified/recover_native_audit.py
```

For the versioned focused checks, list the configured inventory first:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset tools-debug --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset tools-debug --show-only
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset tools-debug -R ^shark.tools.gltf-source$ --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset tools-debug -R ^shark.tooling.gltf-dependencies$ --output-on-failure
```

12b converts selected/default static scenes into checked engine-owned geometry
and hierarchy: sparse expansion, triangle topology, vertex attributes, generated
normals/tangents and transforms. Reuse the bounded source boundary; preserve
all 41 cases, aggregate limits, allocator controls and earlier workloads.

Suggested commit: `feat(tools): add bounded glTF source inspection`.
