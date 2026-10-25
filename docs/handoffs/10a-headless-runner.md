# 10a: Deterministic CPU scene-runner foundation

Date: 2026-10-25. **10a is PASS under the current-machine scope.**
Base: `7f5353a82384b8091580409d3d1d1f7779256a74` on initially clean `main`.
Result: uncommitted working tree; staging, commits and pushing belong to the user.
Follow the [four checkpoints](../roadmap/featureworld-plan.md),
[ADR 0024](../architecture/0024-headless-scene-runner.md),
[runner contract](../scene-runner.md) and
[qualification receipt](../coverage/evidence/10a-headless-runner.json).
**Next: 10b, saved FeatureWorld and current-surface cases. Full REQ-10 remains NOT_RUN.**

## Delivered behavior

`SceneRunner` is a noninteractive CPU executable built with EngineTools, including
standalone Tools with Client/Editor/Server disabled. The public `run_scene_cases`
service executes bounded trusted native cases through existing AuthoredScene
owners and Core scheduling. Setup/tick/finish share one owner; each subsequent
case starts from the same validated source baseline in a fresh owner.

Rates are integral 1..1000 Hz with an exact rational timestep, default 60 Hz.
Ordered one-shot integer inputs are delivered exactly once. SplitMix64-v1 uses
an explicit 64-bit seed and per-case reset without changing production entropy.
Assertions are sticky: ignoring a returned failure cannot make a case pass.
Callback errors/exceptions are recorded with phase, tick, entity and owned error
context. Failed work is not retried. Cancellation is cooperative between native
callbacks; an external process supervisor owns hard deadlines.

Version-1 `shark.scene-run` JSON records engine/source versions, reproducibility
context, IDs, local revisions, counters and artifact references. Result files use
unique sibling stages and atomic create-only hard links; existing paths are
preserved. Unicode paths, concurrent writers and failed publication are tested.
The real foundation fixture creates and edits a scene entity through shared
commands. The intentional scene-assertion fixture exits 1 and publishes a useful
FAIL record. Unsupported GPU requests return a capability error with no execution.

The boundary checker now audits SceneRunner as an authoring root and rejects
Client/storage dependencies. JSON remains private to EngineTools. Existing Core,
Client, Runtime, Server and durable-state implementations are unchanged.
The saved FeatureWorld/catalog, real GPU mode and full coverage audit retain
10b, 10c and 10d. Human/editor authoring adapters retain 15/16; this is a test CLI.

## Qualification

The current candidate has **190 qualified profile test results**, including all
thirteen Tools families on both OSes. The Linux Debug row combines 95 passing
tests from its full run with the repository check's successful environment retry;
both original receipts are preserved. An additional two sanitizer-policy checks
pass with explicit fatal ASan/UBSan runtime options.

| Profile | Registered | Qualified in this increment |
| --- | ---: | ---: |
| Windows Debug | 103 | 17 related Tools/timing/boundary/contract checks |
| Windows Release | 103 | 17 related checks |
| Windows ASan | 103 | 17 related checks |
| Windows standalone Tools | 68 | 13 Tools families |
| WSL GCC Debug | 96 | 96, with the documented repository retry |
| WSL Clang ASan/UBSan | 96 | 17 related checks |
| WSL standalone Tools | 68 | 13 Tools families |

All final selected checks pass without skips. The new Catch family records all
**13 base cases**, seed 1337. Its process family independently checks **29 real
executable outcomes** per profile. Both families retain 60-second timeouts. The
original thirty boundary violations remain alongside two new runner violations;
all three allowed configurations still pass. Twenty-eight JUnit negative controls
reject omitted, duplicate, failed and skipped results.

All nine public Tools examples compile on MSVC, GCC and Clang: **27 compiles**.
Compiler-reported include inventories exclude private implementation/dependency
headers. Actual sanitizer flags and symbols are audited in the runner library,
CLI and test objects on Windows ASan and WSL ASan/UBSan: six objects total.

Protected Windows/WSL Runtime-only and Server-only builds pass. The native Runtime
package contains 33 files and passes isolated Unicode relocation/version/help;
its temporary deployment is removed. The installed Linux Server passes source,
link, ELF and package closure and SDK-free startup. Read-only package mounts leave
no new installed package copy in the retained runtime userspace. Dedicated full
Server-suite evidence remains historical 09e; this increment changes no Core,
Storage or Server implementation. No GPU qualification is claimed here.

## Preparatory outcomes and reproduction

Initial compilation corrected the qualified UTF-8 helper namespace and a signed
test comparison. A first full Windows Debug candidate passed 103 CTests. Review
then identified that valid scene activation changes incarnation while retaining
its AuthoredScene owner. Result validation now permits that transition and tests
it explicitly, retaining same-incarnation revision checks. The initial snapshot
and interrupted preparatory Release attempt are preserved separately. Current
qualification refreshes every affected Tools family and shared boundary/timing
checks; the full current Linux Debug run retains broader regression evidence.

That Linux run's repository check rejected Git ownership for the read-only
`/source` bind. A process-local `safe.directory=/source` exception fixed only that
verification environment; its retry passed. No global Git/host/WSL settings, source
behavior, acceptance limits or earlier workloads were changed to hide a failure.

Representative local commands, from the repository root:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/10a-work/qualify-windows-current.py
wsl.exe -d Ubuntu -u root -- bash /mnt/c/Users/mrkda/Projects/shark/out/10a-work/wsl-continue.sh
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/10a-work/audit-windows.py
wsl.exe -d Ubuntu -u root -- bash /mnt/c/Users/mrkda/Projects/shark/out/10a-work/wsl-protected.sh
```

Local evidence destinations are create-only; preserve completed attempts before
rerunning. These ignored helpers are not checkout/build prerequisites. Ordinary
focused tests run with `ctest --preset windows-debug -R ^shark.tools.scene-runner`.
Existing build profiles and pinned archives were reused. Generated results and
compact receipts stay under `out/10a-work`; the interrupted test's tiny temporary
barrier files were preserved there rather than left outside the project.
