# 17b: transform manipulation and drag transactions

Date: 2026-10-29. Base: `3290ea3` (`feat(editor): add viewport navigation and
triangle picking`), initially clean `main`. User-owned staging, commits and
pushing remain untouched. Desktop interaction remains paused; full REQ-16 and
REQ-17 remain NOT_RUN. **Implementation and selected headless verification are complete.**

## Delivered behavior and contracts

The [manipulation contract](../transform-manipulation.md) and
[ADR 0043](../architecture/0043-transform-manipulation.md) define the shared pure
EngineTools helper and editor-private TransformDrag. Translation, rotation and
scale support local/world axes, cumulative snapping and signed/nonuniform scale.
World operations use full affine parents and preserve the entity pivot; invalid
scale, unreliable inverses and unrepresentable local shear explicitly reject.
Existing Core tolerances and global cell/authority boundaries are unchanged.

ScenePanel observes child local TRS and parent world matrix at the same complete
revision. Drag updates validate one `set_transform` through `scene.preview`,
without changing live scene state/history or consuming mutation identities.
One retained commit creates one command/history group. Cancel discards values;
a failed latest update cannot commit a prior candidate. Selection transition
epochs, full revisions, current owner observations and pending reconciliation
fence stale gestures, including away-and-back selection and project reopen.
Known commit success survives display-refresh failure through the existing path.

The new public C++ API has no private handles or external dependencies. There is
no new RPC, component, source/protocol version, FeatureWorld command or alternate
mutation owner. Terminal parity uses existing inspection, preview and retained
`set_transform`, history and persistence. Gizmo widgets and rendered candidate
presentation remain 17c; no windowed work was performed.

## Selected verification

Selection follows the [testing policy](../testing-policy.md):
new math/drag families, affected ScenePanel/Inspector/headless GUI consumers on
Windows Debug, and portable math/drag/ScenePanel in retained WSL Debug and
ASan+UBSan. No whole-project inventory, unrelated Server/network/storage/package
matrix or native graphics checks are selected.

| Selection | Result / evidence under `out/17b-work/` |
| --- | --- |
| Windows affected targets and Editor | PASS; `windows-build.log`. Built without launching an Editor window. |
| Windows selected CPU/headless checks | 6/6 PASS, 1.76 s; `windows-inventory.log`, `windows-tests.log/xml`. |
| WSL Debug portable selection | 3/3 PASS, 0.42 s; `wsl/linux-debug-{build,inventory,tests}.log`, `wsl/linux-debug-tests.xml`. |
| WSL ASan+UBSan same selection | 3/3 PASS, 1.42 s; matching `wsl/linux-asan-ubsan-*` logs/XML. |
| Repository, links/manifest, pinned formatting and diff | PASS; `repository.log`, `contract.log`, `format-check.log`; diff check clean. |
| Actual gizmo controls, rendered previews and native input/focus/layout | NOT_RUN; 17c integration and explicit desktop resumption are required. |

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset windows-debug --target TransformManipulationTests EditorTransformDragTests EditorScenePanelTests EditorInspectorTests EditorGuiTests Editor --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R "^(shark\.tools\.transform-manipulation|shark\.editor\.(transform-drag|scene-panel|inspector|gui-adapter|cli))$" --output-on-failure --no-tests=error
```

Both inventories were listed before selecting tests. WSL uses the existing
namespace/chroot wrapper and canonical Debug then ASan+UBSan builds sequentially,
with `--parallel 2`, targeting TransformManipulationTests,
EditorTransformDragTests and EditorScenePanelTests. Its selection is
`^(shark\.tools\.transform-manipulation|shark\.editor\.(transform-drag|scene-panel))$`.
No host/WSL resource or kernel settings changed. Selected builds/tests had no
failures or retries; only existing vendor CMake deprecation warnings were emitted.

Six math cases use independent matrix/vector expectations for axes, parents,
pivots, snapping and explicit invalid results. Five adapter cases exercise the
actual retained protocol owner, preview purity, one-group commit/undo/redo,
no-op/cancel/correction, full-descendant rejection, selection/revision/incarnation
conflicts, creating-thread guards and terminal-equivalent saved/reopened state.
The terminal parity case uses in-process protocol requests; it is not a new
cross-process or actual UI acceptance result. Both new families use seed 1337,
60-second timeouts and explicit CPU/unit labels (drag also editor/integration).

The tests consume the new public helper header; no documentation C++ example was
added or changed. Existing Core transforms, source formats, retained ledger,
renderer, shader and protected dependency boundaries are unchanged. 17a and 16e
results remain historical evidence for those unchanged paths, not new passes.
Fresh ScenePanel, Inspector and headless GUI regressions cover the affected
selection observer and transform adapter consumers. No private failure hook or
new evidence framework was added. Code stayed unchanged after these final runs.

## Limits and next work

Next is **17c: gizmo integration** through these services. Connect controls,
local/world modes, snapping, validated candidate rendering and focus/capture/
cancellation/close decisions. Gate candidate display on `active() && !stale()`;
the owned accessor alone does not promise current selection/revision validity.
Select and pin any new dependency before use.
Do not temporarily mutate the retained Scene to draw a preview, silently rebase a
drag, or replace an uncertain mutation identity. Continue headless coding until
explicit desktop resumption. The [UI backlog](../backlog.md) lists the remaining
actual interaction/rendering and connected terminal acceptance.

Coverage adds `editor.transform-drag` for the two real CPU families, with a
justified external fixture for gesture/math state; existing FeatureWorld
AuthoringCommands remains the shared command coverage. `.gitignore` was reviewed:
existing build/out/cache/local-roadmap patterns cover all new generated outputs;
no additional cache or ignore pattern is needed.

Suggested commit: `feat(authoring): add transform manipulation and drag transactions`.
