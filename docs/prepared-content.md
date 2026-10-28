# Prepared runtime content

13b supplies CPU mesh/scene loading through
[prepared_assets.hpp](../engine/core/include/shark/prepared_assets.hpp) and
[prepared_scene.hpp](../engine/core/include/shark/prepared_scene.hpp). Tools prepares
these values through [prepared_content.hpp](../engine_tools/include/shark/tools/prepared_content.hpp).
See [ADR 0033](architecture/0033-prepared-runtime-content.md). No API here initializes
graphics, imports a raw source file in Runtime, or provides an export command.

## Meshes and immutable ownership

`PreparedMeshAsset` contains its AssetId and dense ordered meshes. Each mesh owns
triangle primitives with vertices, uint32 indices and temporary material values.
Vertex attributes retain position, normal, tangent, UV0/UV1 and color. Material
values retain base-color factor and double-sided state. Neither source paths nor
source parser objects appear in the payload. Mesh indices in MeshRenderer refer
to this dense prepared order, not original glTF source indices.

The temporary debug/unlit profile does not contain texture images/samplers, PBR
terms or alpha-mode/cutout/blend behavior. Base-color alpha and vertex alpha are
retained values; they do not establish transparency rendering. The complete
12d cache payload remains unchanged for later material preparation. 13d implements
actual geometric rendering; 25-29 establish lighting/material quality.

`encode_prepared_mesh` and `decode_prepared_mesh` validate explicit engine values
and bytes. Magic is `SHKMESH1`, version 1. Payloads are capped at 2 MiB and logical
decoded allocation at 16 MiB, with bounded mesh/primitive/vertex/index counts.
Nonfinite attributes, invalid factors, malformed triangle/index structure,
invalid versions, trailing bytes, invalid identity and digest mismatch reject.
Bounds are derived from validated positions by [CPU extraction](scene-rendering.md)
rather than trusting serialized bounding boxes. This is not a packed GPU vertex layout.

`PreparedAssets::create` accepts explicit `{AssetId, payload span}` inputs, copies
decoded data into owned immutable storage and includes the two built-ins. At most
128 total assets, 32 MiB aggregate supplied bytes and 32 MiB logical decoded storage
are admitted. Supplied ID must match the payload's embedded ID. Duplicate IDs and
attempts to replace built-ins reject. Complete construction succeeds or returns
an error; caller input lifetimes do not escape into the result.

`find` returns shared const asset ownership; `resolve(MeshRenderer)` additionally
checks mesh index, regardless of visibility. Repeated references share one asset.
A loaded scene retains the immutable collection; acquired references can outlive
that scene or collection. There is no update/erase operation: replacement means
constructing another complete snapshot. Each snapshot has its own budget; callers
are responsible for releasing old scenes/references, not treating these limits
as a process-wide memory cap.

## Built-in geometry

| Asset | Stable reserved UUID | Mesh zero |
| --- | --- | --- |
| Cube | `00000000-0000-4000-8000-000000000013` | Unit cube centered at zero, 24 face vertices / 36 indices, outward normals |
| Plane | `00000000-0000-4000-8000-000000000014` | Unit XZ plane centered at zero, +Y normal, 4 vertices / 6 indices |

Use `builtin_cube_asset_id()` or `builtin_plane_asset_id()` in the same existing
MeshRenderer authoring command. Both are available in a fresh collection without
files, downloads or import sidecars. Geometry is deterministic engine-authored
MIT content, with CCW front faces and UV0; it uses the same validators/ownership
as prepared assets. Scaling and placement belong to the entity Transform.

## Scenes and load boundary

`PreparedScene` contains SceneId, optional WorldPosition origin and up to 4096
entities with explicit authored IDs, parent IDs, Name, Transform and optional
Camera/MeshRenderer. Source extras/project paths, command history, runtime handles,
network authority and credentials are absent. `SHKSCNE1` version 1 uses a 4 MiB
encoded bound and 16 MiB logical decoded bound. Both codec directions validate
complete values, canonicalize TRS and reject duplicate/dangling/cyclic identities,
invalid components, excessive depth and unrepresentable world transforms.
Entity input order may put children before parents; the loader resolves ownership
before creating the validated hierarchy. The source world/cell anchor remains
separate from local matrices and does not implement origin rebasing or streaming.

`LoadedPreparedScene::load(bytes, assets)` validates every mesh reference, then
returns a fresh existing Scene owner together with shared asset ownership and
the world anchor. Missing assets or out-of-range mesh indices reject before a
new owner is returned, including hidden meshes. No caller's existing scene is
modified on failure. Successful reload produces a different scene incarnation;
entity handles/references remain scoped to their original owner. `scene()` exposes
normal Core mutation for gameplay, not Tools authoring history. [CPU extraction](scene-rendering.md)
resolves renderer references again after subsequent Core mutations.

## Tools preparation

`prepare_mesh_asset(id, cached_gltf_payload)` validates the existing complete glTF
payload and copies selected dense meshes/attributes plus the temporary material
subset into runtime bytes. It does not reinterpret raw `.gltf`, external-resource
or transformed byte-import payloads. Existing ProjectFiles/catalog/cache/job
services still own source reads, import revisions and publication; preparation
does not add a raw-file bypass or replace the valid cache.
The trusted caller selects the correctly identified, fresh, audience-permitted
cache artifact through those services. A payload span alone does not attest cache
freshness, content audience, provenance or authority; the bridge does not invent
that metadata. Imported content cannot use either reserved built-in ID.

`prepare_scene(owner, expected_revision)` captures the existing AuthoredScene at
one committed revision and prepares its runtime fields. Stale revision and pending
work use the original capture guards. Source files, dirty baseline and history
remain untouched. `prepare_gltf_scene(asset, scene, cached_payload, node_ids)` maps
the converted imported hierarchy using caller-supplied durable IDs in node order.
It preserves TRS, parents and dense mesh references, uses `Node` for empty names,
and rejects invalid nonempty Names. It creates neither camera nor global origin;
those are explicit authoring/runtime inputs. Node ID counts/uniqueness must match.

These services return owned bytes only. Assignment 13e integrates application
loading and assignment 14 stages files for a standalone export. No new filesystem
resolver, implicit import, export CLI or final-cook claim is made in 13b.

## Public Core example

```cpp
#include <shark/prepared_scene.hpp>

shark::Result<std::unique_ptr<shark::LoadedPreparedScene>>
load_builtin_scene(std::span<const std::byte> scene_bytes) {
    auto assets = shark::PreparedAssets::create({});
    if (!assets) return assets.error();
    return shark::LoadedPreparedScene::load(scene_bytes, assets.value());
}
```

The input must be prepared scene bytes referencing only built-ins. Additional
assets must be supplied explicitly when constructing the collection.
