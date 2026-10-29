# Editor scene workspace

Assignment 16b extends the [shell](editor-shell.md) with hierarchy, selection,
entity creation/deletion/reparenting and an authored-scene viewport. The
[handoff](handoffs/16b-editor-scene-workspace.md) records verification;
[ADR 0041](architecture/0041-editor-scene-workspace.md) defines the ownership
decision. Desktop testing remains paused: the native fixture and actual UI checks
are deferred/NOT_RUN in [BL-16A-UI-01](backlog.md). Full REQ-16 stays NOT_RUN.

## Hierarchy and shared edits

The Editor-private ScenePanel borrows ProjectSession on its creating thread.
It queries the original retained owner for name, hierarchy, Camera and
MeshRenderer observations. A complete candidate is bounded to 4,096 rows,
64 query pages and 65,536 scanned slots, with at most 4,096 slots scanned per
request. The wire query uses up to 256 rows per page; display pages contain
64 rows after parent-first ordering with stable sibling order. No continuation
cursor is held merely to display a local page. Incomplete or inconsistent
hierarchies reject without publishing a partial tree.

Selection stores a durable entity ID plus the exact observed SceneRevision.
External revision changes invalidate selection, including delete/undo recreation;
the native entity generation remains private. A confirmed successful create or
reparent may reselect its known surviving entity at the returned revision.
The viewport camera is separately explicit and is cleared on a revision change.
No camera is chosen implicitly, and stale UI drafts must be refreshed and
confirmed again before they can mutate the owner.

Create, delete and reparent dispatch existing `scene.mutate` commands with a
fresh retained mutation identity and the expected revision. Reparent offers the
existing keep-world/keep-local behavior; shared validation rejects cycles and
invalid transforms. All three outcome levels are checked. An uncertain reply
blocks further edits until lookup reconciles the original identity; request
correlation never creates a replacement mutation. A successful mutation with a
failed hierarchy refresh returns its success and a separate refresh error.
Save, dirty close decisions and history continue through the original shell
session. This increment adds no component schemas or alternate persistence path.
[16c's inspector](editor-inspector.md) extends this same retained path with
component drafts and shared undo/redo.

## Read-only viewport preparation

`AuthoringProtocol::viewport_snapshot(AuthoringViewportRequest)` is a typed
CPU-only read, forwarded by ProjectSession. It requires an explicit Camera ID,
expected revision, 1..2048 render dimensions, bounded local origin with a nonzero
generation, and client asset target. It returns an owned RenderScene extracted
from the original retained Scene, including unsaved edits and original scene
identity. The source world anchor remains distinct from the local render origin.

Fresh imported mesh closures use the existing retained asset owner and final
candidate validation. The caller explicitly supplies metadata sidecars to the
existing `asset.catalog.refresh` method before requesting imported content.
In the UI, **Import target** defaults to `windows-client`; **Load asset inventory**
accepts a newline-separated complete list of metadata sidecar paths. This is an
inventory selection, not a filesystem scan or import. Missing, stale, unknown or
wrong-target assets produce useful errors rather than silently using old output.
The viewport read does not save, import, mutate history or clear dirty state.

Select a hierarchy entity with Camera, then explicitly use it for the viewport.
Extent changes request a new projection at the current revision. The existing
unlit scene pipeline supplies the displayed geometry. 17a adds
[display-only navigation and snapshot picking](viewport-navigation.md): the
typed request's optional `view_camera` supplies a validated affine display view
before culling, while the selected authored Camera remains provenance.
[17c gizmos](viewport-gizmos.md) add validated subtree previews and one-commit
transform gestures; cancellation restores committed rendering without Scene writes.
Material inspection and gameplay execution remain with their later owners.

## GPU composition and lifetime

GuiDraw selects `GuiTexture::atlas` (the compatible default) or
`GuiTexture::scene_viewport`. GuiFrame optionally owns a SceneViewportRequest;
a scene-textured draw without that request is rejected. Existing GUI limits and
all nested viewport limits apply. Editor maps its two internal texture IDs to
these choices, rejecting callbacks and unknown textures. Public values expose no
native handles or ImGui types.

The existing GraphicsDevice renders the scene into a retained linear RGBA8
texture with shared depth, transitions it for sampling, then composites the GUI
before presentation. Per-image targets persist while their extent is unchanged.
Changed geometry, camera, shaders or content invalidate the exact-value cache
even at the same SceneRevision. Complete resource candidates replace submitted
owners only after bounded completion; failed recording or ambiguous submission
retains adopted owners until safe cleanup. Removal/readdition and framebuffer
resize use the existing retirement path while keeping the GUI atlas.

There is no CPU image transfer or second device. Scene RGB remains linear until
the sRGB presentation attachment encodes it once. The scene texture samples with
alpha one, preserving the existing opaque scene profile even when source vertex
or material alpha is below one. GUI overlays retain their own alpha coverage.
Standalone cube, scene and GUI
presentation modes retain their existing exclusivity; the scene image is part of
the GUI mode. Runtime gains no authoring or importer dependency.

## Deferred native qualification

The existing `tools/gui_smoke.py` default mode retains 16a's four phases.
Optional `--scene-shaders PATH` selects the verified scene shader bundle and
enables the 16b fixture; GuiRenderSmoke receives paired `--scene-vertex` and
`--scene-pixel` paths. Its eight phases cover baseline, camera change at the same
revision, content change at the same revision, target resize, framebuffer resize,
removal, restoration and cancelled close. Each phase presents through acquired
image reuse before capture. Fixed independent pixels check shared-depth
occlusion, clipping/alpha overlay and linear-to-sRGB conversion; generation and
digest checks cover the changed/restored results.

Both current-PC GPUs still need this required-validation run, followed by actual
hierarchy/Camera/viewport, CRUD, save and dirty-dialog interactions. They must not
be launched until the user explicitly resumes desktop testing. CPU checks and
compilation do not qualify native rendering or actual human/terminal parity.
