# Viewport gizmos and rendered previews

17c connects the [17b transform services](transform-manipulation.md) to the
existing editor viewport. [ADR 0044](architecture/0044-viewport-gizmos.md) records
the native focused-tool choice. No additional dependency, source/protocol version
or terminal command is introduced. Actual UI/GPU qualification remains deferred
under the user's desktop pause; see the [handoff](handoffs/17c-viewport-gizmos.md).

## Controls and coordinates

Choose Select, Translate, Rotate or Scale above the image, then Local or World.
Select retains ordinary triangle picking. The selected entity's projected X/Y/Z
handles use red/green/blue, with the active handle highlighted. Translation uses
shafts with round tips, scale uses square tips, and rotation uses axis-plane rings.
Handles are clipped to the actual displayed image, including its offsets and
nonuniform framebuffer scaling. No native cursor capture, warping or input
injection is used. Handles overlay geometry; they are not depth-occluded.

Drag with the left button and release inside the image to commit once. Snap is
optional: translation steps are meters, rotation steps display degrees and convert
to radians, and scale steps are factor increments around one. The shared helper
performs snapping and validation. Signed/nonuniform scale is supported; zero,
unresolved bases and unrepresentable local shear reject explicitly. A rejected
pointer candidate cancels the gesture; release cannot commit an older value.

The camera, image rectangle, selected entity/revision, parent matrix, tool, space
and snap settings are frozen for a gesture. Translation and scale use the closest
point on the selected axis to the camera ray. Local axes retain the full affine
parent and starting local rotation, excluding the entity's own scale. The axis
parameter converts back to the shared helper's local units. World axes are XYZ in
the scene's local frame, never global MMO cell coordinates.

Rotation intersects the pointer ray with the chosen axis plane, including affine
ellipses from parent scale/shear, and unwraps successive angles across +/-pi.
Motion between samples follows the shorter arc; more than half a revolution
between samples cannot be inferred. A gesture is bounded to 1024*pi radians.
Guide handles stay at the captured pose while dragging, so cumulative input has
a stable reference; the scene geometry previews the candidate. After commit the
handles use the new pose.

The largest projected handle radius targets 80 logical pixels. Hit tolerance is
7 pixels; the inner 12 pixels around shaft pivots remain ordinary picking space.
Each rotation ring has 64 segments. Clipped, nearly view-aligned axes and edge-on
rings are unavailable; orbit to another angle. Finite coordinate, ray and matrix
checks reject unusable projections instead of inventing an alternate motion.
This initial focused tool supplies single-axis gestures; multi-axis plane handles,
screen-space rotation and multi-selection are not implemented.

## Capture, cancellation and history

Gizmo routing precedes picking and navigation. A handle press, ongoing gesture,
release or cancellation consumes that frame's viewport pointer input. Right/middle
navigation and wheel motion cannot also run during a gizmo drag. A held button
does not restart a cancelled gesture; a fresh inside press is required.

Escape, leaving the image, focus loss, minimize, text/modal ownership, changed
layout/extent/camera/origin, selection/revision changes, tool/space/snap changes,
resource refresh/reset and disappearing tabs/images cancel. Close requests cancel
transient drags before the existing dirty/draft dialogs; they never silently apply
the preview. Committed edits retain the normal save/discard/cancel rules. Inspector
and asset drafts remain explicit and independent.

The widget uses TransformDrag and the same retained ProjectSession throughout.
One changed release is one `set_transform` history group. Failed or ambiguous
commit retains the existing outcome reconciliation obligation. A known successful
transform preserves viewport navigation and explicitly updates the camera's
revision within the same scene incarnation; unrelated scene changes still clear
the old camera observation. The next image/input uses a freshly captured revision.

## Preview extraction and publication

`AuthoringViewportRequest::transform_preview` optionally supplies an authored
entity ID and local Transform. CPU Tools validates it through the existing pure
owner preview and retains the current source/asset fences. Core extraction's
optional `RenderTransformOverride` resolves a checked handle at the exact revision,
collects one bounded hierarchy observation and recomposes the entire selected
subtree parent-first using original local TRS. It validates hidden and nonrendered
descendants too. No mutable Scene, temporary live write or private candidate handle
is exposed.

Recomposition occurs before camera matrices, bounds, normals, reflection and
culling. Content previously outside the frustum can become visible. An affected
authored camera follows the candidate unless the explicit display camera override
is present. Entity identities, global anchor and committed SceneRevision stay
unchanged. `RenderScene::transform_preview()` distinguishes this display value
from a committed snapshot; ordinary viewport picking rejects marked snapshots.
Default extraction retains its original committed-state path and limits.

The widget's owner-thread preview callback completes Tools extraction and existing
Client float packing before accepting a candidate or releasing it to commit.
Release validates again even if the pointer did not move, preserving source/asset
freshness. The viewport retains the original packed request alongside the preview;
cancellation restores it by move before submission. A successful release retains
the validated result for that frame and captures the new committed revision for
the next. The existing GraphicsDevice/shaders own drawing and retirement.

Terminal users reproduce resulting transforms through existing semantic inspection,
`scene.preview`, retained `set_transform`, history and persistence. Overlay/tool
settings are editor-private presentation state. No player or durable-world authority
is added. FeatureWorld's existing command coverage remains; dedicated CPU geometry,
extraction, retained-owner and headless ImGui fixtures cover the new adapter.
