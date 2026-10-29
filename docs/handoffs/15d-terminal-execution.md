# 15d: terminal run, export and capture

Date: 2026-10-28. Base: `71cbf14` (`feat(authoring): add terminal asset imports and job control`),
initially clean `main`. **15d is PASS** under current-machine scope. Full REQ-15
remains NOT_RUN until 15e connected acceptance. Staging, commits and pushing remain
the user's work.

## Delivered behavior

The [protocol](../authoring-protocol.md) and
[ADR 0039](../architecture/0039-terminal-authoring-protocol.md) add three methods
without changing protocol version 1 or the existing source/cooked formats:

- `scene.run` captures the active authored scene, including unsaved values, and
  uses the existing deterministic scene runner. Its `scene-integrity` case checks
  the isolated snapshot and stable per-tick state, optionally against an expected
  entity count. This is static integrity coverage, not gameplay, Lua or input
  execution. Native PASS/FAIL results remain visible to the caller.
- Configured `scene.export` prepares the active scene through the retained
  ProjectFiles/AuthoredScene/catalog/cache owners. A bounded Windows child adapts
  the existing Python package builder; it never opens another project writer.
  Native staging and final candidate validation surround package assembly and
  publication. Current dirty values, history and mutation retries survive.
- Optional `scene.capture` uses a separate `SharkAuthorClient` executable and
  application-owned Client provider. CPU `SharkAuthor` and EngineTools remain
  independently buildable without Client, Editor, window or graphics dependencies.
  The provider renders an immutable prepared snapshot through the existing
  GraphicsDevice and publishes bounded linear RGBA8, little-endian float depth
  and `shark.authoring-capture` v1 metadata after clean GPU completion/close.

Discovery exposes 41 base CPU methods, 42 with export configured, and 43 with
export and capture configured. Unavailable artifact methods are absent from
discovery and return `capability_unavailable`. Host startup selects trusted export
helper paths and exact capture shaders/device/validation policy; requests cannot
replace that configuration. Public C++ additions are protocol options and the
immutable capture-provider boundary; native candidate handles remain private.

Export/capture require current scene/catalog/cache revisions, an explicit startup
camera and fresh client-permitted dependency closure. They check saved project
and scene file fences both before preparation and immediately before publication,
while consuming current unsaved scene values. Output must be a fresh directory
outside the project root with an existing parent. Missing parents return a
structured error without terminating the host. No request ID becomes mutation
retry identity, and no operation silently saves or discards authoring state.

The export bridge retains full notices, Runtime CRT closure and Core-only startup
content. Its child has a 60-second deadline, bounded output, restricted inherited
handles and process-tree cleanup. A known publication remains `published:true`
after a later failure. Lost completion after publication permission reports an
explicit unknown outcome and original output path for reconciliation.

Capture defaults to 160x120, permits extents through 2048 with at most 1,048,576
pixels, and keeps bounded local origins distinct from world anchors. Reports bind
the original authored revision and dirty state to the fresh rendered incarnation,
content digest, actual adapter/driver, artifacts and diagnostics. Publication does
not claim confirmed disk-flush durability.

## Selected verification

Evidence is under `out/15d-work/`. Inventories were listed before selection. The
affected regression covers terminal protocol/edit/import/run ownership, existing
scene-runner/export consumers and package tooling. New process cases cover dirty
snapshots, repeatable run output, explicit failing expectations, capability and
revision errors, output rejection and retained-owner publication failures.

| Selection | Result / evidence |
| --- | --- |
| Windows Debug seven affected targets | Build PASS; `windows-build-final.log`, `windows-output-path-build.log` |
| Windows Debug initial affected regression | 9/9 PASS, 25.47 s; `windows-tests.log`, `windows-debug.xml` |
| Final protocol/process/run/export controls | 4/4 PASS, 1.96 s; `windows-final-protocol.log/xml` |
| WSL Debug six affected CPU targets/regression | Build and 8/8 PASS, 5.13 s; `wsl/linux-debug-build.log`, `wsl/linux-debug-tests.log`, `wsl/linux-debug.xml` |
| WSL final structured-path fix/run process | Build and PASS, 0.75 s; `wsl/linux-final-build.log`, `wsl/linux-final-process.log/xml` |
| Windows ASan affected CPU owners | Build PASS; eight selected CTest families PASS without findings; `windows-asan-final-build.log`, `windows-asan-tests.log`, `windows-asan.xml`; optional consumer failure explained below |
| ASan native export-worker process selection | 6/6 PASS, 1.937 s; `windows-asan-export-worker-final.log` |
| Four new private source objects | ASan load checks present; `asan-new-objects.log` |
| Independent CPU Tools configuration | `tools-debug` SharkAuthor build and 2/2 process families PASS, 1.18 s; `tools-build.log`, `tools-startup.log/xml`, `tools-dependents.log` |
| Updated public create/edit/run example | PASS; `documented-example.log` |
| Actual terminal capture on RTX 4070 Laptop and Intel | Required validation, clean close and independent image/depth oracle PASS on both; `artifacts-final/summary.json`, 12.319 s including export |
| Actual retained-owner package export/CPU inspection | PASS, same dirty prepared scene as both captures; `artifacts-final/summary.json` and retained `artifacts-final/package` |
| Relocated package isolated launches on both GPUs | PASS; `isolated-final/summary.json`, `isolated-final-token.json`; initially empty external working directories, sanitized environment, validation off and observed module audit |
| DPI-aware package checker regression | PASS, 16.36 s; `package-checker-final.log/xml` |
| Final runner discovery-schema correction | Both hosts rebuilt; execution process PASS, 0.32 s; `schema-final-build.log`, `schema-final.log/xml` |
| Coverage audit and repository contracts | PASS; `coverage-final.log/xml`, `contract-final.log`, `repository-final.log` |

