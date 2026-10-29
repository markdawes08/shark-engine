# 15b: terminal editing and persistence

Date: 2026-10-28. Base: `5264fa7`, initially clean `main`.
**15b is PASS under current-machine scope.** Full REQ-15 remains NOT_RUN; this checkpoint does
not implement imports/jobs, device capture, runs or export adapters.
No staging, commits or pushes were performed.

## Delivered behavior

The CPU `SharkAuthor` host now exposes nineteen implemented methods through the
existing one-shot/stdio protocol. The [protocol guide](../authoring-protocol.md)
and dated [ADR 0039 extension](../architecture/0039-terminal-authoring-protocol.md)
define their request/result schemas, bounds and runnable examples.

- `scene.mutate` adapts all seven typed commands, batches, undo/redo and history
  clear through the existing retained mutation ledger. It preserves current and
  original revisions, all three outcome levels, exact typed-input matching,
  bounded retention and explicit unknown-outcome reconciliation.
- `scene.preview`, history and mutation status/outcome queries use the same owner.
  Preview is pure; changed edits invalidate query cursors and share native history.
- Project creation validates an empty startup scene and uses the same ProjectFiles
  owner to publish scene then project. Each file has its own publication/durability
  receipt. A second-file failure preserves the first file and reports partial
  publication without activating an owner or claiming a multi-file transaction.
- Save checks the retained project and scene file revisions, keeps history and
  acknowledges the actual saved revision. Reload replaces the scene incarnation,
  clears history and preserves the retry ledger. Dirty reload/close requires
  explicit discard with the current revision; default close still protects edits.
- `text.read`, `text.write` and `text.release` expose bounded script text through
  new shared ProjectFiles methods. Scope is lowercase `scripts/` and `.lua`, valid
  UTF-8 without NUL, at most 32 KiB. These operations neither execute Lua nor edit
  typed scene/project sources. Protocol guards exclude all retained catalog paths
  and detect external project edits before file access. Thirty-two immutable,
  path-bound file tokens provide exact-content conflicts and explicit release.

JSON-RPC request IDs remain correlation only. File operations are not retained
scene mutations; uncertain delivery requires inspecting files. EOF/process death
never saves unsaved changes. No public native handle, alternate registry, generic
filesystem writer, network listener, new scheduler or provider SDK was added.
Runtime/Core/Server dependencies and their protected payloads are unchanged.

## Selected verification

Evidence is under `out/15b-work/`. The Windows inventory was listed before selecting
the affected protocol, shared command/history/retry and file families. Existing
15a framing/ownership cases remain; six new real-process workflows cover editing,
persistence, retries, conflicts and script bounds. The native protocol family adds
a deterministic failure between creation's two file publications. Three focused
ProjectFiles cases cover script identity, limits, rollback and limited durability.

| Selection | Result / evidence |
| --- | --- |
| Windows Debug four affected targets | Build PASS; `windows-build-final.log` |
| Windows Debug affected regression | 8/8 families PASS, 7.84 s; `windows-tests.log`, `windows-debug.xml`; after the fixture clarification, retry family PASS again, 0.24 s, `windows-retry-after-diagnostic.log/xml` |
| WSL Debug | Affected selection PASS using seven unchanged passing families plus the clarified retry family PASS, 0.14 s; `wsl/linux-debug-tests.log`, `wsl-retry/linux-debug-retries.log/xml` |
| WSL ASan+UBSan | NOT_RUN: Clang compiler crashes before tests, including an isolated preprocessed-input reproduction; `wsl-diagnostic/`, `wsl-repro/` |
| Windows ASan alternative | Build PASS; 8/8 affected families PASS, 17.58 s, no reported findings; `windows-asan-build-resumed.log`, `windows-asan-tests.log`, `windows-asan.xml` |
| Published editing example | PASS: create, preview, commit, replay, save, reopen and inspect; `documented-example.log` |
| Existing coverage audit | PASS, 2.32 s; `coverage.log`, `coverage.xml` |

Primary commands:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset windows-debug --target SharkAuthor AuthoringProtocolTests ProjectFilesTests AuthoringCommandTests --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug --show-only
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R "^shark\.tools\.(authoring-(protocol(-process)?|editing-process|commands|batches|history|retries)|project-files)$" --output-on-failure --no-tests=error
```

Portable checks reuse the existing retained WSL Debug/ASan builds, read-only
source mount and offline inputs. No host settings, compiler flags, drivers or
dependencies changed. This CPU adapter does not justify GPU runs, package
redeployment or unrelated Server/engine matrices. The 15a dependency closure
remains applicable: no production target edges or protected inputs changed.
The existing Windows ASan profile supplies the policy's applicable sanitizer
selection after the WSL compiler failure; WSL Debug supplies portable behavior
coverage. WSL ASan/UBSan is still NOT_RUN, not a second required profile or a
passed run. No backlog waiver is needed for choosing this verified alternative.

## Investigated failures

The initial WSL retry allocation sweep segfaulted in a Catch assertion containing
a helper that itself performs a Catch assertion. The fixture now captures the
allocation index, retains the replay in a named local and checks its nested Result
and pointer before comparing outcomes. All original behavioral assertions remain.
The exact case passes 14,250 assertions, followed by the complete retry family on
WSL and Windows. This separates nested assertion/lifetime handling; it does not
establish the underlying cause of the original crash.

Clang 18 crashed while compiling `project_files.cpp` and `source_document.cpp`,
before sanitizer tests. The saved preprocessed source also crashes in isolated
compiler invocation. Windows MSVC separately reported an internal compiler error
while rebuilding the clarified test. Its raw diagnostic compile failed at a
different STL location; baseline and preprocessed-input diagnostics passed,
followed by the canonical rebuild and selected retry test. These observations do
not establish a compiler or host repair. Original logs/repro inputs are retained;
no production warnings, sanitizer settings, compiler versions or host settings
were changed. Failed compiler processes are not engine sanitizer findings.

## Coverage and next work

The manifest maps eleven additional terminal methods and the native script file
service to real process/native fixtures. Existing FeatureWorld keeps seventeen
cases and 56 in-process CPU surfaces. External CPU commands now comprise nineteen
terminal methods plus the exporter; two GPU commands remain separate. Terminal
fixtures use saved FeatureWorld copies and freshly created projects with
independent expected state. No Lua entry is advertised for storing script text.

`.gitignore` already covers builds, generated output, local environments, project
locks and the optional roadmap. No ignore change is needed. Successful temporary
projects use managed scratch and clean up; failure evidence is retained.
The public editing example, coverage audit, final pinned formatting,
repository/link/manifest validation and diff review pass. A final formatting-only
adjustment to the wire schema's description does not change behavior or invalidate
the compiled test results.

Next, 15c adapts existing asset/import/job services to the
retained project owner. Preserve unsaved edits, exact file fences, bounded job
status/cancellation and truthful publication outcomes. Add methods to discovery
only when implemented; keep GPU/run/export adapters for 15d.

Suggested commit: `feat(authoring): add terminal editing and persistence`.
