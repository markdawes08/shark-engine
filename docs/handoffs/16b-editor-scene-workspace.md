# 16b: retained hierarchy and scene viewport

Date: 2026-10-29. Base: `9893b47` (`feat(editor): add project shell and defer desktop acceptance`),
initially clean `main`. **16b implementation and selected headless verification
are complete. Desktop acceptance is deferred/NOT_RUN
under the user's coding-only amendment. Full REQ-16 remains NOT_RUN;
`current_assignment` is 16.** The user owns staging, commits and pushing.

## Delivered behavior

The [scene workspace](../editor-scene-workspace.md) extends the existing shell
with bounded parent-first hierarchy pages, revision-scoped selection, create,
delete and keep-world/keep-local reparent commands. ScenePanel consumes the
retained ProjectSession protocol and reconciles original mutation outcomes;
it holds only display values, never another mutable Scene. External revisions
invalidate selection, and viewport camera selection is explicit and revision
bound. Existing save/close and dirty-state decisions now have real scene edits.

A typed CPU `AuthoringProtocol::viewport_snapshot` prepares fresh client asset
closures and extracts the original retained Scene, including unsaved edits.
Imported content requires an explicit metadata inventory through the existing
catalog method; the UI supplies target and sidecar fields without implicit scans
or imports. CPU Tools stays independent of Client. No new terminal RPC method,
component schema, source format or FeatureWorld case is introduced.

Client GuiFrame owns an optional scene viewport and GuiDraw has an engine-owned
texture choice. The existing device renders a linear scene texture then samples
it in GUI presentation, with persistent per-image targets, exact-value camera/
content invalidation and completion-safe candidate replacement. Recording failure
retains adopted owners through existing cleanup. No native handles, CPU image
transfer or second renderer cross the boundary. [ADR 0041](../architecture/0041-editor-scene-workspace.md)
records this API extension; existing GUI atlas draws remain source compatible.

The Editor accepts optional paired `--scene-vertex` / `--scene-pixel` shader
paths, defaulting to `assets/scene-unlit-vs.spv` and `assets/scene-unlit-ps.spv` beside the
executable. The existing GUI shader pair and diagnostic modes remain available.
The native GUI fixture now has an optional eight-phase scene composition mode;
its original 16a mode remains intact.

## Selected verification

Selection follows the [testing policy](../testing-policy.md): affected retained
viewport, Editor hierarchy/session/adapter and Client GUI/viewport behavior,
portable owner checks and relevant boundary/CLI checks. Builds and CPU tests
do not open a window. No Editor or GPU fixture was launched during this work.

| Selection | Result / evidence |
| --- | --- |
| Windows Debug affected targets | PASS; `out/16b-work/build-debug.log`, `build-adapter-tests.log`, `build-final-owners.log`, `build-editor-final.log`; Editor and deferred GuiRenderSmoke compiled without launching |
| Selected Windows regression | 11/11 PASS, 3.92 s; `windows-inventory.log`, `windows-tests.log/xml`. The narrow final-owner rerun after GCC portability fixes is recorded in `windows-final-owners.log/xml`; these repeats are not new tests. |
| WSL GCC Debug | 8/8 PASS, 1.64 s; `wsl/linux-debug-build.log`, `wsl/linux-debug-inventory.log`, `wsl/linux-debug-tests.log/xml` |
| WSL Clang ASan+UBSan | 8/8 PASS, 2.98 s; matching `wsl/linux-asan-ubsan-*` logs/XML. Instruments the retained owner and CPU Client contracts, not ImGui or the Windows GPU backend. |
| Protected Runtime build | PASS; `runtime-boundary.log`. ImGui, Tools and source importers remain excluded. No package/runtime launch was required. |
| Offline scene shader build | PASS; `scene-shaders.log`, verified generation under `scene-shaders/`. Reused the exact 16a validation-tools receipt and unchanged lock; copied the pair beside Debug Editor for later use. No GPU initialization. |
| Repository/link/manifest, formatting and diff checks | PASS; `contract-final.log`, `repository-final.log`, `format-final.log`, `git diff --check` |
| Eight-phase native GUI scene composition, RTX and Intel | NOT_RUN; desktop pause, tracked in BL-16A-UI-01 |
| Actual hierarchy, explicit Camera, viewport, CRUD, save and dirty dialogs | NOT_RUN; desktop pause, tracked in BL-16A-UI-01 |

All paths above are under `out/16b-work/`. The Windows primary selection was:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R "^shark\.(editor\.(scene-panel|project-session|gui-adapter|cli)|tools\.(authoring-viewport|authoring-protocol(-process)?)|client\.(gui-draw|presentation|offscreen|scene-viewport))$" --output-on-failure --no-tests=error
```

The retained `wsl-driver.sh` / `wsl-commands.sh` build five affected targets in
the canonical Debug and ASan+UBSan directories, then select the two new owner
families, project-session, authoring-protocol and four Client CPU families. No
whole-project suite, new dependency population or unrelated Server/package
qualification was added. Source and private GUI changes did not require new
FeatureWorld cases or a terminal RPC version change.

Integration initially exposed a missing closing brace in ScenePanel's reconcile
function (`build-cpu.log`). GCC then identified a temporary reference-wrapper
borrow and an unused test helper (`wsl/linux-debug-first-build.log` and
`linux-debug-second-build.log`). The borrow now has an explicit local lifetime;
the unused helper was removed. Final selected checks pass. Static review also
corrected scene-image alpha so opaque geometry is not dimmed by GUI blending;
the deferred native fixture uses zero-alpha scene color to cover that case.
No native rendering evidence is inferred from compilation. Earlier 16a native
results remain evidence for their original inputs only.

## Limits and next work

Full desktop acceptance is deliberately open under the user's amendment. The
[backlog](../backlog.md#desktop-ui-deferral) preserves its exact scope without
blocking further coding. This work adds no navigation, picking, gizmos, property
inspector, asset/job browser or console. These retain assignment 17 and 16c-16d;
actual bidirectional UI/terminal parity remains 16e. No MMO capacity or platform
qualification is claimed.

`.gitignore` was reviewed: existing build/out/.tools, bytecode, local roadmap and
reserved coordination rules cover generated shaders, captures and scratch
projects. No new ignore entry or storage root is needed. Source, fixtures and
documentation remain eligible for version control.

Next is **16c: inspector and history**
under the desktop pause, following the [editor plan](../roadmap/editor-plan.md).
Resume the deferred desktop session only on explicit user instruction.

Suggested commit: `feat(editor): add retained scene hierarchy and viewport`.
