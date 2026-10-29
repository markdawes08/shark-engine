# ADR 0042: Display-only navigation and snapshot picking

Date: 2026-10-29. Status: accepted; qualification belongs to
[17a](../handoffs/17a-viewport-navigation.md). Owner: assignment 17.

The editor retains a display camera and one owned RenderScene for its viewport.
Navigation never edits an authored Camera/Transform, saves files, creates an undo
entry or changes server state. An explicitly selected authored Camera seeds the
view and remains its provenance; this increment does not add a camera-less view.

Append an optional `RenderViewCamera` (Camera values plus full scene-local affine
world matrix) to Core extraction options and the typed Tools viewport request.
Default callers retain authored-camera behavior. Validate the override using the
existing camera/matrix/origin rules and use it before frustum culling. Replacing
only a GPU projection after extraction would omit newly revealed geometry and is
not valid. Retain the original selected entity, exact SceneRevision, world anchor,
local origin generation and fresh immutable asset closure in the snapshot.
No source/prepared/protocol format or terminal method changes.

Navigation uses bounded explicit input deltas and complete candidate adoption.
Orbit/pan/dolly retain the seed's affine camera behavior; they do not decompose
away scale, hierarchy shear or reflection. Invalid motion preserves the last
valid view. There is no second renderer, scene, worker or asset importer.

Picking intersects indexed triangles in that same immutable snapshot, with the
same camera matrices, near/far clip segment, visibility and material face policy.
Actual image bounds determine pointer coordinates; available panel size and raw
framebuffer coordinates cannot stand in for a scaled or clamped image. Charge
bounded triangle work before reporting any hit. Select through ScenePanel only
when its current revision still matches the observed snapshot; old hits do not
authorize rebinding a durable ID in a newer incarnation.

Input begins over the image and is gated by native focus, text/modal ownership,
blocked state and image lifetime. Cancellation prevents a held button from
resuming a prior gesture after focus, layout, tab or revision changes. Existing
inspector drafts remain explicit values and are not discarded by picking.

The [contract](../viewport-navigation.md) records controls, limits and omission of
gizmos/drag transactions until 17b-17c. CPU checks qualify the math and adapters;
actual UI/GPU checks remain in the [backlog](../backlog.md). The desktop pause
does not establish full REQ-16 or REQ-17 acceptance.
