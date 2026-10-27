---
name: shark-asset-import
description: Implement or change Shark asset import and cache behavior with bounded decoding, stable asset identity, explicit format support and licensed offline fixtures.
---

# Shark asset import

Apply the [testing policy](../../../docs/testing-policy.md) before choosing checks.
Its 2026-10-27 cadence supersedes older frequency, matrix and evidence-work
instructions. Preserve existing cases, budgets, seeds and timeouts; preservation
does not require rerunning all asset/process families in every profile. Select
affected import and FeatureWorld regressions, then use the policy's subchunk or
assignment-closure scope. A private Tools dependency change needs boundary checks,
not automatically full Server suites, package deployments or GPU runs.

The [static glTF qualification contract](../../../docs/gltf-qualification.md)
connects the 12a-12d families to saved GLB/sparse/fallback owner tests and real
glTF job interruption/recovery. Preserve those paths, the original byte-import
controls and the offline licensed corpus. The pinned independent validator is
an opt-in additional oracle, not a runtime dependency or a download during tests.
Select future verification by actual changed behavior under the testing policy.

12d adds [materials and asset payloads](../../../docs/gltf-assets.md). Require the
typed identity-only resource importer and owner-supplied descriptor/source digest;
never interpret transformed byte bodies as original sources. Logical URI aliases
are normalized settings, not native file requests. Preserve the existing cache
and job bounds/fences; the qualification contract records the connected paths.

12c adds pure [texture conversion](../../../docs/gltf-textures.md) through the
same source owner. Preserve pinned memory-only PNG/JPEG, precision, strict PNG
framing/inflate/palette checks, JPEG structural/scan-work checks, nullable arena
cleanup and independent output/decoder/work budgets. Processing semantic belongs
to each usage: color, data and normal mip variants share only compatible rules.
Preserve source identities, sampler state, NPOT coverage and all earlier cases.
The twelfth FeatureWorld case exercises actual pixels; material/cache remains12d.

12b extends the same source owner with pure CPU geometry conversion under the
[geometry contract](../../../docs/gltf-geometry.md). Preserve effective sparse
bytewise reads, precharged output/work bounds, shared mesh instances and Core
TRS/inverse validation. Generated corner tangents use the explicitly documented
per-triangle basis. Extend the eleventh FeatureWorld case and independent
expectations with geometry changes; cache/material publication remains 12d.

12a starts the [five glTF checkpoints](../../../docs/roadmap/gltf-plan.md) under
[ADR 0028](../../../docs/architecture/0028-bounded-gltf-sources.md). Follow the
[source contract](../../../docs/gltf-source.md): pure inspection consumes explicit
immutable snapshots, never a ProjectFiles bypass or a worker filesystem callback.
Preserve per-file/aggregate/parser/accessor-work bounds and all allocation sweeps.
Private cgltf allocation failure unwinds only within its controlled C++ boundary;
MSVC requires the source-file-only `/EHc-` option after `/EHsc`. Source inspection
does not qualify geometry/image/material conversion or complete optional fallback.
Texture roles belong to usage; transformed ByteAssetImporter bodies are not raw
external source. Decoder integration remains 12c; full REQ-12 closes only in 12e.

11d connects the [asset qualification](../../../docs/asset-qualification.md)
lineage through the ninth native FeatureWorld case and actual asynchronous
process interruption/recovery. Preserve the earlier eight cases, all old limits,
59 synchronous interruption checks and the new `shark.tools.asset-jobs-process`
family (150 seconds). Root bytes alone cannot prove dependency invalidation:
inspect dependency IDs/keys and literal dependency payloads too. Fresh owners
reject old job scopes and reconcile real cache state; fixture markers are not
durable job journals. New APIs/formats/decoders are outside this qualification.

11c adds [bounded jobs](../../../docs/asset-import-jobs.md) under
[ADR 0027](../../../docs/architecture/0027-bounded-asset-jobs.md). Preserve the
four retained slots, one private worker, independent aggregate source/cache
budgets, cancellation/publication fences and explicit terminal take/retry.
Importer instances are exclusively borrowed; callbacks are serialized and cannot
reenter owner services. Worker views retain immutable revision ownership and
never call ProjectFiles/catalog/Scene. Extend the eighth native FeatureWorld case
and all six job mappings together; keep all earlier cases/limits.

