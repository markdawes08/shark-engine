# 14b: Standalone scene export and package launch

Date: 2026-10-28. Base: `4cf7863`, initially clean `main`.
**Implementation delivered; checkpoint closure is pending selected sanitizer
verification.** Full REQ-14 remains NOT_RUN. Do not advance to 14c until the
remaining gate below is closed or the user explicitly amends it. Staging,
commits and pushing remain with the user.

## Delivered behavior

`tools/package_runtime.py export` is a noninteractive Windows x64 command with
structured publication/error/artifact results. It reuses the existing protected
Release Runtime builder, binary inspection, shader verification, notice collection
and fresh sibling-stage publication. `ExportContent` is a CPU Tools worker that
holds ProjectFiles and the same scene/catalog/import owners through the final
rename. Before that rename it rechecks original project/scene file revisions,
14a's content candidate and shader bytes. The parent also checks full asset notice
inputs. Existing outputs and source files are preserved; timeout/EOF cleanup
releases the native owner. A failure after rename retains `published: true`.

The command requires explicit camera, content profile and sidecar inventory;
imports remain explicit prerequisites. Used asset license identifiers require
full UTF-8 notice inputs. Packages contain prepared scene/meshes, fixed scene
shaders, Runtime/DLLs, attribution/full notices and existing provenance. Exact
sizes, digests, inventory, count/byte bounds and Windows target restrictions are
checked. It exports saved state; the underlying 14a API still supports an existing
owner's unsaved candidate. No editor/terminal attachment protocol was added.

Core adds the bounded `PreparedStartup` / `SHKSTRT1` codec. Client loads and
decodes the same verified file snapshots, including shaders, before returning a
complete owned package candidate. Runtime accepts `--package PATH`, portable CPU
`--check-package PATH`, and no-argument launch from the descriptor beside its
actual executable. F5 reload replaces scene/camera/shaders together after full
validation and bounded GPU drain. Source importers, Tools and JSON remain absent
from Runtime. The existing explicit scene and diagnostic paths remain available.

Actual Release closure inspection exposed `msvcp140_2.dll`, required by scene
math's three-argument hypot functions. Its exact installed Release bytes, version
and valid Microsoft signature are pinned in `WindowsCRT.runtime_additional_files`.
Runtime now collects four CRT DLLs; the graphics probe keeps its original three.
No compiler flags, source math or dependency exclusions were weakened. The lock
change required a freshly verified validator receipt and scene shader generation;
historical receipts remain unchanged.

Contracts: [ADR 0038](../architecture/0038-standalone-scene-export.md),
[standalone export](../standalone-export.md), [14a content](../client-export.md).
New public surfaces are the Core startup codec, Client package loader and the
export/package command group. Coverage maps focused codec/loader tests and the
separate real-process export fixture; FeatureWorld retains its 17 cases and 56
in-process surfaces, with one additional external CPU export command. The existing
Runtime preparer now seeds the imported source, metadata and actual fresh cache
while verifying that its prior prepared geometry is unchanged.

## Selected verification

Follow the [testing policy](../testing-policy.md). Evidence is under
`out/14b-work/`; inventories were listed before CTest selections. This is not a
full-project run. Existing seeds, workloads and limits are unchanged.

| Selection | Result / evidence |
| --- | --- |
| Windows Debug affected targets | PASS after the compiler investigation below; `build-debug-continued.log` |
| Windows Debug Core startup, Client package/content/launch/shader bundle, Runtime CLI, Tools export/process/preparer | 9/9 PASS, 3.77 s; `windows-tests.log`, `windows-debug.xml` |
| Final Windows export process and coverage audit | 2/2 PASS, 3.89 s; `windows-process-final.log`, `windows-process-final.xml`. The process rerun covers its writable fixture copy for read-only WSL checkouts and final parent changes. |
| Windows standalone CPU Tools | ExportContent, RuntimeScenePrepare and PreparedStartupTests build PASS; 2/2 startup/process tests PASS, 1.73 s; `tools-build.log`, `tools-tests.log`, `tools-debug.xml` |
| WSL Debug portable selection | Core startup, Client package/launch/shader bundle and actual export process PASS, 5/5, 2.23 s; `wsl/linux-debug-tests.log`, `wsl/linux-debug.xml` |
| Package tooling | 28 Runtime package checks PASS, 5.713 s; 16 shared graphics-package checks PASS, 0.249 s. `package-tooling-final.log`, `graphics-package-tooling.log` |
| Protected Release Runtime | Build/boundary PASS; `runtime-build.log`, `runtime-lock-build.log` |
| Real imported-scene export | PASS, 42 files; `export-final.json`. Exact package inventory inspection and packaged Runtime CPU inspection PASS in `package-inspection.log`, `runtime-inspection.json`. |
| One actual package presentation | RTX 4070 Laptop, UUID `863d6f59a51894ab8b4829aa9460c78d`, required Vulkan/NVRHI validation, 160x120, two frames. Existing imported-scene oracle checks 18,640 pixels: zero color/depth error, no validation diagnostics, drained owners. `rtx-package-smoke/summary.json`, `package-smoke-token.json` |
| Windows ASan Core startup only | 1/1 PASS, 0.08 s; `startup-asan-build.log`, `startup-asan-tests.log`, `startup-asan.xml`. This is narrower than the intended sanitizer selection. |
| Client/worker sanitizer selection | NOT_RUN: WSL ASan/UBSan and Windows ASan builds encountered compiler-process failures before these executable checks. Details below. |

