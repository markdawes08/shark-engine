# 16c: component inspector and shared history

Date: 2026-10-29. Base: `dbba952` (`feat(editor): add retained scene hierarchy and viewport`),
initially clean `main`. **Implementation and selected headless verification are
complete.**
Desktop acceptance is deferred/NOT_RUN under the user's coding-only amendment.
Full REQ-16 remains NOT_RUN; `current_assignment` is 16.
The user owns staging, commits and pushing.

## Delivered behavior

The [inspector](../editor-inspector.md) extends the same retained project owner
and ScenePanel mutation path. Entity/revision-bound component drafts use the
existing shared schemas and authoring commands; history uses current tickets and
revisions. No new terminal method, source schema, public component, renderer or
FeatureWorld case is introduced. [ADR 0041](../architecture/0041-editor-scene-workspace.md)
records the Editor-private extension.

The inspector supports Name, local Transform, Camera and MeshRenderer. Widgets
use the existing schema labels, types, units and enumeration, with typed double,
xyzw quaternion, uint32 and Boolean values. MeshRenderer requires explicit asset
identity; no import or resolution happens during property editing. Optional
Camera/MeshRenderer can be added or removed in the draft. Apply submits one
atomic four-command batch and one changed history group. Invalid later fields
cannot partially publish earlier Name/Transform changes.

Selection changes, intervening edits and delete/undo recreation leave an old
draft stale until explicit reload. Known mutation success retains a separate
display-refresh error. Shared undo/redo checks the original history ticket and
revision through the same retained pending-mutation slot as hierarchy edits;
there is no second edit owner or blind retry.

The workspace presents hierarchy, inspector/history and viewport panels. Scene
mutations occur before emitting the viewport image. Unapplied drafts get a
separate close confirmation before the existing dirty-scene flow; save retains
the draft and reports that it was not applied. There are no added shortcuts.

## Selected verification

Following the [testing policy](../testing-policy.md), build the Editor and affected
CPU adapters; select inspector, hierarchy and session regressions and relevant
headless GUI fixtures. Run portable adapter checks on retained WSL Debug and
ASan/UBSan. Keep required native checks in [BL-16A-UI-01](../backlog.md).
No desktop automation, Editor launch or GPU fixture is permitted while paused.

| Selection | Result / evidence under `out/16c-work/` |
| --- | --- |
| Windows Debug Editor and affected adapter targets | PASS; `build-cpu.log`, `build-editor.log`. Editor compiled without launch. |
| Windows selected headless CTests | 5/5 PASS, 1.43 s; `windows-selected-inventory.log`, `windows-tests.log/xml`. Inspector, hierarchy, session, headless ImGui adapter and CLI. |
| WSL GCC Debug selected adapters | 3/3 PASS, 0.38 s; `wsl/linux-debug-{build,inventory,tests}.log`, `wsl/linux-debug-tests.xml`. |
| WSL Clang ASan+UBSan selected adapters | 3/3 PASS, 1.30 s; matching `wsl/linux-asan-ubsan-*` logs/XML. ImGui/native Windows UI is outside this sanitizer selection. |
| Repository/link/manifest, formatting and diff checks | PASS; `contract-final.log`, `repository-final.log`, `format-final.log`, `git diff --check`. |
| Actual inspector/history controls and draft-close interaction | NOT_RUN; desktop pause, [BL-16A-UI-01](../backlog.md). |

Windows selection:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R "^shark\.editor\.(inspector|scene-panel|project-session|gui-adapter|cli)$" --output-on-failure --no-tests=error
```

Built `Editor`, `EditorInspectorTests`, `EditorScenePanelTests`,
`EditorProjectSessionTests` and `EditorGuiTests`, using the same Windows wrapper
and `--parallel 2`. The selected inventory was listed after reconfiguration.
The new inspector family has four cases, seed 1337 and a 60-second timeout;
one added in-memory ImGui case uses the existing adapter family. It renders
current, blocked and stale drafts and verifies the draft and source remain
distinct, without a window or device.

WSL reused the retained namespace/chroot wrapper and canonical
`/work/build-05a-final-client/{linux-debug,linux-asan-ubsan}` directories.
`wsl-driver.sh` / `wsl-commands.sh` build only the three CPU Editor adapter targets
and select `^shark\.editor\.(inspector|scene-panel|project-session)$`. No host or
WSL resource/kernel settings changed. The repeated profiles are not additional
unique test families.

Review caught a draft-discard defect before verification: clearing only the
editable value left its baseline and therefore a dirty flag. Explicit discard
now clears both, covered in the model and headless widget fixtures. All selected
builds/tests passed on their first run. No tolerances, workloads or baselines
were changed.

The repository check initially selected Python without PyYAML. Running it with
the existing `.tools/venv/Scripts/python.exe` passed; no dependency was installed
or changed. The initial interpreter failures remain in
`repository-system-python.log` and `repository-subprocess-python.log`.

Core, Tools, Client graphics, dependencies and Runtime inputs are unchanged.
The changes are confined to Editor-private adapters and test wiring, so no
unrelated Core/Tools matrix, native GPU run, protected-package rebuild or new
FeatureWorld case was selected. Existing results remain historical for their
original inputs. Coverage adds `editor.inspector-history`; full REQ-16 stays
NOT_RUN and component/command/Lua inventories remain unchanged.

## Limits and next work

Full REQ-16 is not closed by CPU passes. Actual inspector controls, layout,
focus, hierarchy/viewport updates, shared history and draft/close decisions need
the deferred UI session. Assets/jobs/console remain 16d; actual bidirectional
UI/terminal semantic handoff remains 16e. Navigation, picking and gizmos remain
assignment 17.

`.gitignore` was reviewed. Existing build/out/cache/local-roadmap rules cover
all generated outputs; no new ignore pattern or storage root is needed.

Next is **16d: assets, jobs and console** under
the desktop pause, following the [editor plan](../roadmap/editor-plan.md).

Suggested commit: `feat(editor): add component inspector and shared undo redo`.
