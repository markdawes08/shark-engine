# ADR 0044: Focused native gizmos and immutable subtree previews

Date: 2026-10-29. Status: accepted. Owner: assignment 17c.

Use focused editor-native axis and ring handles through the existing ImGui draw
layer, satisfying assignment 17's equivalent-tool option without another dependency.
Separate pure bounded projection/hit/gesture math from ImGui routing and the
existing TransformDrag owner. Local/world modes and snapping use 17b semantics.
Full affine parent axes are retained; unusable projections explicitly reject.
The [gizmo contract](../viewport-gizmos.md) defines controls and deliberate limits.

Append a typed optional transform preview to Core extraction and the retained CPU
Tools viewport request. Tools runs the shared authoring preview. Core recomposes
one observed subtree before camera, bounds, normals and culling, validating all
descendants without mutating the Scene. Mark returned snapshots as previews while
retaining the original revision and identities. Picking cannot treat them as
committed content. Default extraction and source/protocol formats are unchanged;
C++ consumers rebuild with the engine.

Validate and pack the candidate through the existing render path before release
can commit. Cancellation restores the retained committed request before submission;
it never writes a baseline back. One release uses one retained set_transform/history
entry. Existing single-owner, retry and source/asset fences remain authoritative.
Close cancels transient gestures before the normal dirty/draft decisions.

This changes no shader, GPU lifetime, renderer ownership, gameplay protocol or
global cell identity. Headless checks cover CPU preparation and adapter input;
actual focus/layout/controls/rendering on both GPUs remain NOT_RUN until explicit
desktop resumption. 17d owns connected acceptance and full REQ-17 remains open.
