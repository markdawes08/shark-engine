# 14a: Client export content preparation

Date: 2026-10-28. Base: `5a8ed5c`, initially clean `main`.
**14a is PASS under current-machine scope.** Full REQ-14 remains NOT_RUN;
next is **14b, export command and package launch**, following the
[three export checkpoints](../roadmap/export-plan.md). Staging, commits and
pushing remain with the user.

## Delivered

CPU EngineTools now prepares the current project's startup scene and explicit
camera into an immutable, owned export candidate. It captures unsaved edits
without changing source bytes, dirty state, history, revisions or the cache.
Every renderer participates, including hidden references. Built-ins retain their
Core IDs; external meshes require fresh client-permitted typed glTF cache
artifacts. Shared dependencies are deduplicated, while the complete used import
closure contributes asset identity, origin and license attribution.

The service rejects missing/stale/corrupt/private inputs, incompatible owners or
revisions, invalid cameras/mesh indices and count/byte/work overflow. It uses
existing prepared scene/mesh formats and validates the complete Core load.
Limits include 126 external meshes, 128 source records, 32 MiB combined prepared
bytes and 512 dependency-node visits per freshness pass, counted separately for
each root. This caps repeated source-read work at 512 MiB per pass; preparation
performs two passes. It is not a retained-memory allowance.

New public contracts are `ClientExportRequest`, immutable `PreparedClientExport`,
`prepare_client_export`, `validate_client_export` and the narrow
`AssetImports::check_context` operation. The last operation checks project,
catalog/cache revisions and disk/cache ownership even for built-in-only scenes.
Validation repeats the original observation fences before later publication;
cooperating writers must coordinate the final validation/publication interval.
Owned bytes survive source-owner destruction as a historical snapshot. See
[ADR 0037](../architecture/0037-client-export-content.md) and the
[public contract/example](../client-export.md).

Nine focused native cases cover real imports, shared dependency closure,
freshness/privacy, hidden references, nonmutation, lifetime, owner/revision
fences and limits. FeatureWorld adds `featureworld.client-export` with independent
saved hierarchy, global anchor, identity and content expectations. Its original
16 cases and saved IDs remain intact. Coverage now maps 17 cases and 56 CPU
surfaces, plus the unchanged two external GPU commands. The new command group is
`content.client-export`; no Lua or wire format was introduced.

## Selected verification

Selection follows the [testing policy](../testing-policy.md). Logs and standard
JUnit reports are under `out/14a-work/`; inventories were listed before execution.
All native cases retain seed 1337 and existing timeouts. The new family has a
60-second limit and `cpu;unit;tooling;integration` labels.

| Profile / check | Result and retained evidence |
| --- | --- |
| Windows Debug build | PASS: ClientExportTests, PreparedContentTests, GltfImportTests, AssetImportTests, AssetImportJobsTests, FeatureWorldTests and SceneRunner; `build-debug.log` |
| Windows Debug affected regression | Seven families PASS initially; FeatureWorld process initially FAIL, then PASS after investigation below. `windows-tests.log`, `windows-debug.xml`, `featureworld-process-diagnostic-rerun.log` |
| Windows standalone CPU Tools | New export family PASS, 1.13 s wall time; Client/Editor/native graphics OFF. `tools-only-build.log`, `tools-only-tests.log`, `tools-only.xml` |
| Retained WSL Debug | Export, prepared-content and FeatureWorld PASS, 3/3, 1.35 s. `wsl/debug-tests.log`, `wsl/debug.xml` |
| Retained WSL ASan/UBSan | Same selection PASS, 3/3, 2.93 s, leak detection and halt-on-error enabled. `wsl/sanitizer-tests.log`, `wsl/sanitizer.xml` |
| Public C++ example | PASS with public headers and strict MSVC warnings; `public-example.cpp`, `public-example.log` |
| Repository / formatting | Contract/link/manifest and repository checks PASS; pinned clang-format dry run and diff whitespace checks PASS |

