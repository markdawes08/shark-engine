# ADR 0034: Bounded CPU scene extraction

Status: accepted. Date: 2026-10-28. Owner: 13c, partial REQ-13.

The existing Scene owns transforms/components and PreparedAssets owns immutable
mesh data. Runtime and headless Tools need the same camera, bounds and visibility
results before Client submits graphics work. Add a pure CPU extraction service to
EngineCore, using these owners and existing revisioned query pages. Do not create
another registry, asset manager, scheduler or renderer backend.

`extract_render_scene` requires an explicit camera handle, exact SceneRevision,
viewport and render origin. It reads committed state on the Scene owner thread;
pending writes remain pending. It returns owned values and shared const asset
leases, with no Scene pointer. Failures publish no partial result and mutate no
owner. Resolve every mesh reference again, including hidden entities, because
normal Core gameplay can change components after prepared scene loading.

Preserve full affine hierarchy matrices, including shear and reflections. Camera
view is the inverse of its complete origin-relative world matrix; camera scale
and shear affect the view rather than being silently removed. Projection follows
[ADR 0005](0005-simulation-and-rendering.md): finite right-handed perspective or
orthographic, -Z forward, depth [0,1], one projection-Y flip for a positive-height
viewport. An odd camera reflection reverses raster parity in addition to an odd
model reflection. Normal matrices use inverse-transpose linear transforms.

Derive mesh AABBs from immutable vertex positions once per distinct visible-flag
mesh in each extraction. Transform all eight corners conservatively, then reject
only boxes entirely outside an inward frustum plane. Touching/uncertain boxes
stay visible; this is conservative whole-mesh culling, not triangle clipping or
occlusion. Invalid visible geometry rejects even when outside the frustum.

The optional global WorldPosition identifies scene-local zero and is retained
verbatim, separately from double local matrices. Subtract an explicit finite
scene-local render offset; bound local mesh positions, render translations and
transformed bounds to +/-65536 meters. Retain the caller's nonzero origin
generation alongside scene incarnation/revision. No global-cell-to-float
conversion, implicit origin selection, cross-cell mapping or rebase occurs.
M05 owns that larger lifecycle. Matrix values remain double here; 13d owns checked
GPU packing and actual device/resource behavior.

The [public contract](../scene-rendering.md) fixes work/output limits and
numerical rejection. Existing source/prepared formats and component schemas do
not change. Pure math/extraction is available to CPU Tools and Server without
presentation dependencies; only EngineClient will draw. FeatureWorld exercises
the public service through a separate loaded owner. Terminal/editor adapters,
offscreen draws and connected Runtime launch retain their planned owners.