11b implements [dependency imports and the development cache](../../../docs/asset-import-cache.md)
under [ADR 0026](../../../docs/architecture/0026-development-asset-cache.md).
Preserve `shark.tools.asset-import` (90 seconds, Catch seed 1337), the real
`shark.tools.asset-import-process` interruption fixture (120 seconds, 59 checks),
the seventh native FeatureWorld case, and all earlier cases/limits. Core content
hashing shares the existing private TLS crypto runtime; changes to that shared
runtime require the affected Core/Server regressions under the testing policy.
Keep source/payload/closure/cache budgets independent,
publication atomic, last-valid dependency closures retained, and recovery explicit.
Background jobs remain 11c; there are no source decoders or live-thread imports.

Read [AGENTS.md](../../../AGENTS.md),
[decoder/dependency choices](../../../docs/architecture/0002-dependencies.md),
[formats and identity](../../../docs/architecture/0004-formats-and-identity.md)
and the [exact source profile](../../../docs/architecture/0006-v1-profile.md).
Apply the [human and terminal-agent contract](../../../docs/architecture/0008-human-agent-authoring.md)
through its [implementation plan](../../../docs/roadmap/human-agent-plan.md).

11a adds persistent asset sidecars and a bounded revisioned catalog through the
existing ProjectFiles owner. Follow the [asset checkpoints](../../../docs/roadmap/asset-plan.md),
[ADR 0025](../../../docs/architecture/0025-asset-identity.md) and
[identity contract](../../../docs/asset-identity.md). Preserve
`shark.tools.asset-catalog` (60 seconds, seed 1337), the added native
`featureworld.asset-identity` case and all earlier workloads. The saved asset
reference is bounded scene extras, not a new component or scene schema. Sidecars
and sources move together; missing sources retain their ID, while duplicate IDs
reject a complete catalog candidate. Import/cache follows 11b above; background
jobs remain 11c, source decoders 12, audio 24 and HDRI 27. There is no import CLI command yet.
Use the existing native CPU checks from [building](../../../docs/building.md)
and `python tools/validate_contract.py` for shared-code/contract changes without
claiming import/cache behavior was exercised by identity tests. Run the actual
affected asset and FeatureWorld families in the profiles selected by the testing
policy, including the independent coverage inventory when adding public surfaces.

Keep external parsers and image/audio codecs in EngineTools. Runtime loads
validated engine-owned payloads; parser recognition never means rendering or
decompression support. Enforce the required-extension allowlist, core fallback
policy, alpha/color/UV semantics and unsupported-content diagnostics. Reject path
escapes, malformed arithmetic, excessive encoded/decoded sizes and nonfinite data
before publication, with bounded reads/allocations and useful source context.

Asset IDs survive moves and renames. Cache keys include declared source/settings/
tool/dependency inputs; cancellation and failure cannot publish partial results.
Stage and validate output before atomic replacement, preserving the last valid
asset according to the documented policy. Test invalidation, deduplication,
cancellation, rename/move, reproducibility and malformed source paths.

When import/cook services land, expose the same settings schema, discovery,
validation and jobs to the human editor and terminal agent. CPU import/cook work
must not require a running editor, window or GPU; separately capability-gate any
GPU-dependent preview or processing. Provide stable JSON diagnostics, job IDs,
progress/cancellation results and artifact manifests. Test revision conflicts,
retry/idempotency and the documented undo or recovery policy through both
adapters, including a human edit during an agent job. Never publish stale output
over newer source/settings or require GUI interaction to resolve a CPU job.

Keep fixtures local and licensed with origin/license metadata; ordinary tests
must not download assets. Cover embedded/external glTF, sparse accessors,
negative/nonuniform transforms, missing images, required extensions and decoder
failures appropriate to the selected profile. Update public version policy,
coverage, FeatureWorld when available, dependency notices and the handoff.

The [MMORPG plan](../../../docs/roadmap/mmorpg-plan.md) extends static import in M05/M06
with cell/collision/navigation data and a bounded skeletal profile. Preserve the
intermediate unsupported-content errors until those behaviors exist. Test streaming
dependency closure, cancellation and per-cell budgets; keep server collision/rules
roots separate from client visual/audio payloads and never cook private server
content into a client package. Seamless streaming must include collision readiness.
