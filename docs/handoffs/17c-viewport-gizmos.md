# 17c: viewport gizmos and subtree previews

Date: 2026-10-29. Base: `93a7128` (`feat(authoring): add transform manipulation
and drag transactions`), initially clean `main`. Staging, commits and pushing
remain the user's work. Desktop interaction remains paused; full REQ-16 and
REQ-17 remain NOT_RUN. **Implementation and selected headless verification are complete.**

## Delivered behavior

[ADR 0044](../architecture/0044-viewport-gizmos.md) and the
[gizmo contract](../viewport-gizmos.md) define focused native axis/ring controls
through existing ImGui. Local/world translation, rotation, signed scale and
snapping feed 17b's shared TransformDrag. Pure geometry/gesture helpers use the
actual image rectangle, affine parents and bounded ray math. No new third-party
dependency, source format, RPC or semantic command is introduced.

Typed optional transform previews extend Core extraction and the retained Tools
viewport request. Core recomposes the complete selected subtree from owned local
TRS before camera, normals, bounds, winding and culling; hidden/nonrendered
descendants are validated. Snapshot provenance marks display-only values while
preserving committed revision/entity identities. Ordinary picking rejects these
snapshots. Default committed extraction retains its original path and budgets.

Tools uses the shared authoring preview and source/asset fences. The widget asks
the existing Client packing path to validate each candidate before commit,
including release with an unchanged pointer. Cancellation restores the retained
committed request by move before submission. One successful release is one retained
set_transform/history group. Its current validated geometry remains for that
frame, and the next frame captures the committed revision. Navigation survives
known same-incarnation transform commits; unrelated edits retain existing view
invalidation. There is no temporary live Scene write or new render owner.

Gestures own viewport input ahead of picking/navigation and cancel on focus,
text/modal, image/layout, selection/revision, tool/settings and close changes.
Close cancels transient drags before the existing dirty/draft dialogs. Unapplied
inspector/asset drafts and ambiguous mutation reconciliation remain explicit.
Guide handles stay at the captured pose during a gesture; geometry previews move.
Single-axis handles are implemented; multi-selection and plane/screen handles are
outside this initial tool. Actual UI/GPU behavior remains unqualified while paused.

## Selected verification

Under the [testing policy](../testing-policy.md), verification covers
changed Core extraction, retained Tools viewport, pure gizmos, drag and navigation,
headless ImGui, Client float preparation and Editor CLI in Windows Debug. Select
portable Core/Tools/gizmo/navigation/drag in retained WSL Debug and ASan+UBSan.
Native window/GPU checks are deferred, not replaced by synthetic input.

| Selection | Result / evidence under `out/17c-work/` |
| --- | --- |
| Windows affected targets and Editor | PASS; `windows-build.log`, `windows-build-ui-final.log`, `windows-gui-fix-build.log`. No Editor window launched. |
| Windows final affected selection | 8/8 PASS using seven unchanged passing results in `windows-tests.log/xml` and the corrected GUI family in `windows-gui-fix.log/xml` (0.91 s). Inventory: `windows-inventory.log`. |
| WSL Debug portable selection | 5/5 PASS, 0.66 s; `wsl/linux-debug-{build,inventory,tests}.log`, `wsl/linux-debug-tests.xml`. |
| WSL ASan+UBSan portable selection | 5/5 PASS, 2.22 s; matching `wsl/linux-asan-ubsan-*` logs/XML. |
| Repository, documentation/manifest, pinned formatting and diff | PASS; `repository.log`, `contract.log`, `format-check.log`; diff check clean. |
| Native gizmo/input/layout/focus and both-GPU rendering | NOT_RUN; explicit desktop pause. |

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset windows-debug --target SceneRenderTests AuthoringViewportTests EditorGizmoTests EditorViewportNavigationTests EditorTransformDragTests EditorGuiTests Editor Tests --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R "^(shark\.core\.scene-render|shark\.tools\.authoring-viewport|shark\.client\.scene-viewport|shark\.editor\.(gizmo|transform-drag|viewport-navigation|gui-adapter|cli))$" --output-on-failure --no-tests=error
```

Inventories were listed before selection. The first Windows run took 2.39 s and
passed seven families; the GUI family failed because the new test harness emitted
a scene texture without supplying its required owned viewport to GUI capture.
A diagnostic run confirmed `graphics.gui.validate` rejected that omission.
The harness now packs its actual RenderScene with the existing CPU shader fixture
and supplies the owned viewport, preserving the validation contract. The final
GUI-only run passed all 14 cases. Original and diagnostic failure logs/XML remain
in `windows-tests.*` and `windows-gui-diagnostic.*`. Only that test file changed
after the seven passing results; unrelated checks were not repeated.

WSL uses the retained namespace/chroot wrapper and canonical Debug then
ASan+UBSan builds sequentially with `--parallel 2`. Targets are SceneRenderTests,
AuthoringViewportTests, EditorGizmoTests, EditorViewportNavigationTests and
EditorTransformDragTests; selection is
`^(shark\.core\.scene-render|shark\.tools\.authoring-viewport|shark\.editor\.(gizmo|viewport-navigation|transform-drag))$`.
These portable inputs stayed unchanged after the passing runs. No host/WSL
resource or kernel settings changed; no native graphics fixture was launched.

Five new pure gizmo cases cover independent projection/axis/affine-ring math,
cumulative snapping, signed scale, angle unwrapping and rejection. Four headless
ImGui cases use actual image bounds, nonuniform framebuffer scale and the retained
project owner, including real typed preview extraction, one-step undo, rendering
rejection, selection/camera/layout/focus cancellation and press/release edge handling.
Three Core cases and one Tools case cover subtree recomposition, newly visible
descendants, reflection/normals/origin/camera behavior, hidden/nonrendered failures
and preview purity/freshness. A picking case rejects marked snapshots. Existing
cases, seeds and limits remain; the new gizmo family uses seed 1337 and 60 seconds.

Static review caught fresh-entry and between-frame release/repress edge cases
before final verification; both now have meaningful regression coverage. Error
status uses fixed height so clearing a diagnostic cannot move the viewport and
invalidate a new gesture. No tolerance, workload or baseline was relaxed.
Unchanged 17a/17b and 16e results remain historical evidence for their original
owners. No broad Server/storage/network/package matrix or second sanitizer
configuration was selected. The protected dependency graph and shader inputs are
unchanged; additions are engine-owned Core values and editor-only source files.

## Coverage, limits and next work

Coverage adds `editor.viewport-gizmos` mapped to the actual CPU/headless families.
Existing FeatureWorld AuthoringCommands still owns set_transform semantics; no
duplicate case/command or Lua stub is introduced. Public options are appended
with defaults, so existing source callers retain committed rendering behavior;
C++ consumers rebuild with the engine. No documentation C++ examples changed.

Next is **17d: connected acceptance**. Continue headless implementation/preparation
under the desktop pause, reusing valid focused results. Connect equivalent human
and terminal transforms, stale intents, save/reopen and undo; actual navigation,
picking, gizmo/focus/layout behavior and required rendering on both GPUs remain
in the [UI backlog](../backlog.md). Do not close full REQ-17 with CPU evidence alone.

`.gitignore` was reviewed: existing build/out/cache/local-roadmap patterns cover
all generated outputs, with no new dependency cache or ignore pattern needed.
No source was staged, committed or pushed.

Suggested commit: `feat(editor): integrate transform gizmos and subtree previews`.
