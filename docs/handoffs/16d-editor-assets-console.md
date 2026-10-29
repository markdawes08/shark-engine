# 16d: asset, job and console panels

Date: 2026-10-29. Base: `935d0b3` (`feat(editor): add component inspector and shared undo redo`),
initially clean `main`. **Implementation and selected headless verification are
complete.** Actual UI acceptance remains deferred/NOT_RUN under the user's
coding-only amendment. Full REQ-16 stays NOT_RUN; current assignment is 16.
The user owns staging, commits and pushing.

## Delivered behavior

The [assets/jobs/console contract](../editor-assets-console.md) defines the new
Assets and Console tabs. Bounded catalog pages, explicit complete inventory,
registered importer discovery, metadata read/create/update, cache freshness and
last-valid artifact inspection all use the existing ProjectSession. Metadata
drafts retain exact-file tokens and truthful publication receipts, and join the
existing unapplied-draft close protection. Scene Save/Undo do not write or undo
asset metadata.

Job controls distinguish submission, observation, explicit owner publication,
cancellation and terminal acknowledgement. Original cancel/drain/take receipts
survive later display refreshes. No polling publishes or releases outcomes.
Source/cache conflicts preserve last-valid output; closing retains the original
cancel/join-before-owner-release behavior. Asset content changes invalidate the
cached viewport without adding an import or rendering path.

The bounded session console consumes existing structured replies and errors.
Nested mutation/job failures, correlation and durability remain distinct.
Display logging is best-effort and does not change operation replies; project
state observation failure no longer hides an already completed operation.
The shell visibly requires refresh before ordinary edits, while scoped cleanup
remains possible. Open uses its complete original status before panels bind to
the owner. No engine/public protocol/source version or FeatureWorld surface is
added; this is Editor-private composition under
[ADR 0041](../architecture/0041-editor-scene-workspace.md).

## Selected verification

Selection follows the [testing policy](../testing-policy.md): Windows Debug
Editor and affected asset/session/hierarchy/inspector/headless-ImGui/CLI checks;
the four portable CPU adapters in retained WSL Debug and ASan/UBSan. Existing
importer algorithms, protected dependency inputs and graphics are unchanged, so
their unrelated matrices/packages/GPU runs are not selected. No desktop window,
native input, Editor or GPU fixture has been launched.

| Selection | Result / evidence under `out/16d-work/` |
| --- | --- |
| Windows Debug Editor and affected adapter targets | PASS; `build-final.log`. No Editor launch. |
| Selected Windows headless CTests | 6/6 PASS, 2.38 s; `windows-inventory.log`, `windows-tests.log/xml`. Assets, session, hierarchy, inspector, GUI adapter and CLI. |
| WSL GCC Debug selected adapters | 4/4 PASS, 0.63 s; `wsl/linux-debug-{build,inventory,tests}.log`, `wsl/linux-debug-tests.xml`. |
| WSL Clang ASan+UBSan selected adapters | 4/4 PASS, 1.98 s; matching `wsl/linux-asan-ubsan-*` logs/XML. No ImGui/native graphics sanitizer claim. |
| Repository, links/manifest, formatting and diff | PASS; `repository-final.log`, `contract-final.log`, `format-final.log`; `git diff --check` clean. |
| Actual asset/metadata/job/console controls and connected UI | NOT_RUN; desktop pause, BL-16A-UI-01. |

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset windows-debug --target EditorAssetPanelTests EditorProjectSessionTests EditorScenePanelTests EditorInspectorTests Editor EditorGuiTests --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R "^shark\.editor\.(assets|project-session|scene-panel|inspector|gui-adapter|cli)$" --output-on-failure --no-tests=error
```

The new `shark.editor.assets` family has four CPU cases, seed 1337, timeout
60 seconds. Real temporary metadata/source/cache files prove fixed-page/stale
catalog behavior, exact-file tokens, failed imports preserving last-valid output,
explicit ready/drain/take semantics, cancellation and owner release. Existing
session tests gain console publication/rejection assertions and two focused
console cases. One headless ImGui case proves drawing a ready job does not
publish it and drawing a terminal job does not acknowledge it; it captures
asset/console draw data without a native window. No new fixture download is used.

WSL builds the four CPU adapter targets sequentially in the retained canonical
Debug and ASan/UBSan directories, using the existing namespace/chroot wrapper and
`--parallel 2`; its selection is
`^shark\.editor\.(assets|project-session|scene-panel|inspector)$`.
No host/WSL resource or kernel settings changed. Initial passing results are
retained with `initial-` names; final runs cover the subsequent session/console
and cleanup-observation changes. These repeats are not additional unique tests.

The first Windows build rejected unreachable code after Catch2's fatal assertion
in a test deadline helper (`build-debug.log`). The redundant unreachable statement
was removed; final build and checks pass. Review also corrected initial catalog
observation in the GUI fixture, placement of the status-health fields, and panel
binding after project open before qualification. Workloads, deadlines and
tolerances were not reduced.

The handoff's post-request status-failure guarantee is a narrow best-effort catch
and explicit health state; no new allocation-failure injection framework was
introduced. Selected tests exercise normal/rejected outcomes and publication
classification, not arbitrary process-wide allocation exhaustion.

Coverage adds `editor.assets-jobs-console`. Core/Tools behavior, shader/device
inputs, protected dependency closures, public examples and FeatureWorld cases are
unchanged; no unrelated import/Server/graphics/package matrix was rerun.

## Limits and next work

The advanced metadata UI edits a bounded JSON envelope rather than providing
format-specific settings widgets. Sources must already exist in the project;
there is no source-copy dialog, scan, watcher, implicit import or automatic cache
maintenance. The console is bounded in-memory display history, not a persistent
audit or command executor. Existing hardware/platform scope is unchanged.

Actual controls, layout/focus, metadata close decisions, import publication and
viewport updates remain in [BL-16A-UI-01](../backlog.md). Their deferral permits
coding progression but does not close full REQ-16 or establish UI/API parity.

`.gitignore` was reviewed: existing build/out/cache/local-roadmap rules cover all
new outputs. No new ignore pattern, dependency or storage root is needed.

Next is **16e: connected human/terminal acceptance**. While desktop testing is
paused, continue only its coding/headless preparation; actual bidirectional UI
interaction awaits the user's explicit resumption.

Suggested commit: `feat(editor): add asset jobs and diagnostic console panels`.
