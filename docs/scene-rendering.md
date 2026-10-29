# CPU scene extraction and cameras

13c adds [scene_render.hpp](../engine/core/include/shark/scene_render.hpp) to
EngineCore. It consumes the existing [Scene](scene-contracts.md) and immutable
[prepared assets](prepared-content.md), without initializing graphics or loading
files. See [ADR 0034](architecture/0034-scene-extraction.md). The [offscreen viewport](scene-viewport.md) consumes these snapshots in 13d;
application loading and connected acceptance follow the completed
[Runtime scene contract](runtime-scenes.md).

## Snapshot boundary

`extract_render_scene(scene, assets, options)` requires a live camera handle and
an exact `{scene_instance, revision}`. All Scene reads use its owner thread and
existing bounded inspection/query pages. Closed owners, stale revisions and
foreign/stale handles reject. Deferred writes are not applied: extraction reads
the committed state, including during an existing Scene visit. It invokes no
user callbacks and changes no owner, revision, history or scheduler.

The returned `RenderScene` owns a camera reference, viewport, origin, matrices,
statistics and a const view of ordered render records. Records follow stable
Scene slot order. Each record retains its checked entity reference, shared const
mesh asset, dense mesh index, complete origin-relative world matrix, normal
matrix, local/render AABBs and model reflection parity. One record represents
the entire mesh, including all its primitives/material values; it is not a GPU
draw or upload. Immutable leases survive entity destruction, asset collection
release or scene reload. Such snapshots are historical values, not authorization
to address a newer incarnation/revision/origin.

Every MeshRenderer reference is resolved again, including `visible=false`.
Missing assets or mesh indices reject the complete extraction, with operation,
entity and asset diagnostic context. Hidden instances skip bounds and records.
Visible-flag instances validate geometry/transforms before optional culling;
out-of-range content does not silently disappear merely because it is offscreen.
`frustum_culling=false` keeps all valid visible-flag instances for diagnostics.
No per-entity partial success is returned.

17a appends an optional `RenderViewCamera` through `view_camera` in extraction
options. It contains Camera values and a full scene-local world matrix for a
display-only view. The explicit selected Camera handle and expected revision
remain required; `camera_entity()` retains that source identity even when the
matrices use the override. Validate the override with `make_render_camera` before
culling so the snapshot contains geometry visible from the actual requested
view. Omitted overrides preserve existing behavior. No component, source or
prepared format changes; the original Scene is never mutated. See
[ADR 0042](architecture/0042-viewport-navigation-and-picking.md).

17c appends optional `transform_preview` with a checked entity handle and local
TRS. Extraction recomposes its entire subtree from bounded owned hierarchy values
before camera/bounds/normals/culling, without a Scene write. The returned snapshot
retains original identities/revision and exposes `transform_preview()` provenance.
An explicit display camera still takes precedence over affected authored camera
transforms. Omitted previews retain the streaming committed extraction path.
See [gizmos and previews](viewport-gizmos.md) for Tools validation and UI adoption.

## Coordinates, camera and raster conventions

`RenderOrigin.world_anchor`, when present, is the canonical global WorldPosition
of scene-local zero. It is validated and retained verbatim. Its integer cells and
offset are never added to a matrix. `scene_local_offset` is a separate finite
double vector subtracted from camera/model translations. `generation` is nonzero
and caller-owned; the caller must change it when the origin changes and compare
it along with scene incarnation/revision when retaining work. No rebase manager,
global mapping or supported world extent is claimed by this value service.

`make_render_camera` is independently usable with a validated Camera, full
scene-local world matrix, viewport and origin. The view is the checked inverse of
the complete relative matrix. Nonuniform scale, hierarchy shear and negative
scale retain their mathematical effect; choosing a rigid camera is a content
decision. Both view and model use the same relative origin, so moving that origin
does not change their relative view within the admitted numerical range.

Matrices use double precision, column vectors and column-major storage:
`clip = projection * view * world * position`. Cameras face -Z. Perspective uses
vertical FOV and viewport aspect; orthographic uses centered height and aspect.
Near/far map to depth 0/1. Projection negates Y exactly once; later Vulkan uses a
positive-height viewport, depth clear 1 and LESS_OR_EQUAL. Base authored winding
is CCW; reverse raster front-face when model and camera reflection flags differ.
Normal matrices have an inverse-transpose upper 3x3 and identity affine remainder.
Model reflection also reverses tangent handedness. Double-sided materials retain
their existing value for the later draw owner.

Nonfinite or numerically unusable projection/inverse/frustum results reject.
In particular, a perspective far/near ratio that collapses the finite depth
coefficient to the infinite-far value is unsupported. Component validity alone
does not guarantee a usable viewport/projection. GPU float packing and device
extent capabilities remain separate checks in 13d.

## Bounds, visibility and limits

Bounds include all vertices of every primitive, including unused vertices, so
they conservatively contain the mesh. A per-call cache computes them once per
distinct `(AssetId, mesh_index)` with a true visibility flag, including instances
later culled. Full affine corner transformation retains shear and mirrors;
outward roundoff allowance prevents inward-rounded bounds. Planes are normalized
and inward-facing, ordered left/right/clip-minimum-Y/clip-maximum-Y/near/far (the Y flip makes
these camera-space top/bottom respectively). A support-point test
rejects only wholly outside AABBs, retaining contact and roundoff uncertainty.
Each plane carries an absolute signed-distance uncertainty over the bounded
coordinate cube, including cancellation while constructing its normalized
coefficients and admitted near/far reconstruction drift. Culling adds that bound
to its fixed 64-double-epsilon evaluation allowance; cancellation cannot silently
turn a boundary contact into a rejection.
False positives near corners are expected; actual triangle clipping remains GPU
work. No occlusion, LOD, streaming, animation or transparent sorting is added.

| Limit | Contract |
| --- | --- |
| Live entities / output mesh records | 4096 each; reject excess without partial publication |
| Scanned query slots | 65536 including holes; extraction pages use 256 rows / 256 scanned slots, charged before continuation; camera inspection addresses one additional slot directly |
| Distinct-mesh vertex work | 1,048,576 vertices; charge a complete mesh before traversal |
| Viewport | Each extent 1..16384; zero/minimized views require the caller to skip extraction |
| Local positions / relative translations / transformed AABBs | Each component within +/-65536 meters |

Bounds padding can reject a transformed corner exactly at the range limit;
leave numerical margin in content. The local position bound applies before model
scaling. Camera-relative translations and bounds use the same limit. Matrices
and normalized planes must remain finite and inverses satisfy the existing
[transform contract](transforms.md); matrix coefficients are not stored as floats.
Limits bound one extraction, not process-wide memory retained by caller snapshots.
The statistics distinguish entities, mesh instances, hidden/culled/visible
instances, unique bounded meshes and visited vertices. They do not measure GPU
draw count, frame time or MMORPG capacity.

This public example uses Core headers only and does not transfer Scene ownership:

```cpp
#include <shark/scene_render.hpp>

shark::Result<shark::RenderScene>
extract_view(const shark::Scene& scene, const shark::PreparedAssets& assets,
             shark::EntityHandle camera, shark::SceneRevision revision,
             shark::RenderOrigin origin) {
    shark::RenderExtractionOptions options{camera, revision};
    options.viewport = {1280, 720};
    options.origin = std::move(origin);
    return shark::extract_render_scene(scene, assets, options);
}
```

Check the Result before accessing records. Never retain spans after their owning
RenderScene is destroyed. Callers may transfer owned snapshots across threads
with normal synchronization; extracting still requires the Scene owner thread.