Primary commands (through the MSVC environment wrapper):

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset windows-debug --target SharkAuthor SharkAuthorClient AuthoringProtocolTests SceneRunnerTests ClientExportTests ExportContent RuntimeScenePrepare --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug --show-only
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R "^shark\.(tools\.(authoring-(protocol(-process)?|editing-process|import-process|execution-process)|scene-runner|client-export(-process)?)|tooling\.runtime-package)$" --output-on-failure --no-tests=error
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R "^shark\.tools\.authoring-(protocol(-process)?|execution-process|export-controls)$" --output-on-failure --no-tests=error
```

The actual artifact workflow uses `tools/tests/test_authoring_artifact_process.py`
through `tools/windows_unelevated.py`; its retained report records the binary,
shader generation, validation manifest, GPU UUIDs and exact output inputs. The
package manifest digest is
`cfa182e835a4db0efa8e1a1cc97be598c13b0bb955968344ceea1e13d0c12581`.
The deployment was copied outside the checkout through a Unicode path and removed
after successful checks; `deployment.json` records cleanup. Capture validation-on
evidence is separate from isolated validation-off package evidence. Neither claims
another machine or production capacity. The existing 14c default-startup/F5 proof
remains applicable to unchanged Runtime behavior; it was not rerun.

### Failures and resolutions

- The first compile found a namespace collision in UTF-8 validation; explicitly
  qualifying the shared Core helper fixed it. `windows-build.log` is retained.
  Final missing-output-parent checks found and fixed a filesystem exception path;
  the real artifact fixture now checks structured rejection and surviving history.
- The first artifact attempt selected an obsolete shader generation and was
  correctly rejected. The next reached capture but ordinary-user Runtime build
  freshness inspection could not read an administrator-owned project-local Mbed
  TLS cache. Granting the current user read/execute on that exact cache restored
  inspection without changing dependency bytes. Failed reports remain in
  `artifacts/` and `artifacts-current/`; `runtime-cache-access.log` and
  `runtime-freshness-current.log` record the correction. Final artifacts passed.
- One of nine ASan CTest families failed because its optional Runtime consumer
  was an old bootstrap executable lacking `--check-package`, not due to an ASan
  finding. The six affected native export-worker cases passed with that optional
  consumer omitted; actual current protected Runtime inspection/deployment passed
  separately. No ASan Runtime/Client qualification is claimed. Initial attempts
  without the MSVC DLL environment and with verbose stderr crossing the wrapper
  were corrected; their logs remain alongside the final worker result.
- The initial isolated launch rendered and closed cleanly but the checker assumed
  96 DPI. This PC uses 144 DPI: the unchanged 160x120 request produces 240x180
  physical pixels through GLFW's established scaling policy. The checker now
  independently observes the child HWND's DPI/physical client rectangle, restores
  its temporary thread DPI context, and requires the exact computed dimensions in
  completion and raw readbacks. Both 96/144-DPI positive and mismatch-negative
  controls pass. Two frames, extent limits and image tolerances are unchanged.
  The initial failure remains in `isolated/`; only its empty working directory
  and successful temporary deployment were cleaned.
- Final review aligned discovery and documentation with the existing runner's
  `[A-Za-z0-9._-]{1,64}` token rule for `run_id` and `revision`. The owning process
  family passed with explicit discovery assertions. Earlier platform, sanitizer,
  GPU and package results remain applicable to unchanged execution behavior;
  those selections were not repeated for schema metadata alone.

The testing policy selects one relevant sanitizer and affected consumers, not a
whole-project matrix. WSL uses the retained offline CPU build; no native Linux
graphics is claimed. No driver, host/WSL resource, kernel, compiler flag or pinned
dependency change was made. Protected Runtime/Server dependency inputs remain
unchanged. The tools-only cache disables Client/Editor/native graphics/window;
the resulting CPU host's DLL inventory contains no graphics dependency.

Pinned C++ formatting, coverage/repository/link/manifest checks and diff review
pass. Coverage maps 43 external CPU surfaces (42 terminal methods including
configured export plus the existing exporter) and three GPU surfaces; seventeen
FeatureWorld cases and 56 in-process CPU surfaces are unchanged. CPU capability
controls do not count as GPU evidence. `.gitignore` adds unpublished capture
stages while preserving source, fixtures, skills and documentation eligibility.

## Next work

15e qualifies a connected public subprocess workflow: create/edit/import/save/
reopen/run/capture/export, with explicit failure/retry/cancellation and ownership
handoff evidence. Reuse these valid focused results and existing native owners;
extend the real public fixture to connect missing steps rather than adding a
second mutation path. Publish a complete usable example and finish REQ-15
coverage. The human editor UI remains assignment 16.

Suggested commit: `feat(authoring): add retained scene runs exports and capture`.
