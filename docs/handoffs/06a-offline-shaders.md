# 06a handoff: Offline shader tooling

Date: 2026-10-18. Base: `db4117f` on initially clean `main`.
Result: uncommitted worktree changes; the user owns staging, commits and pushes.
Status: **06a PASS for offline tooling only.** Full `REQ-05` and `REQ-06` remain
NOT_RUN. Durable results are in the [06a receipt](../coverage/evidence/06a-offline-shaders.json).

The user's instruction to proceed with the next subchunk starts CPU/offline 06a
under the [sequencing decision](../roadmap/shader-resource-plan.md). Assignment
05c remains open, with all failed evidence and fixed budgets preserved. The
[prior investigation](05c-intel-asan-investigation.md) remains authoritative for
NVIDIA desktop-DC retention, Intel hook retention and the unexplained original
Intel ASan process-chain timeout. This work does not change graphics drivers or
host settings.

The [shader contract](../shader-pipeline.md) describes the implemented interface.
Full `REQ-06` remains NOT_RUN: GPU loading, resources, pipelines, uploads, window/
offscreen cube readback and final distribution acceptance are subsequent work.
06b requires 05c closure or a subsequent explicit sequencing decision.

## Delivered behavior

- `tools/shader_pipeline.py` builds strict version-1 recipes with the existing
  pinned, cached DXC and independently validates SPIR-V. It snapshots a bounded
  literal-include closure, hashes source/tool/convention inputs, verifies cache
  hits and publishes a complete immutable generation through an atomic manifest
  replacement. Conflicting writers and source/tool changes fail explicitly.
- `shaders/` supplies textured-cube vertex/pixel sources with explicit column-major
  matrices and set/binding conventions. These are compiled building blocks; no
  cube is rendered or loaded by Runtime in this checkpoint.
- The source/tool-free `verify` command checks the selected generation's strict
  manifest, inventory, hashes and basic SPIR-V stage/binding contract. It is not a
  signature verifier, full reflection system or packaged-game loader.
- `tools/tests/test_shader_pipeline.py` provides 22 CPU cases, registered as
  `shark.tooling.shader-pipeline` (`cpu;tooling`, 30 seconds). The explicit
  `tools/tests/check_shader_pipeline.py` fixture exercises real vertex/pixel/
  compute compilation and output failures without a GPU.
- Active plans, requirements, build/CI guidance and `.gitignore` document the
  sequencing exception and source/generated-file boundaries. The local ignored
  roadmap carries the same note. No dependency pins, C++ sources, engine target
  relationships or graphics drivers changed. Git staging/commits/pushes were not
  performed.

## Verification

Windows host: Windows 11 x64 `10.0.26200`, Python 3.13.5, CMake 3.31.10,
Ninja 1.13.2, Visual Studio 18 / VC tools 14.51.36231, SDK 10.0.28000.0.
Real offline compiler: locked DXC 1.9.2609 (reported 1.9.2609.5,
revision `01b62ad47`); validator: SPIRV-Tools v2026.2,
revision `18b08af19b08515d5a1749adbdb05bc4369a57ed`. Exact file hashes and
commands are retained in the receipt.

| Check | Result and scope |
| --- | --- |
| Windows Debug configure/build and complete CPU CTest | **44/44 PASS**, zero skipped/disabled, on unchanged final implementation inputs. |
| Real offline shader integration | **11 checks PASS**, 17 pipeline commands plus two actual tool version queries. Independent cold builds match exactly; cache hits, nested-include edits, unrelated edits, malformed HLSL, missing includes, unsupported versions, descriptor-number mismatch, corrupt/missing artifacts and relocated verification are exercised. Paths include spaces and Unicode. |
| Checked-in cube shader outputs | Vertex 1,064 bytes; pixel 780 bytes; both independently validated. Separate storage-buffer compute fixture is 608 bytes. Compilation only, no draw/dispatch. |
| WSL Ubuntu CPU tooling | Initial 81 Python tests pass (22 shader, 18 graphics-tool, 27 Server closure, 14 Runtime-package), plus 19 boundary cases (3 accepted, 16 rejected). After the Unicode invocation fix, the final 22 shader tests pass again with frozen source hashes. |
| WSL isolation | Pinned Ubuntu 24.04.5 / Python 3.12.3; source bind read-only, no GPU/display devices, package inventory unchanged, private mounts removed on exit. No Linux shader-tool integration or graphics execution claimed. |
| Windows Runtime-only / Server-only | Configure/build and `--version` pass; existing ALL boundary targets pass. Generated build graphs contain no shader pipeline/compiler/validator dependency, and caches do not discover Python. |

