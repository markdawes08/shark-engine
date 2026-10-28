# Camera and MeshRenderer

Assignment 13a extends the existing CPU Scene and AuthoredScene owners. Public
values live in [render_components.hpp](../engine/core/include/shark/render_components.hpp).
Follow [ADR 0032](architecture/0032-render-components.md) and the
[checkpoint plan](roadmap/scene-rendering-plan.md). These are authored presentation
values, not gameplay authority, replicated state, an asset loader or a renderer.

## Values and validation

| Camera field | Default | Constraint |
| --- | --- | --- |
| `projection` | `CameraProjection::perspective` | `perspective` or `orthographic` |
| `vertical_fov_radians` | pi / 3 | Finite, strictly between zero and pi |
| `orthographic_height` | 10 meters | Finite and strictly positive; full vertical span |
| `near_clip` | 0.1 meters | Finite; positive for perspective, nonnegative for orthographic |
| `far_clip` | 1000 meters | Finite and strictly greater than near |

Both projection modes validate every stored parameter, including the inactive
mode's field. Switching projection therefore does not expose unchecked data.
`validate_camera` and `validate_component` use the same checks as Scene writes.
Aspect ratio and active-camera selection are later viewport/extraction inputs.
The existing entity Transform supplies pose, with -Z forward and +Y up; no
position or rotation is duplicated in Camera. Projection math follows ADR 0005
in 13c, including finite zero-to-one depth and a single projection-Y inversion.

`MeshRenderer` requires `AssetId asset`, with `uint32_t mesh_index` default zero
and `bool visible` default true. The ID is a validated, nonnil UUIDv4. Mesh index
selects a mesh in the prepared asset; its range against an actual asset is checked
by the later resolver. Core stores unresolved references without doing I/O.
All primitives of the selected mesh belong to that renderer. No material override,
PBR claim, GPU handle or resource pointer is stored here. Visibility is presentation
state and does not authorize gameplay or replace recipient privacy.

## Ownership, discovery and edits

`EntitySeed`, `SceneEntityState` and `SourceEntity` carry optional `camera` and
`mesh_renderer` values. `Scene::set_camera(handle, optional<Camera>)` and
`set_mesh_renderer(handle, optional<MeshRenderer>)` add/update present values;
`std::nullopt` removes them. Matching `defer_set_camera` and
`defer_set_mesh_renderer` use the existing bounded FIFO. Invalid values and stale
handles reject without mutation; true no-ops preserve revision. Owner-thread,
visit/pending-write guards, candidate copying and checked lifetime remain intact.

Schemas 3 / `shark.camera` and 4 / `shark.mesh_renderer` are optional and removable.
Property IDs 5..9 identify projection, FOV, orthographic height, near and far;
10..12 identify asset ID, mesh index and visibility. Discovery supplies typed
defaults, ranges, units, enumerants and scene-owner mutability. Required asset
identity has no automatic default. Cross-field camera constraints are enforced
by the complete validator; editing returned metadata never changes policy.

Query flags `camera` and `mesh_renderer` default false to preserve existing
three-flag identity-only queries. Set each flag explicitly to receive its owned
optional value. With the flag true, an empty optional means the entity lacks that
component. With it false, an empty optional is an omitted field. The caller's
selection distinguishes those cases; revisioned pagination is unchanged.

`CreateEntityCommand` accepts the two optional values. `SetCameraCommand` and
`SetMeshRendererCommand` use optional values for add/update/remove through the
shared AuthoredScene owner. Preview, ordered atomic batches, undo/redo, retained
retry, capture and activation preserve every field and component absence. Exact
retry comparison includes projection, every floating-point bit pattern, asset ID,
mesh index, visibility and presence, including values on create commands.
Save retains history; reload replaces incarnation and clears history as before.

## Source compatibility

The source envelope stays version 1. The existing strict `components` object
allows optional `shark.camera` and `shark.mesh_renderer` objects. Each requires
integer `schema_version: 1`; MeshRenderer also requires `asset_id`. Other fields
use the defaults above when absent. Camera projection is serialized as
`"perspective"` or `"orthographic"`; the remaining keys match the Camera fields.
Renderer keys are `asset_id`, `mesh_index`, `visible`. Unknown fields, versions,
invalid types, nonfinite numbers and invalid IDs reject the complete candidate.
An absent component is omitted, never serialized as null or synthesized on load.

Existing sources and legacy-0 migration retain their meanings. Old readers reject
new component names explicitly; they cannot silently drop authored values. New
readers reject future component versions. JSON source remains private to Tools;
Runtime consumes prepared data only when 13b lands.

## Public CPU example

```cpp
#include <shark/tools/authored_scene.hpp>

shark::Result<shark::tools::AuthoringCommandReply>
set_camera(shark::tools::AuthoredScene& owner, shark::AuthoredEntityId entity,
           shark::SceneRevision revision) {
    shark::Camera camera;
    camera.projection = shark::CameraProjection::orthographic;
    camera.orthographic_height = 20.0;
    return owner.execute(shark::tools::SetCameraCommand{entity, camera}, revision);
}
```

The caller must check both the outer Result and reply outcome. A successful
component edit does not prove an asset resolved or any pixels were rendered.
