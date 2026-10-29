# ADR 0041: Retained scene workspace and GUI viewport composition

Date: 2026-10-29. Status: accepted design; qualification belongs to the
[16b handoff](../handoffs/16b-editor-scene-workspace.md). Owner: assignment 16.

Extend the [editor shell](0040-human-editor-shell.md) with an Editor-private
ScenePanel over the same ProjectSession. Hierarchy rows are owned query values,
ordered parent first and paged locally after a bounded complete query. They are
not another mutable Scene. Selection carries the observed SceneRevision; an
external revision change clears it rather than rebinding a recreated entity by
durable ID. Create, delete and reparent use existing retained commands, inspect
all outcome levels and preserve the original mutation identity for reconciliation.
A completed mutation with a failed display refresh remains a completed mutation.

Add a typed CPU-only `AuthoringProtocol::viewport_snapshot` read. The caller
selects a Camera, revision, render extent, local origin and client asset target.
The retained owner supplies fresh immutable asset closures and extracts its
original scene identity, including unsaved edits. Reuse existing asset preparation
and final validation without creating a second authored Scene, exposing mutable
handles or writing source. Imported content requires explicit catalog inventory;
this read never walks files or starts imports. CPU Tools retains its Client and
GUI exclusion. This is an in-process API addition, not a new terminal RPC method
or protocol/source format version.

Extend the engine-owned GuiFrame with an optional SceneViewportRequest and a
closed GuiTexture choice of atlas or scene viewport. No ImGui texture handle or
native resource crosses Client's public boundary. The existing scene shader
profile renders into a linear RGBA8 target with shared depth before GUI draws
sample it in the same submission. The sRGB presentation attachment encodes RGB
once; no CPU readback/upload bridge or second graphics device is introduced.

Each presentation image retains its scene target and frame resources. Complete
candidates replace changed content or extent only after that image's submitted
use completes. The cache compares all supplied scene, camera, geometry and shader
values; SceneRevision alone cannot identify a camera or viewport change. Failed
recording/submission retains adopted owners through the existing bounded cleanup
path. Removing/readding the scene image preserves the configured GUI atlas.

Navigation, picking and gizmos remain assignment 17. Inspector/history and
asset/job/console panels retain 16c-16d; connected human/terminal acceptance is
16e. The user's desktop pause defers native composition and actual UI acceptance,
without qualifying this behavior or closing full REQ-16. See the
[workspace contract](../editor-scene-workspace.md) and [backlog](../backlog.md).
