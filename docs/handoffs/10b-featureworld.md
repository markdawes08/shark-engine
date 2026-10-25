# 10b: Saved FeatureWorld and current-surface cases

Date: 2026-10-25. **10b is PASS under the current-machine scope.**
Base: `9631d67ac858d76401cd4ee11c85743d2f434b64` on initially clean `main`.
Result: uncommitted working tree; staging, commits and pushing belong to the user.
**Next: 10c, explicit GPU execution. Full REQ-10 remains NOT_RUN until 10d.**

Follow the [checkpoint plan](../roadmap/featureworld-plan.md),
[FeatureWorld guide](../featureworld.md) and
[ADR 0024](../architecture/0024-headless-scene-runner.md) and
[qualification receipt](../coverage/evidence/10b-featureworld.json).

The saved source project and scene provide four stable authored entities and
five trusted native CPU cases through the existing runner/AuthoredScene owner.
They connect component/schema/query values, single commands/hierarchy, atomic
batches/preview, shared history, retained retries and real project-file saves and
reload. Independent source expectations and actual revision transitions reject
valid but unexpected fixture edits. All current component/command mappings resolve
through the real executable catalog and passing scene cases. Lua stays empty;
isolated network, durable-state, platform, fault and resource suites remain required.

The CLI loads an explicit project through ProjectFiles, holds its cooperating
owner and saves only into a unique child of an explicit scratch directory.
Input source bytes are preserved. Successful cleanup removes owned known names;
unexpected remnants remain inspectable. No new dependency, target-boundary
permission, source/result format or Core/Client/Server implementation is introduced.
The new public case catalog and factory are shared EngineTools services.

## Qualification

All **311 current-candidate CTest results** pass without failures or skips.
Both broad Debug runs include the original network/storage/process suites.
All fifteen Tools families pass on both OSes, including standalone CPU Tools.

| Profile | Registered | Executed |
| --- | ---: | ---: |
| Windows Debug | 105 | 105, full suite |
| Windows Release | 105 | 26 related Tools/scene/timing/boundary/contract checks |
| Windows ASan | 105 | 26 related checks |
| Windows standalone Tools | 70 | 15 Tools families |
| WSL GCC Debug | 98 | 98, full suite |
| WSL Clang ASan/UBSan | 98 | 26 related checks |
| WSL standalone Tools | 70 | 15 Tools families |

Every profile verifies five new Catch cases and 23 independent executable checks.
The process supervisor resolves all **19 component/command mappings**, checks the
real native catalog, repeats saved runs with exact semantic revision expectations,
selects every case individually, publishes PASS/FAIL artifacts and rejects invalid
versions, IDs, paths, options and source drift. Input source bytes and scratch
sentinels remain intact; owned case scratch directories are removed.

Both new families retain 60-second timeouts; Catch uses seed 1337. The original
thirteen runner cases, 29 process checks, timeouts, bounds and all earlier
authoring workloads remain unchanged. Twenty-eight JUnit negative controls reject
missing, duplicate, failed and skipped results. Contract self-tests pass 43 controls.

All ten public Tools examples compile on MSVC, GCC and Clang: **30 compiles**,
with compiler-reported include inventories excluding private dependencies.
Actual sanitizer flags and symbols are verified in the new case library, runner
CLI and new test objects on each OS: **six objects**. Linux tests use explicit
fatal ASan/UBSan runtime options, including leak detection; Windows uses fatal ASan.

Protected native/WSL Runtime-only and Server-only builds pass without acquiring
Tools/JSON. The native Runtime package has 33 payload files and passes isolated
Unicode relocation/version/help with system-only PATH; its managed deployment is
removed. WSL Server passes source/link/ELF/package closure and actual SDK-free
startup, alongside CPU Runtime. Read-only package mounts leave no deployment copy
in the retained runtime userspace. Dedicated full Server profiles were not rerun:
Core/Storage/Server implementation is unchanged and full Windows/WSL Debug refresh
their existing tests; dedicated full Server evidence remains historical 09e.
No GPU, scene-rendering, distributed MMO or production-capacity claim is made.

## Preparatory outcomes and reproduction

A new test initially used the wrong TypedId accessor; its compile was corrected.
The first process attempt caught a Windows path-key spelling error in the new
supervisor. The first GCC build rejected a borrow through a temporary reference
wrapper; naming the wrapper resolved the warning without suppression. Its source
snapshot/hash and FAIL receipt are preserved. The preparatory Windows session was
stopped before that source correction and is not qualification evidence. All final
runs use the corrected frozen inputs. No case, workload or acceptance limit was
removed to obtain a pass.

Representative commands, from the repository root:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/10b-work/qualify.py
wsl.exe -d Ubuntu -u root -- bash /mnt/c/Users/mrkda/Projects/shark/out/10b-work/wsl.sh
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/10b-work/audit-windows.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/10b-work/asan-windows.py
wsl.exe -d Ubuntu -u root -- bash /mnt/c/Users/mrkda/Projects/shark/out/10b-work/wsl-protected.sh
```

Local evidence destinations are create-only. Preserve completed attempts before
rerunning; these ignored helpers are not checkout/build prerequisites. Ordinary
focused execution uses `ctest --preset windows-debug -R ^shark.tools.featureworld`.
Build directories, pinned dependency archives and existing WSL userspaces were
reused. The process-local Git ownership exception for read-only `/source` and
sanitizer environment do not change host/global Git/WSL resource settings.

About 19 MiB of compact receipts/logs/public objects/package evidence is retained
under `out/10b-work`, with a verified hash index and source freeze. One zero-byte
ignored ProjectFiles coordination marker is explicitly classified in that freeze;
it is not a build or ordinary test input. No copied build environment was created.
Automatic approval review blocked deletion of the verified temporary native Runtime
package without a detailed reason. Its **3.1 MB** copy remains at
`out/10b-work/windows-audits/runtime-package`; no deletion was retried.

10c must preserve CPU Tools/SceneRunner independence while adding explicit actual
GPU execution using the available Windows devices and existing graphics fixtures.
Scene rendering retains 13; terminal/editor adapters and Lua retain 15/16/19.
