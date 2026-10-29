# 16e: connected editor/terminal acceptance preparation

Date: 2026-10-29. Base: `f05a795` (`feat(editor): add asset jobs and diagnostic
console panels`), initially clean `main`. **Coding and selected headless preparation
are complete.** The user's desktop pause remains active. Actual UI acceptance
and full REQ-16 remain **NOT_RUN**. The user owns staging, commits and pushing.

## Delivered work

One connected CPU process case bridges the existing public terminal example,
Editor-private ProjectSession/ScenePanel/InspectorPanel/AssetPanel and real
SharkAuthor stdio processes. It reuses the licensed offline triangle, existing
process runner and bounded pipe client. A fixed-purpose test executable supplies
editor actions without a window, renderer or general-purpose editor RPC service.

The case compares independently expected saved component, hierarchy and asset
values across terminal creation, editor edits/import/save/close, terminal
continuation and editor reopen. Editor edits change Name, local translation and
Camera height. An exact-token metadata edit removes an optional default setting,
forcing a new root import key while preserving geometry, source bytes and the
dependency identity. Terminal continuation changes Name/translation and reparents
the entity; the next editor owner observes those values and exercises undo/redo
before explicitly discarding its temporary edit. Both directions reject a competing writer.
Explicit dirty-state decisions, history preservation across save and history
reset on reopen are part of the same connected flow. Isolated stale-draft,
cancellation and token-limit matrices stay in their existing owners.

The [acceptance procedure](../editor-acceptance.md) gives the remaining actual
UI/terminal workflow. [BL-16A-UI-01](../backlog.md) retains its desktop gate and
earlier native composition/focus/lifecycle checks. No public API, protocol/schema
version, dependency, component or FeatureWorld case is added. Production editor,
Core, Tools, Runtime and Server behavior is unchanged.

## Selected verification

Selection follows the [testing policy](../testing-policy.md). Build the new CPU
fixture and SharkAuthor; run the connected process case on Windows Debug and
retained WSL Debug/ASan+UBSan. The unchanged editor adapters retain their final
16d Windows and WSL results at this base. The connected case itself runs the
unchanged public example and its independent oracle, so it does not require a
second standalone example run. No native window, Editor or GPU fixture is run.

| Selection | Result / evidence under `out/16e-work/` |
| --- | --- |
| Windows Debug CPU fixture and public host | PASS; `windows-build.log`, `windows-build-final.log`. |
| Windows connected process case | 1/1 PASS, 1.27 s (1.29 s total); `windows-inventory.log`, `windows-tests.log/xml`. |
| WSL Debug connected process case | 1/1 PASS, 0.68 s; `wsl/linux-debug-{build,inventory,tests}.log`, `wsl/linux-debug-tests.xml`. |
| WSL ASan+UBSan connected process case | 1/1 PASS, 1.34 s; matching `wsl/linux-asan-ubsan-*` logs/XML. |
| Repository, links/manifest, formatting and diff | PASS; `contract.log`, `repository.log`, `format.log`; `git diff --check` clean. |
| Actual connected UI and remaining native checks | NOT_RUN; desktop pause, BL-16A-UI-01. |

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset windows-debug --target EditorHandoff SharkAuthor --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug --show-only -R "^shark\.editor\.handoff-process$"
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R "^shark\.editor\.handoff-process$" --output-on-failure --no-tests=error
```

The case has CPU/editor/integration labels, fixed seed 1337 in fixture phase
records and a 120-second CTest bound. Import readiness uses an observed state
and bounded deadline, not a sleep. The parent bounds child responses/exit and
terminates/drains its known child on failure. Unexpected failures retain inputs
and a compact transcript under the configured build's `tests/editor-handoff/`;
successful scenario directories are removed. WSL reuses the existing namespace/
chroot wrapper and canonical `/work/build-05a-final-client/{linux-debug,linux-asan-ubsan}`,
building the same two targets sequentially with `--parallel 2`, then listing and
running the same test selection. No settings are changed.

The initial WSL GCC build rejected deprecated C++20 `filesystem::u8path` under
warnings-as-errors. The fixture now constructs an explicit UTF-8 path, matching
the existing process fixtures; Windows retains native wide-argument handling.
The first compiler failure is retained in `wsl/initial-linux-debug-build.log` and
`wsl/initial-driver.log`. Final builds and selected tests pass; no engine behavior,
workload, deadline or tolerance was changed to obtain these results.

No whole-project matrix, repeated source-freeze ledger, new dependency cache,
host/WSL setting change or standalone package qualification is needed for this
test-only integration. Existing protected target graphs are unchanged; the new
fixture is test-only and links CPU Tools without Client/GUI dependencies.

Coverage adds `editor.terminal-handoff` while keeping full REQ-16 NOT_RUN and
`current_assignment` at 16. The test fixture's results do not establish actual
human interaction, full UI/API equivalence or GPU qualification.

## Limits and next work

16e's remaining gate is actual editor interaction using the connected procedure,
plus the deferred native checks, after explicit desktop resumption. Until then,
the user's backlog amendment permits subsequent coding, including assignment-17
checkpoint planning, without closing full REQ-16. Do not automatically run UI
checks on a later coding request or infer permission from elapsed time.

`.gitignore` was reviewed; existing build/out/cache/local-roadmap rules cover all
new generated files. No new pattern is needed. Test source and documentation
remain eligible for version control; successful disposable projects were cleaned.

Suggested commit: `test(editor): connect terminal and editor authoring handoffs`.
