# 10c: Explicit CPU/GPU test supervision

Date: 2026-10-25. **10c is PASS under the current-machine scope.**
The user approved an Ubuntu restart; all 40 original WSL sanitizer checks now
pass. Qualification totals 315 logical CTest profile results, 30 public-example
compiles, four actual sanitizer objects and twelve real GPU runs with 48 readback
comparisons. Original incomplete attempts remain preserved.
Base: `a3e3597c334832243dae3662633ec499cdca50d5` on initially clean `main`.
Staging, commits and pushing belong to the user. Full REQ-10 remains NOT_RUN;
the next checkpoint after this gate is 10d, integrated coverage qualification.

Follow the [checkpoint plan](../roadmap/featureworld-plan.md),
[execution contract](../scene-execution.md) and
[ADR 0024](../architecture/0024-headless-scene-runner.md).
The [complete receipt](../coverage/evidence/10c-gpu-execution.json) closes this
gate. The [partial receipt](../coverage/evidence/10c-gpu-execution-partial.json)
retains the earlier NOT_RUN state and the evidence available at that time.
Recovery resumed from the user's `2532512` commit on clean `main`; these closing
documentation changes are uncommitted.

## Delivered behavior

`tools/scene_runner.py` supervises explicit CPU scene cases or the existing native
offscreen/window cube fixtures. CPU uses the existing SceneRunner worker and
shared AuthoredScene services; its native result/catalog contracts and protected
dependency graph remain unchanged. GPU uses existing EngineClient rendering,
process containment, validation tools and independent fixed readback oracles.
No dependency, alternate renderer, scheduler or authoring adapter is introduced.

The new version-1 execution envelope records requested mode/case/device, caller
context, tool/executable/shader identities, worker completion and bounded hashed
artifact references. A GPU wrapper label cannot establish PASS: native records,
actual readbacks, exact UUID/capabilities, required validation, source identity
and completed retirement are revalidated. Unavailable capabilities never select
another device, CPU case or validation setting. Incomplete GPU initialization
is explicitly unknown rather than falsely reported device-free.

The new tests exercise real noninteractive CPU processes and UTF-8 files, saved
FeatureWorld, failure/publication/capability controls and synthetic native evidence
mutations. Synthetic CPU fixtures qualify no GPU. Existing 10a/10b workloads,
timeouts, source IDs and nineteen scene mappings are retained. One real GPU
command entry has an explicit external fixture justification; authored scene
rendering stays in 13 and general terminal/editor authoring stays in 15/16.

## Completed evidence

Frozen inputs are in `out/10c-work/source-freeze.json` (374 files); no implementation
or acceptance limit changed during qualification. Native Debug, Release and ASan
each pass 47 selected CPU checks. Windows standalone Tools passes 17. The real GPU
supervisor passes all **12 runs**: two cases, both current GPUs, three profiles,
with **48 independent readback comparisons**, required clean validation and
completed owner retirement. The actual unavailable UUID control returns NOT_RUN
without fallback. NVIDIA/Intel window captures were visually inspected and match
the fixed numeric reference. The Release Intel window run also uses a Unicode
output root. See `out/10c-work/windows-continued/summary.json` and retained native
result/artifact files; the initial incomplete attempt remains separate.

The first ASan window request stopped before native dispatch because its adjacent
cube bundle was absent. Exporting the already verified offline generation supplied
that prerequisite; the remaining three GPU runs then passed. This was a real
NOT_RUN capability result, not a rendering pass or discarded failure.

The full WSL Debug run has 99 PASS and one boundary timeout. An isolated diagnostic
profile found repeated filesystem path/existence checks dominate. The same CTest
passed in 4.1 seconds at its unchanged 10-second timeout, comparable to 10b's 3.96s.
`out/10c-work/wsl-continued/linux-debug-reconciled.xml` explicitly combines those
results; original JUnit and diagnostics are preserved. Precise original transient
cause is not established. No workload, source or limit was relaxed.

