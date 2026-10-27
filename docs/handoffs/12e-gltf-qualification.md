# 12e: Integrated glTF qualification

Date: 2026-10-27. Base: `46cce58`, initially clean `main`.
**12e and full REQ-12 are PASS under current-machine scope.**
Next: assignment 13, basic scene rendering and cameras; plan its checkpoints
before implementation. See the [results](../coverage/evidence/12e-gltf-qualification.json).
The user owns staging, commits and pushing.

Follow the [qualification contract](../gltf-qualification.md),
[checkpoint plan](../roadmap/gltf-plan.md) and [testing policy](../testing-policy.md).

## Delivered

Four additional cases extend the existing glTF import family from fifteen to
nineteen without changing its 90-second limit: saved external/embedded/GLB strips
through cache reopen; repaired sparse indices followed by failed async replacement;
textured GLB semantic mip values; and usable optional fallback followed by
required-extension/missing-image failures with retained valid output.

The existing asset-job process probe/supervisor now also exercises actual glTF
payloads. `shark.tools.gltf-process` has a 90-second limit, 50 checks, sixteen
children and three abrupt-exit phases: ready, before publication and published.
It verifies literal decoded geometry, mirrored/nonuniform bounds, BLEND material
values, typed external-resource identity/digest, changing dependency/root keys,
atomic cache bytes, foreign job rejection, orphan recovery and reproducibility.
All three final snapshots have identical bytes and root/dependency keys within
each profile. The original 59 synchronous and 113 asynchronous byte checks remain.

The pinned, opt-in [Khronos oracle](../dependencies/gltf-validator.md) passes twelve
licensed positive containers and three named invalid controls. Normal CTests
remain offline and do not require Node. The initial oracle found missing required
POSITION bounds in three texture containers. Their generator/metadata/manifest now
include the exact bounds; geometry/image bytes, independent pixel expectations and
workloads are unchanged. Original oracle failures remain in `out/12e-work/oracle/`;
final reports are under its `final/` directory. Generated-tangent warnings remain
visible and agree with the documented tangent-generation policy.

The broader native Debug run also caught an older supervisor expectation of twelve
FeatureWorld cases. The saved suite itself passed; updating that independent count
to thirteen fixed `shark.tools.scene-execution`. Only the failed check was rerun.
Its original failed batch and passing retry remain recorded. Thirteen native cases,
four saved entity IDs, 31 process checks, 47 CPU surfaces and one separate GPU
command are preserved. Coverage maps the new process test to the existing material
and asset APIs; no new public surface or native FeatureWorld case is needed.

Engine implementations, public APIs, payload version, dependency lock, install
rules and graphics inputs are unchanged. No additional owner, worker, file resolver,
renderer or authoring adapter is introduced. `.gitignore` was reviewed: existing
`build/`, `out/`, cache and ignored local-roadmap rules cover all generated inputs.

## Verification

The selected assignment-closure checks follow the testing policy. Full Debug
inventories are combined with targeted Release/sanitizer and standalone Tools
coverage; no recurring seven-profile matrix or source-freeze framework was added.

| Profile | Passing results | Scope |
| --- | ---: | --- |
| Windows MSVC Debug | 123 | Full 123-test inventory; one corrected supervisor expectation |
| WSL GCC Debug | 116 | Full 116-test inventory split to schedule heavy dependency checks separately |
| Windows MSVC Release | 7 | Six glTF families and actual glTF process recovery |
| WSL GCC standalone Tools Debug | 8 | Six glTF families, process recovery and FeatureWorld |
| WSL Clang ASan/UBSan | 8 | Same affected eight-test selection |

These are **262 passing profile-test results**, not 262 new unique tests.
There were 266 CTest executions including the three development repeats and the
one resolved failed attempt. The native full run took 975.95 seconds; the WSL
Debug partition took 413 seconds plus 31.82 seconds for its boundary. Release,
standalone Tools and sanitizer selections took 4.79, 4.92 and 23.06 seconds.
Contract/repository checks, fixture integrity, changed C++ formatting and diff
review pass after the final documentation update.
The initial three-test development selection and the failed check's retry are
additional executions, not added coverage. All original limits/seeds remain.
The six glTF families retain 41/45/43/14/12 source/geometry/texture/material/codec
cases and nineteen owner cases; async polling makes assertion totals variable.

Native commands use `tools/windows_dev.ps1` and canonical `build/windows-debug`
and `build/windows-release`; build logs, inventory and results are under
`out/12e-work/`. `native-debug.xml` preserves the original full run;
`scene-execution-fixed.xml` is its single passing correction. `native-release.xml`
records the seven-test selection. CTest initially writes these relative JUnit paths
under the preset build directory; completed reports are copied to the evidence root.

WSL reuses `/work/build-05a-final-client/linux-debug`,
`/work/build-05a-final-client/linux-asan-ubsan` and `/work/build-09a-tools` in the
retained userspace. The three `wsl-*-commands.sh` files record ordinary build/test
commands; `wsl/debug.xml`, `debug-boundary.xml`, `tools.xml` and `sanitizer.xml`
record results. Their adjacent driver logs retain output. Mounts are private;
no host/WSL resource or kernel settings changed. Process reports are retained for
each selected profile alongside these logs.

Protected packages also pass. Native `runtime-only` configure/build excludes
Tools/JSON/cgltf/stb. The 35-file Runtime package passes inspection and standard-user
`--version`/`--help` after Unicode relocation with system-only PATH. WSL `server-only`
configure/build, twelve-file source/build/ELF/license closure and SDK-free,
network-isolated version/help/bounded startup all pass. Details are in
`out/12e-work/packages/summary.json`, the native manifest and `wsl/server-closure.json`
plus `wsl/sdk-free.json`. The WSL deployment used namespace-local tmpfs, released
on exit; canonical builds remain reusable.

Public C++ examples are unchanged: reuse 12a-12c's recorded public-header compiler
checks and 12d's MSVC material/asset example at `46cce58`; they are not counted as
fresh compiles. No extra dedicated Server matrices, Windows ASan/object audits or
GPU runs were scheduled: those implementation/instrumentation/graphics inputs did
not change. Relevant Release allocation/error paths and WSL sanitizer behavior are
fresh. The unchanged 12c shader bundle supports the protected Runtime package.
Rendering, cooked distribution, animation and terminal/editor adapters retain their
later assignments; this is bounded CPU import qualification, not a playable editor.

## Retained temporary files

Automatic approval review rejected verified native cleanup as "blocked by policy"
without a detailed reason. No deletion retry was attempted. Two 2.97 MiB copies
remain (about 6 MiB total):

- `out/12e-work/packages/runtime-package`
- `C:\Users\mrkda\AppData\Local\Temp\shark-12e-package-8289574a230e45b0a5ac0251322bd535`

This is a housekeeping limitation; package closure and isolated launches passed.
No source, canonical build, archive cache or historical evidence was removed.

Suggested commit: `test(tools): qualify static glTF import pipeline`.
