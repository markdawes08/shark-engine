---
name: shark-asset-import
description: Implement or change Shark asset import and cache behavior with bounded decoding, stable asset identity, explicit format support and licensed offline fixtures.
---

# Shark asset import

Apply the [testing policy](../../../docs/testing-policy.md) and read [AGENTS.md](../../../AGENTS.md)
plus the assigned increment. Use [ADR 0002](../../../docs/architecture/0002-dependencies.md),
[identity/version rules](../../../docs/architecture/0004-formats-and-identity.md) and
[the declared profile](../../../docs/architecture/0006-v1-profile.md) before changing
formats/decoders. Existing contracts and handoffs hold detailed limits and evidence;
do not copy their checkpoint chronologies into new instructions.

## Existing ownership

- [Identity/catalog](../../../docs/asset-identity.md): exact sidecar schema, stable IDs,
  native bounded reads, revisioned inventory/pages, duplicate-ID rejection and
  retained identity for missing sources. ProjectFiles outlives borrowed services.
- [Import/cache](../../../docs/asset-import-cache.md): bounded captured dependency
  closure; exact source/settings/tool/dependency/target keys; strict index/payload
  validation; last-valid output and explicit reload/prune/clear/recovery.
- [Jobs](../../../docs/asset-import-jobs.md): one existing private worker, four retained
  slots, independent aggregate source/cache budgets, pending deduplication,
  cooperative cancellation, owner drain/publication and explicit terminal take.
  Importers are exclusively borrowed, serialize callbacks and cannot reenter owners.
- [Connected qualification](../../../docs/asset-qualification.md): preserve moves,
  owner edits during work, stale rejection, retry/reopen, actual process interruption
  and dependency-key/byte reproducibility. Fresh job scopes require cache
  reconciliation; observation markers never authorize durable replay.

Workers consume immutable captured bytes and never call ProjectFiles, catalog or
Scene. Preserve original source/metadata/catalog/cache revision fences. A failed,
cancelled or stale import cannot replace valid output; report actual publication
truth if cancellation arrives after commit. Source moves with sidecars retain IDs;
content/settings changes invalidate the correct dependency closure.

## Static glTF

Read the affected [source](../../../docs/gltf-source.md),
[geometry](../../../docs/gltf-geometry.md), [texture](../../../docs/gltf-textures.md)
and [material/asset](../../../docs/gltf-assets.md) contracts. Keep parsing/conversion
pure over explicit immutable snapshots. No raw-file resolver bypass, network URI
fetch or Scene/device mutation belongs in a converter/importer callback.

Preserve independent source/parser/accessor/work/decoded/output/allocation budgets,
pre-normalization URI validation, bytewise effective sparse reads and source-index
correspondence. Only the parser source uses MSVC `/EHc-` after `/EHsc` for its owned
allocation-failure boundary. Geometry preserves shared meshes, Core TRS/inverse
validation, local winding, world shear, instance mirror metadata and the documented
per-triangle tangent basis.

Keep the private pinned memory-only PNG/JPEG decoder, strict container/inflate/
palette and scan-work validation, nullable allocation cleanup, precision, complete
NPOT mips and per-use samplers. Color/data/normal semantics belong to usage, with
linear alpha. Unsupported required extensions reject by name; optional extensions
need a usable core fallback. Ignored extension-only images cannot invalidate that
fallback, while every core image reference keeps its normal checks. Parser
recognition never establishes rendering or unsupported decompression support.

`GltfResourceImporter` is identity-only; consume owner-supplied descriptors and
original source digests. Never strip transformed ByteAssetImporter envelopes to
recover source truth. Logical URI aliases are normalized settings, not file-open
requests. Preserve the independently versioned engine payload, exact references,
bounded readers and existing 2 MiB artifact / 16 MiB closure limits. A pure
conversion can exceed the cache limit and then reject publication without losing
last-valid output.

## Fixtures, adapters and verification

Keep fixtures offline and licensed with origin/license metadata. Test meaningful
accepted containers, sparse data, transforms, texture roles, malformed bounds,
missing resources and extension policies. Ordinary tests never download models.
The [glTF qualification](../../../docs/gltf-qualification.md) connects saved sources,
owner/cache/jobs and real interruption/recovery; keep the earlier byte-import
controls. Its pinned independent validator is an opt-in additional oracle, not a
runtime dependency or substitute for engine checks.

Choose affected import and FeatureWorld regressions under the testing policy.
Source/parser/allocator changes need corresponding failure sweeps; concurrency and
publication changes need their real ordering/recovery cases. Private Tools changes
do not automatically rerun Server, unrelated packages or GPUs. Update public
version policy, examples, coverage, licenses and handoff when those surfaces change.

Human/terminal adapters use the same settings, discovery, jobs and revision-aware
services under the [human/agent plan](../../../docs/roadmap/human-agent-plan.md).
CPU import/cook needs no GUI/GPU; capability-gate device-dependent previews.
Expose structured progress/cancellation and artifact outcomes. Keep server collision/
rules roots separate from client visual/audio content, and never package private
server data for clients. M05/M06 own streaming/collision/navigation/animation;
retain explicit unsupported behavior until those owners implement it.