Primary commands, through the required x64 MSVC wrapper:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset windows-debug --target ClientExportTests PreparedContentTests GltfImportTests AssetImportTests AssetImportJobsTests FeatureWorldTests SceneRunner --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug --show-only
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R '^shark\.tools\.(client-export|prepared-content|gltf-import|asset-import|asset-import-jobs|featureworld|featureworld-process|scene-qualification)$' --output-on-failure --no-tests=error --output-junit C:/Users/mrkda/Projects/shark/out/14a-work/windows-debug.xml
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R '^shark\.tools\.featureworld-process$' --output-on-failure --no-tests=error
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset tools-debug --target ClientExportTests --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset tools-debug -R '^shark\.tools\.client-export$' --output-on-failure --no-tests=error
.tools/venv/Scripts/python.exe tools/validate_contract.py
.tools/venv/Scripts/python.exe tools/check_repository.py
```

The retained WSL driver `out/14a-work/wsl-check.sh` runs `wsl-commands.sh` in the
existing isolated CPU environment. It builds ClientExportTests,
PreparedContentTests and FeatureWorldTests in the canonical `linux-debug` and
`linux-asan-ubsan` trees, lists each inventory, then selects
`^shark\.tools\.(client-export|prepared-content|featureworld)$`. No host or WSL
resource/kernel settings changed. Profile executions are not unique test counts.

### Preserved failure and investigation

The initial Windows selection had seven passes and one failure in 22.66 s.
`shark.tools.featureworld-process` failed when its individually launched,
existing `featureworld.render-components` child returned nonzero with empty
stdout/stderr. All 17 cases had already passed in three full-suite child runs.
The old helper omitted the failed exit code from its assertion/report, so that
original code cannot be reconstructed. Preserve
`featureworld-process-initial-failure.json` and the initial log/JUnit.

Review found no change to that case's implementation or a matching Windows
application/resource-exhaustion event. One isolated reproduction through the same
supervisor and Unicode project passed; evidence is in
`render-components-investigation/isolated-result.json`. The helper now records
the exit code, arguments, timeout/cleanup state and streams before asserting.
One diagnostic rerun passed all 35 checks and 17 cases in 5.43 s, at unchanged
limits; `featureworld-process-final.json` preserves its report. The original
failure remains unreproduced with unknown cause; no engine fix is claimed.

An earlier build invocation misspelled `AssetImportJobsTests` as
`AssetImportJobTests`; `build-initial.log` retains the rejected target selection.
The corrected build passed. No tolerance, workload or baseline was weakened.

## Limits and next work

This delivers CPU content preparation only. There is no export CLI, package
startup manifest, publication, isolated launch or new GPU qualification in 14a.
Materials remain temporary opaque unlit; client content targets identify cache
bindings, not binary platform compatibility. Server-private exports, final
cooking, authoring adapters and MMO release capacity remain their assigned owners.

Core/Client/Runtime/Server sources, dependency lock, global target graph and install
rules are unchanged. Existing 13e rendering/Runtime boundary evidence at the base
revision supports those unchanged areas; it is not a fresh 14a run. Standalone
Tools builds verify the affected CPU boundary. Unrelated Server, Release,
graphics, package, hosted CI and full-project matrices were not selected.
`.gitignore` was reviewed; current build/output/cache/local-roadmap rules suffice.
Historical handoffs and receipts were not edited.

14b should consume this candidate through a noninteractive structured command,
extend the existing Runtime packager, and add bounded package-relative startup
loading with explicit current-host restrictions. Stage complete prepared content,
scene shaders, Runtime libraries and notices, revalidate before atomic publication,
and return useful completion/failure artifact metadata. Keep CPU preparation free
of graphics initialization and Runtime free of Tools/importers. Reserve relocated
isolated package acceptance on both current GPUs for 14c.

Suggested commit: `feat(tools): prepare validated client export content`.
