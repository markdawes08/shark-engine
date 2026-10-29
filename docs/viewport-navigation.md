# Editor viewport navigation and picking

17a extends the [scene workspace](editor-scene-workspace.md) with a display-only
view and triangle selection. Follow [ADR 0042](architecture/0042-viewport-navigation-and-picking.md)
and the [four checkpoints](roadmap/viewport-interaction-plan.md). The
[handoff](handoffs/17a-viewport-navigation.md) distinguishes headless results from
actual UI/GPU acceptance, which remains NOT_RUN under the desktop pause.

## View ownership and controls

Select an authored Camera and choose **View through camera** to seed the view.
That Camera remains required; a camera-less editor view is outside this increment.
The editor stores temporary Camera values, a full scene-local affine pose and an
orbit pivot. Navigation does not write a component, change the scene revision or
dirty state, create history, save source or send a gameplay intent.

Inside the displayed image, right-drag orbits, middle-drag pans and the wheel
dollies. Left-click picks a rendered entity. **Reset view** returns to the authored
camera. The controls use logical image coordinates and explicit deltas, not a
second frame clock, global mouse hook, native cursor capture or cursor warping.
Both perspective and orthographic cameras are supported. Navigation preserves
the seed's scale, reflection and hierarchy shear rather than replacing it with
an approximate rigid camera. Invalid or out-of-bounds motion rejects as a complete
candidate, retaining the last valid view.

Drag components are bounded to 4096 logical pixels per frame and dolly to 32
fractional wheel steps. Orbit uses 0.005 radians per pixel; dolly scales by
`exp(-0.1 * steps)`. The initial pivot distance is one quarter of the Camera's
clip span clamped to 0.01..10 meters; perspective dolly keeps it within
0.01..65536 meters. Orthographic dolly changes only the temporary projection
height. Existing camera, affine inverse and bounded-origin validation still
decide whether the complete candidate is usable.

An optional `RenderViewCamera` flows through the existing typed authoring viewport
request into Core extraction. Its projection, pose and origin are validated before
frustum culling. This lets navigation reveal geometry outside the seed camera's
old frustum. The returned RenderScene keeps original entity references, exact
revision, bounded local origin and independent global world anchor, plus fresh
immutable asset leases. Import/catalog/source fences remain unchanged. There is
no new RPC or source/prepared format; omitted overrides retain existing behavior.

The workspace retains that RenderScene beside its packed GPU request. A camera,
content, target, extent or scene-revision change invalidates the appropriate view
and active gesture. New navigation is prepared through the shared owner; a click
uses the snapshot for the displayed image, never a newly moved camera that has
not yet supplied that image. Failed preparation does not silently reuse a stale
hit or authorize a scene edit.

## Input and coordinate boundaries

The actual ImGui image rectangle determines pointer mapping. Its origin includes
panel offsets; its logical size reflects framebuffer scaling and the 2048-pixel
viewport clamp. The containing panel's available size is not the image size.
Right and bottom edges are exclusive; outside points are misses. Nonfinite or
invalid rectangles/coordinates reject. Projection already flips Y once: image
top-left maps to NDC `(-1,-1)`, with Vulkan depth endpoints `0` and `1`.

Gestures start with a fresh press over the image. Native focus loss, minimize,
text/modal capture, blocked state, leaving the image, tab/image removal, changed
layout or revision, and Escape cancel the active gesture. A held button cannot
resume a cancelled drag without another press. Image interaction does not consume
text widgets or bypass existing project-close decisions. Picking preserves an
unapplied inspector draft; explicit reload/discard remains the user's decision.

## Picking and bounded work

Picking unprojects a near-to-far segment through the snapshot camera, then tests
the actual indexed triangles and material face policy under each record's full
world transform. AABBs alone cannot select a hole or empty space inside a mesh.
Hidden and frustum-culled instances are absent from the rendered snapshot.
Single-sided winding follows the renderer's reflection correction; double-sided
geometry accepts either face. The nearest clipped intersection wins; equal-depth
intersections select the later record/primitive/triangle, matching the viewport's
LESS_OR_EQUAL ordering. Picking is geometric; it does not certify exact raster
coverage at subpixel triangle boundaries or add transparent/alpha-mask sampling.

Charge the complete instance triangle count before traversal, up to 1,048,576
triangles. Excess work rejects without returning a partial nearest hit. The
existing immutable asset, entity, bounds, origin and float-packing limits remain
in force. The returned authored ID and SceneRevision are observations. The
workspace requires a current ScenePanel and the same exact revision before
selection; a stale hit cannot rebind an entity recreated under a durable ID.
Empty-space clicks clear selection. Selection/navigation do not mutate history.

Camera icons, bounds-only selection of nonrendered entities, occlusion buffers,
spatial acceleration, gizmos and continuous-drag authoring transactions are not
added here. Nonrendered entities remain selectable through the hierarchy; shared
transform/drag behavior is defined by [17b](transform-manipulation.md) and
[17c gizmos](viewport-gizmos.md) consume pointer input before navigation/picking.
Picking rejects preview-marked snapshots even at the committed revision.
Native input/layout and both-GPU
navigation/picking remain in the [UI backlog](backlog.md#desktop-ui-deferral).
