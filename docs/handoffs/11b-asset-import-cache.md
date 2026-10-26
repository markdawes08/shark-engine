# 11b: Dependency imports and reproducible development cache

Date: 2026-10-26. **11b is PASS under the current-machine scope.** Base:
`4da609e` on initially clean `main`. Changes remain unstaged/uncommitted; the
user owns staging, commits and pushing. **Next: 11c, bounded background jobs.**
Full REQ-11 remains NOT_RUN pending 11c/11d.

Follow the [four checkpoints](../roadmap/asset-plan.md),
[ADR 0026](../architecture/0026-development-asset-cache.md),
[cache contract](../asset-import-cache.md) and
[qualification receipt](../coverage/evidence/11b-asset-import-cache.json).

## Delivered behavior

EngineTools adds a focused trusted native importer interface with explicit
ID/kind/tool/payload versions and a bounded discoverable settings schema.
ByteAssetImporter implements version-1 engine-owned byte payloads and explicit
identity/uppercase-ASCII transformations. It uses no new dependency or decoder.
The Core SHA-256 value/API reuses pinned private Mbed TLS/PSA. TLS and content
hashing share the existing crypto lock, initialization, connection counter and
shutdown lifetime, extracted into one private runtime.

AssetImports borrows the existing ProjectFiles/catalog and registered importers,
which must outlive it. Synchronous offline preparation captures a bounded input
closure without publication. Dependency traversal uses an explicit bounded stack.
Keys include project/asset UUID, source and normalized settings digests,
kind/audience, importer/payload/pipeline versions, target ID/side and ordered
dependency IDs/keys. Coordinated source/sidecar moves preserve identity and keys;
provenance/extras are not importer inputs. Client/server privacy rejects cross-side
edges; shared content depends only on shared content.

Matching keys reuse validated artifacts without calling import. Publication
rechecks owner/cache/catalog and exact source/metadata revisions, then replaces
one complete cache snapshot through the existing native staged file service.
Changed or failed inputs preserve last-valid output with fresh/stale/missing
status and structured diagnostics. Explicit reads return owned artifact values.
External writers retain ProjectFiles' coordination rules; revisions are not a
hostile-filesystem sandbox. Check committed FileDurability warnings as published
outcomes, never rollback.

The disposable `.shark-development-cache` has an explicit little-endian header,
version-1 canonical JSON index and binary payloads, whole-container/payload
digests, checked contiguous ranges, recomputed keys and complete dependency
closure. Corrupt, foreign or unsupported snapshots reject. Old opaque artifacts
retain their actual versions; only matching registered versions undergo that
importer's semantic validation. Prune retains every last-valid dependency closure;
clear atomically publishes an empty snapshot. Neither changes source/metadata,
scene history, references or source schemas.

Bounds remain explicit: 128 closure nodes, 1 MiB/source, 2 MiB/payload, independent
16 MiB source/payload totals, 128 cache entries/bindings, 4 MiB index, 24 MiB
snapshot and sixteen registered importers. Settings/schema objects are at most
4096 bytes. `.shark-cache-stage-UUID` admits at most one unpublished stage, and
blocks further publication until explicit recovery. Recovery refuses malformed,
multiple, aliased or oversized stages and preserves `.shark-stage-*` source-save
evidence. Peak managed cache disk use is one snapshot plus one stage, at most
48 MiB. Generated cache/stage files are ignored; metadata/source/skills/docs stay
eligible for version control.

The seventh native case, `featureworld.asset-import`, uses the saved asset UUID,
one scratch ProjectFiles/catalog/import owner and an actual dependency. It
connects discovery, pure prepare, publication, reuse, stale invalidation,
retained reads, usage, prune/reopen/reload, recovery and clear while leaving its
AuthoredScene owner unchanged. Independent inventory and manifest mappings grow
together to **34 CPU scene surfaces and one external GPU command**. Each of the
ten new surfaces resolves to actual passing profile/test results in the receipt.

## Qualification

| Profile | Passing CTest results |
| --- | ---: |
| Windows Debug | 112 |
| Windows Release | 112 |
| Windows ASan | 112 |
| WSL GCC Debug | 105 |
| WSL Clang ASan/UBSan | 105 |
| WSL Server Debug | 68 |
| WSL Server ASan/UBSan | 68 |
| Standalone Windows Tools | 21 |
| Standalone WSL Tools | 21 |

**682 full-profile results plus 42 standalone Tools checks** qualify without
remaining failures/skips. They select actual passing results from complete current
code runs and final boundary reruns, with per-test JUnit provenance. The final
validation-only corrections change three CMake files and two closure-validator
Python files; engine/CPU fixture objects and archives remain byte-identical through
the final rebuilds. Eight native GPU diagnostic fixtures refresh their embedded
source stamp, and their CPU CLI/discovery/supervisor consumers rerun. Original failed reports
remain unchanged. Both full Server suites are refreshed
because the shared private crypto runtime changed. The implementation freeze
contains **625 inputs**. All twelve public Tools examples compile using only
public engine headers and GLM with MSVC/GCC/Clang (**36 compiles**); compiler include
audits exclude private dependency/native headers. **24 actual sanitizer objects**
verify compile options and ASan/UBSan symbols, with fatal runtime options retained.

