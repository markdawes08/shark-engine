# 17d: connected viewport authoring acceptance

Date: 2026-10-29. Base: `f1cdc4c` (`feat(editor): integrate transform gizmos
and subtree previews`), initially clean `main`. User-owned staging, commits and
pushing remain untouched. Desktop interaction remains paused. This checkpoint
completes the headless connected preparation; actual UI/GPU acceptance and full
REQ-16/REQ-17 remain NOT_RUN under the explicit backlog amendment.
**Coding and selected headless verification are complete.**

## Delivered work

Extend the existing EditorHandoff fixed-action executable and public-terminal
process fixture for transform continuation. The helper remains test-only and CPU
portable. It uses the existing ProjectSession, scene observations, pure gizmo
geometry/gesture, TransformDrag and typed viewport preview; it is not another
authoring host or a mutable Scene. No public command, component, format, dependency,
shader or production API changes are introduced.

Review also found that 17c cancelled a scale gesture at the first rejected zero
candidate, making ordinary sampled drags unable to cross from positive to negative
scale. The widget now retains the frozen gesture after a transform validation
error while held, with no committable candidate and committed rendering restored.
A corrected negative value may preview/commit once; release while invalid cancels
without history. Conflict, projection and render-preparation failures still cancel.
The existing zero-scale validator/tolerances remain unchanged. This supersedes
17c's cancel-on-every-invalid-transform behavior in the current gizmo contract.

The connected scenario starts from the public terminal example and its licensed
imported content. Terminal setup supplies a rotated parent and exact reference
poses. The editor adapter previews cumulative input, commits through retained
set_transform/history, and saves before a real terminal process continues the
project. Independent expected local/world values and saved JSON checks establish
the result instead of copying expected transforms from adapter receipts. Existing
16e project/asset handoff coverage remains in the same runner.

The [actual acceptance procedure](../viewport-acceptance.md) describes the remaining
human/terminal workflow on both current-PC GPUs. Synthetic/headless input is
preparation only; required navigation, picking, gizmo, focus/layout and rendering
results remain in the [UI backlog](../backlog.md).

## Selected verification

Under the [testing policy](../testing-policy.md), verification covers
the editor's headless Windows Debug behavior inventory, excluding unchanged
dependency setup, plus the changed connected process fixture in retained WSL
Debug and ASan+UBSan. Reuse valid final-input 17c Core/Tools/Client extraction,
packing and pure geometry results; do not repeat unrelated engine matrices.

| Selection | Result / evidence under `out/17d-work/` |
| --- | --- |
| Windows affected test targets, public SharkAuthor and Editor | PASS; `windows-build-handoff.log`, `windows-build-editor.log`, `windows-build-handoff-final.log`, `windows-build-handoff-portable-fix.log`. No Editor window launched. |
| Windows editor behavior inventory | 12/12 PASS; initial inventory took 6.39 s (`windows-tests.log/xml`), with the final helper rerun 1/1 PASS in 2.40 s (`windows-handoff-final.log/xml`). Inventory: `windows-inventory.log`. |
| WSL Debug connected process fixture | 1/1 PASS (two Python scenarios), 0.97 s; `wsl-final/linux-debug-{build,inventory,tests}.log`, `wsl-final/linux-debug-tests.xml`. |
| WSL ASan+UBSan connected process fixture | 1/1 PASS (same two scenarios), 2.40 s; matching `wsl-final/linux-asan-ubsan-*` logs/XML. |
| Repository, links/manifest, pinned formatting and diff | PASS; `repository.log`, `contract.log`, `format-check.log`; diff check clean. |
| Native controls/rendering/connected UI on both GPUs | NOT_RUN; explicit desktop pause. |

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset windows-debug --target EditorHandoff SharkAuthor EditorGizmoTests EditorTransformDragTests EditorViewportNavigationTests EditorAssetPanelTests EditorInspectorTests EditorScenePanelTests EditorProjectSessionTests EditorGuiTests Editor --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R "^shark\.editor\.(gizmo|transform-drag|viewport-navigation|assets|inspector|scene-panel|project-session|gui-adapter|cli|window-cli|presentation-cli|handoff-process)$" --output-on-failure --no-tests=error
```

The inventory was listed before selection. Window/presentation CLI cases reject
invalid arguments or missing shader bundles before opening a window or Vulkan
loader. The dependency-setup family was excluded because product dependencies
and closures did not change; CMake adds existing CPU adapter sources only to the
test-only helper. All affected editor behavior families were included. The
connected process family contains both the retained 16e scenario and the new
17d transform continuation scenario, with its original 120-second timeout.

The first WSL Debug build rejected a partially initialized viewport request in
the test helper under GCC's warnings-as-errors policy. The helper now initializes
all required aggregate fields directly. Original failure output remains in
`wsl/linux-debug-build.log` and `wsl/driver.log`; the failure stopped that attempt
before tests or ASan. Windows rebuilt and reran only the affected process family
after this portable initialization correction; its other eleven passing inputs
were unchanged. Final WSL evidence uses `wsl-final/` so the failure is preserved.
An initial final-wrapper invocation referenced a missing renamed scratch script;
no engine build or test ran in that invocation, and its diagnostic remains in
`wsl-final/driver-missing-script.log`. The corrected wrapper completed both profiles.

WSL uses the existing namespace/chroot wrapper and canonical Debug then
ASan+UBSan builds sequentially with `--parallel 2`, targeting EditorHandoff and
SharkAuthor. It lists and runs `^shark\.editor\.handoff-process$` only. No
host/WSL resources or kernel settings changed. All final code inputs remained
unchanged after the passing selected runs. No desktop automation, native window
or GPU fixture was launched.

The new GUI case samples positive scale, snapped zero and negative scale on
separate frames, then verifies one-group commit/undo; its alternate path releases
at zero and verifies no stale positive candidate commits. The process fixture
uses seed 1337 and existing bounded pipes/barriers, absolute deadlines and scratch
cleanup. Independent Python literals specify parent/child transforms and complete
column-major world matrices, while saved JSON/component/asset/license checks
verify persistence and collateral-state preservation. No additional report
framework, source-freeze ledger or public host was added.

Unchanged final-input 17c Core extraction, retained Tools viewport, Client float
packing and portable geometry/navigation/drag results remain applicable. Its
headless ImGui family is rerun here for the widget correction. Actual GPU and
UI evidence remains deferred; CPU results do not replace it. No unrelated
Server/network/storage/package matrix or extra sanitizer profile was selected.

## Coverage, limits and next work

Extend the existing editor/terminal handoff mapping to REQ-17 and its connected
transform fixtures. Existing FeatureWorld AuthoringCommands owns the semantic
set_transform surface. No duplicate FeatureWorld case or public protocol entry
is added. `.gitignore` was reviewed: existing build/out/cache/local-roadmap
patterns cover generated outputs. Successful scenario copies use managed scratch
cleanup; unexpected failures retain their inputs and compact diagnostics.

Full assignment 17 remains open for actual UI/GPU acceptance. Explicit desktop
resumption is required before those checks. The user's nonblocking amendment
permits subsequent coding; next coding work is assignment 18's physics checkpoint
planning/implementation, without treating deferred UI gates as passed. Preserve
the current owner/history/preview contracts when physics adds transform authority.

Suggested commit: `test(editor): connect viewport transforms and terminal handoffs`.
