# Static glTF materials and development assets

Owner: 12d / REQ-12. Follow [ADR 0031](architecture/0031-gltf-material-assets.md),
the [glTF checkpoints](roadmap/gltf-plan.md), [source](gltf-source.md),
[geometry](gltf-geometry.md), [texture](gltf-textures.md) and
[cache/job](asset-import-jobs.md) contracts. All operations are CPU Tools services.

## Material values

`convert_gltf_materials` reuses the validated source owner and scene selection.
It returns reachable explicit materials in ascending source-index order. Base
color defaults to white, metallic and roughness to one, emissive to zero, normal
scale and occlusion strength to one, alpha mode to OPAQUE, cutoff to 0.5 and
double-sided to false. Base color, metallic, roughness, emissive and occlusion
values have core [0,1] bounds. Normal scale must be finite; cutoff is finite and
nonnegative. OPAQUE, MASK and BLEND are retained values, not rendering promises.

Each material has five optional slots indexed by `GltfTextureRole`. A slot keeps
the source texture index and UV set; resolve it against texture usages with the
same source material and role. Selected primitives must provide the referenced
UV0/UV1. A primitive with no source material uses default factors and no textures;
there is no synthetic persistent material ID. Material records and names have an
independent 2 MiB limit; the retained source report keeps the separate source bounds.

Optional `KHR_texture_basisu`, `EXT_texture_webp` and `MSFT_texture_dds` source
images used only by those ignored extensions do not need to decode or resolve.
Core texture references take precedence: an image referenced by core is never
excluded by that rule. Required extensions still reject. Arbitrary invalid unused
core images still fail the existing checks. Supplied ignored-image mappings stay
subject to input bounds and accounting, but do not appear as effective core
resources. This supports a usable core PNG/JPEG fallback without claiming compressed
texture support.

## Engine-owned payload

`convert_gltf_asset` combines geometry, textures and materials from the same
immutable snapshots and selected scene. `encode_gltf_asset` and
`decode_gltf_asset` exchange a versioned engine-owned binary representation of
those values, including source correspondence. It contains converted vertices,
indices, hierarchy/transforms, material values, per-use samplers and decoded mip
bytes; it does not embed a glTF document for later parsing.

Encoding is deterministic and bounded by `asset_import_maximum_payload_bytes`
(2 MiB). Owned decoded values have a separate 16 MiB logical allocation charge;
this excludes transient validation containers and is not a process RSS promise.
Decoding checks the version, lengths, digest, finite values and reference
structure before accepting an owned result. Counts/allocations and cancellation
remain bounded. Unknown versions, malformed/corrupt payloads and oversized output
fail through Result. Exact format details belong to the engine codec, not callers
performing offsets or casts. This development format is not the later cooked
runtime format or a packed GPU vertex layout.

The 12b/12c pure conversion budgets remain independent. A model that fits those
budgets can exceed the smaller development payload limit and be rejected by
import. Do not silently enlarge cache/job budgets, split assets implicitly or
publish partial output to fit it.

## Typed resources and owner integration

Register `GltfAssetImporter` (`shark.gltf`, kind `gltf`) and
`GltfResourceImporter` (`shark.gltf-resource`, kind `gltf-resource`), version 1.
Resource settings are `{}`; resources have no child dependencies and preserve
their original bytes in a typed identity/digest envelope. `ByteAssetImporter`
outputs are rejected even when their transform happens to be identity. Dependency
views now expose the actual captured child's kind, importer, versions and source
digest alongside its ID, key and payload. Those views borrow the same immutable
captured artifact for the callback's duration.

glTF settings have this form:

```json
{"path":"models/tree.gltf","resources":[{"path":"textures/leaves.png","asset":"20000000-0000-4000-8000-00000000000b"}],"scene":0}
```

`path` is the required logical root `.gltf`/`.glb` path; `scene` is optional.
Resource mappings name logical project paths reached by glTF URIs, with unique
paths and IDs matching the root's direct metadata dependencies. These aliases
are explicit authoring settings, not automatic filesystem discovery. For example,
`../textures/leaves.png` resolves relative to the logical `models/tree.gltf`.
ProjectFiles reads the catalog source belonging to the mapped AssetId, even if
its actual filename differs. Sidecar/source moves preserve that identity; edit
the mapping when changing the intended logical association. Missing, ambiguous,
unused or wrongly typed resources reject. Settings retain the existing 4096-byte
limit and normalized settings participate in cache keys.

Source capture, native containment, client/server audience checks, dependency
closure, cache keys and original revision fences remain in AssetImports.
Synchronous prepare and the existing bounded worker use the same importers.
Callbacks never open paths, mutate Scene or publish a snapshot. Publication,
stale rejection, cancellation, retained terminal outcomes, explicit retry/reopen
and last-valid output keep the existing cache/job contract.

## Public CPU example

Captured snapshots can be converted and encoded without an editor or device:

```cpp
#include <shark/tools/gltf_asset.hpp>

shark::Result<std::vector<std::byte>>
prepare_static_asset(const shark::tools::GltfSourceInput& captured) {
    auto converted = shark::tools::convert_gltf_asset(captured);
    if (!converted) return converted.error();
    return shark::tools::encode_gltf_asset(converted.value(), captured.stop);
}
```

For cache publication, register the two importer instances with the existing
AssetImports owner and use its `prepare`/`publish` or `submit`/`drain_jobs` flow.
Check the Result and actual commit/terminal status at each boundary. Renderer
consumption and terminal/editor adapters retain their later checkpoints.
13b adds [pure preparation bridges](prepared-content.md) from these validated
cache bytes to Runtime-safe mesh/scene data. The full cache and its import limits
remain unchanged; the prepared temporary unlit profile does not contain the full
material/texture representation.
