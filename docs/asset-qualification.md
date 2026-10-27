# Integrated asset qualification

Owner: 11d / REQ-11. This gate connects the existing
[identity/catalog](asset-identity.md), [dependency/cache](asset-import-cache.md)
and [background-job](asset-import-jobs.md) services under ADRs
[0025](architecture/0025-asset-identity.md),
[0026](architecture/0026-development-asset-cache.md) and
[0027](architecture/0027-bounded-asset-jobs.md).
Qualification status and exact executions belong in the checkpoint handoff and
receipt. This contract introduces no new source/cache/payload version or public
authoring API. Source decoders remain 12; terminal/editor adapters remain 15/16.

## Connected saved-world lineage

`featureworld.asset-pipeline` extends the saved FeatureWorld with one connected
scratch project/scene, the existing saved root AssetId, one declared dependency,
one catalog and one AssetImports lifetime before explicit reopen. Each phase
uses the preceding phase's actual sidecars/cache. The existing AuthoredScene owner
and saved source references remain intact; no second mutable scene is introduced.

The case publishes a real root/dependency import through the private worker and
owner drain. Coordinated source-plus-sidecar movement must preserve the root ID,
saved reference, content key and matching artifact reuse. A controlled importer
gate proves the worker is running while the owner changes dependency source and
settings. The captured completion must fail its original fence, leave exact cache
bytes unchanged and preserve last-valid content. Only explicit take/reconciliation
and a new job may publish the current input.

Independent assertions inspect literal root/dependency payload bodies, the actual
dependency ID/key link, new versus old root/dependency keys and retained old bytes.
The root byte payload alone cannot prove dependency invalidation. Cancellation
before publication must preserve the cache; cancellation after publication must
return the committed result. Missing/private dependencies must diagnose without
discarding last-valid output. Explicit reopen uses fresh owner scopes while
retaining persistent asset IDs and the saved scene reference. Prune, reload,
recovery and clear keep their existing explicit behavior.

All eight earlier FeatureWorld cases, four saved entity IDs, old mappings and
workloads remain. The new case extends actual execution, independent catalog
expectations and implemented manifest mappings together. There are no placeholder
Lua bindings or new gameplay privileges.

## Abrupt asynchronous publication

`shark.tools.asset-jobs-process` launches the real AssetImportProcess fixture in
separate processes. It uses a two-asset closure and actual submit, private-worker
computation, ready completion and owner drain. The old synchronous process modes,
59 checks and their 120-second family are preserved independently.

The new family has 113 checks across 39 children, a 150-second CTest limit and
20-second child deadlines. Its seven
controlled abrupt exits cover a running importer, ready-before-drain and the five
native publication phases: staged, written, flushed, before-publish and published.
These are actual process exits without C++ cleanup, not returned mock failures.
An explicit owner signal orders marker publication before a worker-side exit;
no arbitrary sleep chooses the interleaving.

Each phase starts from a real published old snapshot and changes the dependency
source. Before publication, exact old cache bytes and both old payloads must
remain usable; after publication, the complete new root/dependency snapshot must
be present. Ordinary source-save recovery evidence and both sidecars are retained.
Only the four native prepublication phases leave a cache stage. A retained cache
stage blocks another asynchronous publication until explicit bounded recovery.

A small fixture-only marker records the original job scope/sequence before exit.
A fresh owner rejects that foreign identity on cancel/take and reconciles actual
cache state. It never treats the marker as a durable job journal. A new async
request recovers current content; repeated recovery/import is idempotent and
must report matching reuse/no-op publication. Root/dependency IDs, keys and literal
payloads are checked on reopen. Final snapshot bytes must reproduce across all
seven exit points and qualified compilers/profiles.

The supervisor requires the expected child exit code, clean machine JSON on normal
exits, no stray stdout on controlled abrupt exits, no timeout and no cleanup error.
Unicode/space-containing roots exercise actual native path conversion. Reports
record each child/check and the final snapshot hashes; synthetic rejected-report
controls cannot substitute for these real executions.

## Preserved limits and evidence

The existing catalog/import/job limits remain fixed: catalog inventory 4096 entries
and 16 MiB, 64 KiB sidecars, 128-node import closures, independent 16 MiB source/
payload totals, 128 cache artifacts/bindings, four retained jobs and independent
32 MiB aggregate captured-source/reusable-payload budgets. Peak managed cache disk
remains one snapshot plus one stage, at most 48 MiB. The fixture marker is bounded
test evidence outside the production cache format.

Preserve the 4096-path/1500-level catalog workload, positive 128-node import/job
closures, all 18 catalog cases/6190 assertions, 22 import cases/4361 assertions,
25 job cases, original interruption checks and earlier network/storage/authoring
workloads. Catch seed remains 1337. No timeout, payload limit, image baseline or
dependency rejection is relaxed to obtain a pass.

Closing evidence resolves every current asset surface to actual passing CTest and
FeatureWorld executions. Record exact source inputs, compiler/build profiles,
test inventories, JUnit provenance, case seeds/counts, process checks, actual
sanitizer object symbols, public-header example compiles and protected packages.
A declaration or raw count is not a passing execution. Keep failed/partial attempts
and their explicit resolution; never relabel a failed full run as uninterrupted
green evidence.

Qualification may partition a profile's complete inventory into functional and
I/O-heavy selections, with a shared local coordination gate around dependency
fixtures and contract validation. The selected sets must be disjoint and their
union exactly the profile inventory, with original per-test JUnit provenance and
unchanged CTest limits. Describe such evidence as partitioned executions. The
coordination changes no engine scheduler, host resource or WSL kernel setting.

CPU Tools must configure/build/test without Client/Editor/display/GPU dependencies.
Runtime and Server keep their protected source, link and installed-package
boundaries; registered source importers, JSON, cache/job state and fixture markers
never enter those packages. Core/Server source changes require fresh full Server
suites; otherwise identify unchanged inputs and retained full-suite evidence,
while refreshing protected builds/packages. Actual GPU checks remain independent
and are not inferred from CPU qualification.

Reuse canonical build directories and dependency archives. Remove successful
temporary deployments, retaining manifests/results and verified archives with
original paths/hashes. Never remove historical evidence, archives or retained WSL
userspaces wholesale. Full REQ-11 passes only after all four
[asset checkpoints](roadmap/asset-plan.md) have qualified under current-machine scope.