Windows/WSL standalone Tools each pass 17 selected checks, including all sixteen
Tools families plus the synthetic evidence family. All ten public examples compile
on MSVC/GCC/Clang: **30 compiles**, with private-include audits. Protected native/
WSL Runtime and Server builds/packages, SDK-free startup and four actual sanitizer
objects pass. The native Runtime package was verified identical to the current
build and reused; its temporary Unicode relocation was removed. No additional
native package copy or new userspace/build tree was retained.

New CPU controls retain **21 real process checks** and **14 synthetic evidence
tests**, with two 90-second CTest families. Existing thirteen runner cases,
29 runner process checks, five FeatureWorld cases, 23 FeatureWorld process checks,
nineteen CPU scene mappings and earlier bounds/seeds/timeouts remain unchanged.
Contract self-tests pass 45 negative controls. Full REQ-10 remains NOT_RUN.

## WSL kernel stall and recovery boundary

The WSL Clang ASan/UBSan selection passed the boundary, FeatureWorld, scene-runner
and source-document checks, then the unchanged `AuthoredSceneTests` process
became uninterruptible. PID 612 reports `D` state at `__vma_start_write`, with
`SIGKILL` already pending. Its stack is `mmap -> vma_expand -> __vma_start_write`;
no userspace engine assertion or sanitizer diagnostic was produced. Ordinary
process inspection also stalls on this process; `/proc`-only diagnostics work.
The running kernel is `6.18.33.2-microsoft-standard-WSL2`.

The kernel/version/behavior strongly match the open upstream
[WSL issue 41708](https://github.com/microsoft/WSL/issues/41708). This is an inference,
not a confirmed kernel fix. Raw status/stack/syscall and the interpretation are
preserved in `out/10c-work/wsl-kernel-stall`. Owned stalled diagnostic readers and
test coordinators were stopped; the victim's pending SIGKILL cannot complete
inside this kernel wait. No host/WSL resource settings, drivers or kernel versions
were changed during investigation.

After the user explicitly approved closing Ubuntu shells/processes,
`wsl.exe --terminate Ubuntu` completed successfully. Ubuntu started again with
the old victim absent. The kernel version and boot ID remained unchanged; this
was a distribution restart, with no `wsl --shutdown`, kernel/driver change or
resource setting change. `out/10c-work/restart-recovery.json` records the action.
The same retained build, frozen inputs, fatal ASan/UBSan options and exact
40-test inventory/limits then passed, including all earlier runner and
FeatureWorld cases, process controls and synthetic evidence checks. Original
stalled evidence remains separate in `wsl-kernel-stall` and `wsl-continued`.
The completed recovery is in `out/10c-work/wsl-recovered/summary.json`.
WSL independent standalone Tools and Clang public examples had already completed
through a separate owned command. Next: 10d integrated coverage qualification.

## Storage and reproduction

Use current build profiles and existing dependency inputs. The boundary's 27 MB
raw diagnostic trace was verified in `out/10c-work/boundary-trace.zip` (0.95 MB),
then only that exact hash-matching raw file was removed. Its original path/hash
and archive receipt are preserved in `boundary-trace-retention.json`; restore
through the archive index when raw inspection is needed.

Completed commands include the ignored local qualification/continuation helpers
under `out/10c-work`; they are not checkout/build prerequisites. Ordinary focused
checks are:

```text
ctest --preset windows-debug -R ^shark.tools.scene-execution$
ctest --preset windows-debug -R ^shark.tooling.scene-execution-evidence$
python tools/validate_contract.py --self-test
wsl.exe -d Ubuntu -u root --cd / -- /bin/bash /mnt/c/Users/mrkda/Projects/shark/out/10c-work/recover-wsl.sh
python out/10c-work/receipt-recovered.py
```

The [execution guide](../scene-execution.md) provides direct CPU/GPU commands.
Preserve failed/incomplete evidence and final-result publication semantics.
Suggested commit: `feat(tools): supervise explicit CPU and GPU test execution`.
