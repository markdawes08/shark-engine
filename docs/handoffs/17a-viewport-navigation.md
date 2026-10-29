# 17a: viewport navigation and picking

Date: 2026-10-29. Base: `7a54cf2` (`test(editor): connect terminal and editor
authoring handoffs`), initially clean `main`. **Implementation and selected
headless verification are complete.** User-owned staging, commits and
pushing remain untouched. Actual UI interaction remains deferred under the
desktop pause; full REQ-16 and REQ-17 remain NOT_RUN.

## Delivered behavior and contracts

The [viewport navigation contract](../viewport-navigation.md) and
[ADR 0042](../architecture/0042-viewport-navigation-and-picking.md) define the new
display-only camera, image-coordinate routing and triangle picking. The existing
authored Camera seeds navigation; source components, scene state/history and
global cell identity are unchanged by looking around or selecting geometry.

Core extraction and typed Tools requests gain an optional validated
RenderViewCamera before culling. Default callers retain authored-camera behavior;
original camera provenance, immutable asset leases, exact scene revision and
source/cache fences remain intact. There is no new terminal method, source or
prepared format, component, third-party dependency or native handle exposure.

The editor uses one retained snapshot for GPU preparation and picking. Orbit,
pan and dolly adopt complete bounded view candidates. Pointer routing uses the
actual scaled image rectangle, cancels stale gestures and respects focus/text/
modal/blocked boundaries. Nearest actual indexed triangles determine selection,
with full affine transforms, face policy, clipping and bounded work. Revision
checks prevent stale selection from targeting a recreated entity.

The [assignment-17 plan](../roadmap/viewport-interaction-plan.md) records the
four agreed subchunks. 17b's transform/drag transactions and 17c's gizmos are
separate; this increment does not introduce a second authoring mutation path.

## Selected verification

Selection follows the [testing policy](../testing-policy.md). Windows Debug covers
changed Core extraction and its math foundation, retained Tools viewport reads,
Client viewport packing, editor navigation, headless GUI routing and Editor CLI.
Portable math/extraction/authoring/navigation run in retained WSL Debug and
ASan+UBSan. No Editor window, native input or windowed GPU fixture is launched.

| Selection | Result / evidence under `out/17a-work/` |
| --- | --- |
| Windows affected targets and Editor | PASS; `windows-build.log`. No Editor window launch. |
| Windows selected CPU checks | 7/7 PASS, 1.81 s; `windows-inventory.log`, `windows-tests.log/xml`. |
| WSL Debug selected CPU checks | 4/4 PASS, 0.49 s; `wsl/linux-debug-{build,inventory,tests}.log`, `wsl/linux-debug-tests.xml`. |
| WSL ASan+UBSan selected CPU checks | 4/4 PASS, 1.63 s; matching `wsl/linux-asan-ubsan-*` logs/XML. |
| Repository, links/manifest, formatting and diff | PASS; `contract.log`, `repository.log`, `format.log`; `git diff --check` clean. |
| Actual navigation/picking/focus/layout and both-GPU view updates | NOT_RUN; desktop pause and UI backlog. |

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset windows-debug --target Editor Tests RenderMathTests SceneRenderTests AuthoringViewportTests EditorViewportNavigationTests EditorGuiTests --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R "^(shark\.core\.(render-math|scene-render)|shark\.tools\.authoring-viewport|shark\.client\.scene-viewport|shark\.editor\.(viewport-navigation|gui-adapter|cli))$" --output-on-failure --no-tests=error
```

Inventories were listed before selection. WSL uses the existing namespace/chroot
wrapper and canonical Debug/ASan+UBSan builds, sequentially with `--parallel 2`.
It builds RenderMathTests, SceneRenderTests, AuthoringViewportTests and
EditorViewportNavigationTests and selects
`^(shark\.core\.(render-math|scene-render)|shark\.tools\.authoring-viewport|shark\.editor\.viewport-navigation)$`.
No host/WSL resource or kernel setting was changed. All selected builds/tests
passed without a failing run or relaxed workload/tolerance.

Five new CPU navigation cases independently check geometry and coordinates,
including clipping, silhouettes, reflections, hierarchy/origin and full work
budget rejection. Two Core extraction cases and one retained Tools case qualify
the override and preservation of dirty/history/source state. Two headless GUI
cases cover fresh-press cancellation and an actual ImGui image rectangle under
nonuniform framebuffer scale. The new navigation family uses seed 1337, timeout
60 seconds and CPU/editor/unit/integration labels. Existing families retain their
original seeds, limits and timeouts; synthetic input establishes no native pass.

Unchanged 16d/16e project ownership, asset jobs, history and connected terminal
results remain applicable to those adapters at the base revision; this selection
covers the newly affected viewport path. The public documentation code examples
are unchanged; existing and new tests consume the public headers directly.
No unchanged package, Server/network/storage, full-project CTest or extra
sanitizer matrix is selected. The protected dependency graph and precompiled
shader inputs are unchanged; the view override adds engine-owned values only.

Coverage maps `editor.viewport-navigation` to the real CPU geometry and routing
fixtures, and extends existing Core/Tools viewport mappings for the view override.
The editor-private presentation behavior adds no command or FeatureWorld case.
Current assignment becomes 17; full REQ-16 remains open for its deferred UI gate.

## Limits and next work

Navigation starts from an explicitly selected authored Camera. Picking covers
rendered geometry, not nonrendered entity icons or future alpha-mask sampling.
No spatial index, new renderer, cursor capture or origin-rebase manager is added.
Actual controls and GPU results still require the checks in the
[backlog](../backlog.md) after explicit desktop resumption.

Next is **17b: transform manipulation and drag transactions**. Implement shared
local/world/parent math, snapping, supported scale and shear/singular rejection,
with one undo group per committed drag and agent-equivalent resulting transforms.
Continue headless coding while the desktop pause remains active.

`.gitignore` was reviewed: existing build/out/cache/local-roadmap patterns cover
generated outputs. No additional ignore rule or dependency cache is required.

Suggested commit: `feat(editor): add viewport navigation and triangle picking`.
