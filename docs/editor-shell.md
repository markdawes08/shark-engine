# Human editor shell

Assignment 16a adds an explicit Windows `Editor --editor` application path.
The [shell handoff](handoffs/16a-editor-shell.md) records its verification; the
[checkpoint plan](roadmap/editor-plan.md) retains the remaining UI work.
Full REQ-16 is NOT_RUN. The initial shell opens, saves and closes existing
projects. [16b's scene workspace](editor-scene-workspace.md) now adds hierarchy,
selection, entity edits and a scene viewport. The [inspector/history](editor-inspector.md)
and [asset/console panels](editor-assets-console.md) are implemented; connected
actual UI acceptance follows the [16e procedure](editor-acceptance.md).
[17a navigation/picking](viewport-navigation.md) extends the viewport; transform
drag transactions and gizmos remain later assignment-17 checkpoints.

Desktop testing is currently paused by user instruction. The launch instructions
below remain available for later use; agents must follow the
[coding-only amendment](backlog.md) and defer windowed checks until explicitly
resumed. Builds and headless checks can continue.

## Launch and use

Build `Editor` with the normal native Windows Debug preset. Startup requires
precompiled GUI and scene stages; it never compiles shaders. After the existing
locked DXC and validation tools are prepared, build both generations explicitly:

```powershell
.tools/venv/Scripts/python.exe tools/shader_pipeline.py build --recipe shaders/gui_recipe.json --output out/shaders/editor --validation-manifest out/16a-work/validation-tools/summary.json
.tools/venv/Scripts/python.exe tools/shader_pipeline.py build --recipe shaders/scene_recipe.json --output out/shaders/editor-scene --validation-manifest out/16a-work/validation-tools/summary.json
$guiManifest = Get-Content out/shaders/editor/shader-manifest.json -Raw | ConvertFrom-Json
$guiVertex = Join-Path out/shaders/editor ($guiManifest.shaders | Where-Object id -eq gui-vs).file
$guiPixel = Join-Path out/shaders/editor ($guiManifest.shaders | Where-Object id -eq gui-ps).file
$sceneManifest = Get-Content out/shaders/editor-scene/shader-manifest.json -Raw | ConvertFrom-Json
$sceneVertex = Join-Path out/shaders/editor-scene ($sceneManifest.shaders | Where-Object id -eq scene-unlit-vs).file
$scenePixel = Join-Path out/shaders/editor-scene ($sceneManifest.shaders | Where-Object id -eq scene-unlit-ps).file
# Launch only after desktop testing is explicitly resumed.
build/windows-debug/bin/Editor.exe --editor --gui-vertex $guiVertex --gui-pixel $guiPixel --scene-vertex $sceneVertex --scene-pixel $scenePixel
```

The receipt path identifies the retained current-lock validation tools on this
machine; use the actual fresh receipt if preparing elsewhere. The
[shader guide](shader-pipeline.md) describes tool preparation and integrity checks.
The older `prepare` command exports the cube bundle and is not the GUI installer.
Without explicit stage paths, the Editor reads `assets/gui-vs.spv` and
`assets/gui-ps.spv`, plus `assets/scene-unlit-vs.spv` and `assets/scene-unlit-ps.spv`, beside
its executable; supply verified pairs there first. Each explicit shader option
requires its matching stage option.

Enter an existing project folder and a project path relative to it, then choose
**Open project**. Optional startup `--root PATH --project RELATIVE` opens it
directly. A project produced by the [terminal example](authoring-protocol.md#complete-terminal-example)
uses folder `out/terminal-example/project` and file `Terminal.sharkproject`.
The terminal owner must close before the Editor can open it. The UI displays
project identity, scene revision, saved/unsaved state and operation errors.

**Save scene** publishes through the shared owner. **Close project** releases
ownership; **Exit** and the window close button use the same close decision.
Dirty state opens **Save and close / Discard changes / Cancel**. Failed save or
close leaves the project open and shows the error. Scene edits use the
[shared workspace owner](editor-scene-workspace.md); a process crash is not an
implicit save.

Ctrl+O focuses project opening, Ctrl+S saves and Ctrl+W closes the project.
Shortcuts require focus and a fresh key press, and are suppressed while editing
text or resolving a modal. Mouse, wheel, Unicode text and keyboard navigation
use the shared input snapshot. Focus loss clears queued/held UI input. Current
font coverage follows the embedded default font; text storage remains UTF-8.
Complete modifier chords within a snapshot retain their press around button
events before restoring final held state. Snapshots coalesce transitions; they
do not claim a total ordering of simultaneous independent inputs.

`--device UUID|auto` selects the adapter. Interactive startup defaults to
`--validation off`; qualifying GPU fixtures require the pinned validation layer
and `required` mode under the existing normal-user environment. `--help`,
`--version`, `--window`, `--graphics` and `--cube` retain their earlier roles.
CPU/WSL bootstrap builds reject `--editor` as unavailable and do not fetch ImGui.

## Shared ownership and outcomes

The Editor-private [ProjectSession](../editor/project_session.hpp) dispatches
the existing bounded [authoring protocol](authoring-protocol.md) in process.
There is one creating-thread owner, one cooperating project writer and no
parallel terminal attachment. Opening another project requires explicit close.
The UI's cached identity/revision/dirty values are observations, not an editable
copy of the authoritative scene.

Save carries the expected revision and retains whether publication happened,
which revision was saved, whether current state became clean, byte count and
durability. `directory_flush_failed` means publication happened with an explicit
durability limit. Close-after-save retains that receipt even if close later fails.
Discard-close requires the currently observed revision; cancel preserves the
owner. Out-of-band changes and stale decisions reject rather than silently
overwriting or retrying. Save preserves history; close/reopen obtains a fresh
incarnation and history scope. Panels must inspect all retained mutation
result levels, and use explicit retry identities rather than request correlation.

## GUI presentation boundary

[GuiAtlas and GuiFrame](../engine/client/include/shark/client/gui_draw.hpp)
are owned Client values. Atlas dimensions are 1..2048 with exact RGBA8 storage;
framebuffer dimensions are 1..4096. Frames allow at most 131,072 vertices,
393,216 indices and 4,096 draws. Triangle ranges, indices, finite positions,
normalized UVs/colors and exclusive right/bottom clip rectangles are validated.
Empty frames are allowed. The Editor accepts its font atlas and the engine-owned
scene viewport texture choice, rejecting callbacks or unrecognized textures.
An optional SceneViewportRequest in the same owned frame supplies that image;
all nested viewport limits apply. See the [composition contract](editor-scene-workspace.md#gpu-composition-and-lifetime).

Configure one immutable atlas before first presentation. Frame submission copies
bounded CPU values; the backend's per-image GPU buffers are updated only through
the existing completion/lifetime path. Resize keeps the atlas and replaces
presentation resources through the shared bounded drain. Standalone GUI, cube
and scene presentation modes are exclusive; an embedded scene image is part of
GUI mode. The GUI shader profile is separate from the
cube and scene profiles. Linear RGBA is alpha blended with depth writes disabled;
the actual sRGB attachment can be captured through the existing readback API.

Native close cancellation changes the native close flag before another event/
presentation pass. The shell resolves project closure before graphics teardown.
Device failures preserve the established diagnostic and bounded cleanup rules.
This does not introduce another renderer, scheduler, gameplay loop or server
authority path. See [ADR 0040](architecture/0040-human-editor-shell.md).