Final repository checks pass: pinned formatting for 85 C/C++ files, contract
validation with 29 negative checks and all document links, repository/ignore
validation with three negative skill cases, and `git diff --check`. These are
structural checks, separate from shader compilation and hardware qualification.

The main reproducible commands, run from the repository root, are:

```powershell
& tools/windows_dev.ps1 -CommandArguments @('cmake', '--preset', 'windows-debug')
& tools/windows_dev.ps1 -CommandArguments @('cmake', '--build', '--preset', 'windows-debug', '--parallel', '2')
& tools/windows_dev.ps1 -CommandArguments @('ctest', '--preset', 'windows-debug', '--show-only')
& tools/windows_dev.ps1 -CommandArguments @('ctest', '--preset', 'windows-debug', '--output-on-failure', '--no-tests=error')
.tools/venv/Scripts/python.exe tools/tests/check_shader_pipeline.py --validation-manifest out/05a-work/validation-tools/summary.json --recipe shaders/recipe.json --output out/06a-work/offline-integration-04
& tools/windows_dev.ps1 -CommandArguments @('cmake', '--preset', 'runtime-only')
& tools/windows_dev.ps1 -CommandArguments @('cmake', '--build', '--preset', 'runtime-only', '--parallel', '2')
& tools/windows_dev.ps1 -CommandArguments @('cmake', '--preset', 'server-only')
& tools/windows_dev.ps1 -CommandArguments @('cmake', '--build', '--preset', 'server-only', '--parallel', '2')
```

Use a new integration output directory when repeating the command. The local
validation receipt above was already current; no tool download or rebuild was
needed. The exact evidence wrappers also pin Python and save JUnit/toolchain/log
identities. Their invocations were:

```text
tools/windows_dev.ps1 python out/06a-work/verify-windows-cpu-final.py
tools/windows_dev.ps1 python out/06a-work/verify-windows-boundaries.py
wsl.exe -d Ubuntu -u root -- bash /mnt/c/Users/mrkda/Projects/shark/out/06a-work/run-wsl-shaders.sh
wsl.exe -d Ubuntu -u root -- bash /mnt/c/Users/mrkda/Projects/shark/out/06a-work/run-wsl-shaders-final.sh
```

These ignored WSL helpers use the existing pinned userspace and an ephemeral
private mount namespace. They do not install packages or alter host resources.
The versioned Python suites remain the maintained entry points.

## Preserved failures and corrections

1. `offline-integration-01` selected an old assignment-02 validation receipt.
   The builder rejected its stale lock identity. The dependency lock was unchanged;
   the rerun used the already-current assignment-05 receipt.
2. `offline-integration-02` exposed the pinned Windows validator's handling of a
   non-ASCII absolute artifact argument: it reported a nonexistent filename after
   DXC had produced the file. The builder now selects the artifact directory as
   working directory through the Unicode process API and passes its ASCII artifact
   basename. The successful real run retains Unicode source/output paths.
3. The first Windows CTest run passed 44/44, but its evidence wrapper deliberately
   reported FAIL because the Unicode correction changed inputs during that run.
   `windows-cpu-final` repeats the full suite on frozen inputs and passes. The
   preliminary receipt remains intact. The initial WSL pass is supplemented by
   the final shader-only rerun; unrelated passing suites were not repeated.
4. `offline-integration-03` passed the vertex/pixel workload. The final `-04`
   fixture adds real compute-stage coverage and reruns the complete integration.

## Limits and next work

No Windows Release/ASan or Linux C++ sanitizer rebuild was needed: this increment
changes Python tooling, shader sources, test registration and documentation only.
No native GPU stress, rendering/readback, Runtime shader loading or game package
was exercised. The standalone artifact verification is distinct from the future
compiler-free Runtime package gate. Historical 05c failures are unchanged.

The supported HLSL preprocessor subset is deliberately small and documented;
descriptor types, array/member strides, vertex inputs and cross-stage interfaces
are not fully reflected. There is no asset cooker, shader hot reload, material
system or game-authoring command. `tooling.offline-shaders` maps the actual tests
and justified external fixtures in coverage; command/component/Lua surfaces stay
empty. FeatureWorld connects applicable rendering fixtures in assignment 10.

The next planned implementation is **06b, owned GPU resources and offscreen cube
rendering**, under the [06 plan](../roadmap/shader-resource-plan.md). Its GPU work
still requires 05c closure or a subsequent explicit sequencing decision. This
handoff makes no decision to waive those remaining failures.

Suggested commit: `feat(shaders): add reproducible offline shader pipeline`