Primary Windows commands use the required wrapper:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset windows-debug --target ExportContent PreparedStartupTests Tests Runtime RuntimeScenePrepare ClientExportTests FeatureWorldTests SceneRunner --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug --show-only
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R '^shark\.(core\.prepared-startup|client\.(prepared-package|scene-content|scene-launch|shader-bundle)|tools\.(client-export|client-export-process|runtime-scene-prepare)|runtime\.cli)$' --output-on-failure --no-tests=error
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R '^shark\.tools\.(client-export-process|scene-qualification)$' --output-on-failure --no-tests=error
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset runtime-only --target Runtime --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset tools-debug --target ExportContent RuntimeScenePrepare PreparedStartupTests --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset tools-debug -R '^shark\.(core\.prepared-startup|tools\.client-export-process)$' --output-on-failure --no-tests=error
```

The retained WSL driver `wsl-check.sh` builds the same relevant targets in canonical
`linux-debug` / `linux-asan-ubsan` trees and selects
`^shark\.(core\.prepared-startup|client\.(prepared-package|scene-launch|shader-bundle)|tools\.client-export-process)$`.
No host/WSL resource, kernel, toolchain or driver settings changed. Native GPU
execution used the existing standard-user wrapper, validator tools and imported
image oracle. The package remains under `out/14b-work/imported-export`; its
`bin/Runtime.exe` can be opened to view the imported scene. This is a local static
unlit viewer, not a game-authoring editor or MMO release.

## Failures and remaining gate

The initial Debug compile crashed inside MSVC while parsing a GLM header for
`prepared_startup.cpp`. One isolated compilation of the unchanged file and the
resumed build passed. Preserve `build-debug.log` and `compiler-isolation.log`;
no engine source fix is claimed for that crash.

The first real export rejected an unpackaged `msvcp140_2.dll` import. The failed
result is preserved in `export.json`; `runtime-imports.log` identifies the symbols.
The exact lock/closure correction above resolves that failure. Signature/version/
hash evidence is `runtime-additional-crt.json`. Validator tools were reverified
in the canonical build, then scene shaders were rebuilt with the final lock;
their SPIR-V bytes remain identical. `validation-tools/summary.json` and
`shader-build.log` record those successful operations.

WSL Clang 18 crashed in unrelated parser/code-generation phases while compiling
`shader_bundle.cpp`, `scene_launch.cpp` and the new export worker. The first two
unchanged isolated compiles passed; the worker crashed again in isolation.
The existing Windows ASan alternative also hit MSVC internal errors in unchanged
`gltf_asset.cpp` (including an isolated attempt) and then an unchanged Core test
while building the Client test executable. These are compiler failures, not
reported engine sanitizer defects. No underlying cause was established. Read-only
compiler-package checks found no changed compiler libraries; inspected Windows
logs had no matching WHEA event. Neither observation rules out an environment issue.

Preserve `wsl-launch.log`, `wsl-isolation-launch.log`, `wsl-worker-launch.log`,
the Clang repro files in `wsl-isolation/compiler-repro`,
`windows-asan-build.log`, `windows-asan-isolation.log` and
`windows-asan-core-client-build.log`. Repeated whole builds were stopped. Only the
new Core startup family obtained an ASan result. The remaining selected Client
package/launch/shader-bundle and native export-process sanitizer checks must be
closed or explicitly deferred by the user before advancing to 14c.

## Boundaries and next work

The [export plan](../roadmap/export-plan.md) retains 14c's relocated, different-CWD,
isolated-environment launch and actual module inspection on both GPUs. The one
14b package smoke is not that acceptance. Intel package execution, default-launch
interaction, isolated package failure controls and connected package reload remain
for that gate. Earlier 13e renderer/lifetime results support unchanged rendering
internals; they are not new package qualifications.

Server-only behavior, native Linux graphics, hosted CI and unrelated subsystem
matrices were not selected. Server dependency entries and native graphics sources
are unchanged. Runtime exclusion and standalone CPU Tools builds pass. The
`.gitignore` review adds only the two generated package/export staging prefixes;
outputs remain under ignored `out/` and canonical builds. Historical handoffs and
receipts are unchanged. Final `tools/validate_contract.py`,
`tools/check_repository.py`, pinned clang-format dry run and `git diff --check`
all pass. No staging, commits or pushes were performed.

Suggested commit: `feat(export): publish standalone scene packages and load startup content`.