New families are `shark.core.content-digest` (15 seconds, three cases),
`shark.tools.asset-import` (90 seconds, 22 cases), both Catch seed 1337, and
`shark.tools.asset-import-process` (120 seconds, **59 real process checks**).
Tests cover independent SHA-256 vectors and shared concurrent crypto use,
exact-key invalidation/reuse/reproduction, privacy, graph errors, stale revisions,
callback errors, malformed payloads, corrupt snapshots, resource bounds,
capacity/pruning, native failure/durability outcomes and Unicode restart recovery.
The full 128-node closure publishes, reopens and prunes at the original limit.
Source and payload total limits are tested independently.

The real process fixture exits at five native publication phases, reopens exact
old/new snapshots, checks unchanged source/metadata, blocks orphaned publication,
and explicitly recovers only its cache stage. Earlier catalog workloads retain
18 cases/6190 assertions and the 4096-path/1500-level fixture. Earlier runner and
process workloads remain; FeatureWorld now has seven native cases/25 process
checks. Connected qualification retains 19 checks/15 rejection controls and its
90-second timeout. No acceptance limit or prior timeout was relaxed.

All **35 final process snapshots** (five phases across seven Tools-enabled
profiles) reproduce the same complete cache bytes and root key across MSVC,
GCC and Clang. The receipt records their actual hashes and source results.

Protected native/WSL Runtime and Server builds pass with Tools/JSON excluded.
The native Runtime package retains 35 inspected payload/license files and passes
standard-user Unicode relocation with a system-only PATH. Current Server/CPU
Runtime launch in the retained SDK-free, network-isolated WSL runtime, and the
Server package closure passes. No host/WSL resource settings changed. Graphics
implementation/dependencies are unchanged; previous hardware evidence is retained
and no new GPU qualification is claimed.

Initial failures are retained in `out/11b-work`: recursive input traversal
overflowed the Windows stack at the fixed workload; the implementation now uses
bounded explicit DFS. Narrow Windows process argv lost Unicode paths; the fixture
now uses the existing UTF-8 wmain pattern. The added aggregate-output fixture
initially tried to create existing metadata and a diagnostic ternary needed Catch
macro parentheses; both fixture mistakes were corrected before the freeze.
Stale binaries from a failed build are not counted as qualification.

WSL Debug's full run passed 104/105 checks; its dependency graph check and an
isolated retry exceeded the unchanged 10-second timeout. Profiling identified
repeated canonical-path resolution across shared roots: 4090 filesystem calls.
The checker now resolves each immutable graph path once per invocation while
retaining every root/property/domain policy check. Profiled execution of the same
graph drops from 13.9 to 5.4 seconds with 353 filesystem calls. This is a local
checker measurement, not a capacity claim. A new rejection control allows a path
for earlier Storage roots and rejects that same normalized path for Tools.
Both graph families and the Server closure unit family rerun in all nine profiles,
preserving 10/30-second family
and five-second rejection limits. All three allowed and 33 rejection controls
pass; earlier controls remain. Protected build checks refresh as well, with
unchanged binaries. No engine/test C++ behavior input changes in this correction.

The first actual protected Server closure audit also rejected the private Core
filename `crypto_runtime.cpp`, because its dependency-name heuristic matches
`runtime`. The reviewed allowance is limited to that exact Core source/header,
and the compiler unit must have the exact source and native Core object identity.
All remaining compiler flags, source/include ownership, discovered headers,
targets, archives, libraries and package rules remain checked. Three regression
tests bring the closure family to 43 cases, including generated/misplaced/name
lookalikes, flag injection and mismatched compiler inputs/outputs. The final real
package closure and SDK-free launches pass; the original failed audit remains.

## Rerun and retention

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset tools-debug --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset tools-debug --show-only
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset tools-debug -R ^shark.tools. --output-on-failure
```

Reuse canonical build/profile directories and dependency archive caches. Native
deployment copies are managed and removed after successful tests. The successful
WSL Server deployment is archived with verified hashes/original paths and its
exact expanded files removed; see the retention receipt. Never delete historical
evidence/WSL roots wholesale. Closing status documentation and REQ-11 checkpoint
evidence are reconciled separately from the unchanged qualified source freeze.

11c adds bounded private workers, progress, matching-work deduplication,
cancellation and late-completion fences through this established prepare/publish
path. 11d closes full REQ-11. Source decoders remain 12/24/27, final cooked format
31, terminal/editor adapters 15/16 and cell streaming its later owner. There is
no live-thread importer, import CLI or game-authoring UI in this checkpoint.

Suggested commit: `feat(tools): add reproducible dependency-aware asset cache`.
