# 03c - Time, bounded work and final Core qualification

Date: 2026-10-18. Base: `2676b2b` on `main`, initially clean.
Result: uncommitted worktree changes; the user owns staging, commits and pushes.

**03c and full `REQ-03` are PASS under the current-machine scope.** The
[Core plan](../roadmap/core-plan.md) is complete across 03a/03b/03c. The exact next
assignment is **04, window, raw input and frame scheduling integration**, reusing
these Core policies. No assignment 04 implementation is started here.

## Delivered behavior and public contracts

The [time/work guide](../time-and-work.md) specifies the APIs and callback rules.
`time.hpp/.cpp` adds an injectable monotonic clock, a native steady-clock adapter,
and separate uniquely owned server/offline schedulers. Server timing defaults to
20 Hz with at most 8 callbacks per pass and 200 retained pending ticks; these are
validated provisional policy values, not a capacity claim. It never drops debt
or advances over an unexecuted callback. Excess debt, regression, tick exhaustion
or callback error faults the schedule; the owner must shut down/recover.

Offline timing preserves exact rational 60 Hz, clamps admitted delta to 250 ms,
executes at most 8 callbacks and reports/discards remaining whole ticks while
retaining fractional interpolation time. Callback success alone advances tick
identity. Callback failures retain structured context; exceptions are contained
and reentrancy rejected. Cancellation is terminal and cooperative: it stops
before another callback, retaining inspectable debt, but cannot preempt user code.

`ServerApplication::run_idle` now borrows a clock and optional stop token, bounds
wait requests to 20 ms and validates progress/deadline arithmetic. Expected or
throwing clock failures return diagnostics while leaving resources owned for
explicit cleanup. `server/main.cpp` retains both the timing error and cleanup
failures. CLI flags and successful readiness/completion output are unchanged.
Idle polling is not authoritative simulation; there is no fake tick counter or
empty world introduced to claim a game service.

`completion_queue.hpp` adds a fixed inline value ring with explicit full/closed/
cancelled outcomes, ownership-preserving rejection, stop-aware producer blocking
and owner drain. Compile-time constraints bound slot count/storage and forbid
user move/destruction callbacks under the mutex. This does not bound external
memory reached through pointers or make mutable scene pointers valid payloads.
The queue uses ordinary-thread synchronization, not a signal/audio callback API.
Tests use real `std::jthread` producers; production Server remains worker-free.
No generic task executor, pool, service locator or third-party dependency lands.

Core exposes CMake's classified `Threads::Threads` platform target for standard
clock/thread support. Windows resolves the native thread runtime; pinned Ubuntu
resolves thread support from libc. No presentation or authoring library enters
Core/Server, and no dependency download or lock change is needed.

Changed implementation includes the two new public headers, `time.cpp`, existing
Server application/main and Core/test CMake lists. New tests are
`core_time_tests.cpp`, `server_timing_tests.cpp` and `core_work_tests.cpp`.
`shark.core.timing` and `shark.core.work` use `cpu;unit;server` labels, seed 1337
and 20-second timeouts. Work tests also retain the general `[core]` tag, so those
11 cases overlap the Core contracts family; family counts must not be summed as
unique cases.

## Verification and durable evidence

The [receipt](../coverage/evidence/03c-local.json) records the full base SHA,
changed source/test hashes, prior 03a/03b receipt hashes, exact commands,
toolchains, artifact digests and limitations. Raw output remains ignored.

| Configuration/check | Result |
| --- | --- |
| Windows Debug, Release, AddressSanitizer | 25/25 CTests each PASS |
| WSL pinned Ubuntu GCC Debug, Clang ASan+UBSan | 23/23 CTests each PASS |
| Timing family, all five configurations | 33 cases, 30,743 assertions each; no skips |
| Work family, all five configurations | 11 cases, 2,635 assertions each; no skips |
| Lifecycle regression, all five configurations | 26 cases, 2,568 assertions each; no skips |
| Core contracts including work, all five configurations | 42 cases, 3,643 assertions each; no skips |
| Windows Runtime-only / Server-only | Configure/build, dependency boundaries and executable smoke PASS |
| WSL Release Server-only | Configure/build/boundary/install, source/compiler/header/link/package closure and installed smoke PASS |
| Separate SDK-free WSL runtime | Fresh `/package-03c`, isolated network and timed Server startup/shutdown PASS |

All five final full suites tested the final C++ and test source. An initial passing
Debug run exposed a test-tag registration mismatch: scheduler tests appeared in
the Server unit group. Tags were normalized to `[timing]`, then the complete final
Debug suite was rebuilt/rerun. Its initial output is retained separately under
`out/03c-work/initial-windows-debug`; no code defect or tolerance relaxation was
needed. Source hashes remained unchanged through WSL verification and were checked
again when collecting the receipt. Final documentation/status changes are covered
by the final contract/repository checks.

Windows: Windows 11 build 26200 x64, MSVC 14.51.36231, SDK 10.0.28000.0,
Python 3.13.5. WSL: existing kernel 6.18.33.2 and pinned Ubuntu 24.04 userspace,
GCC 13.3.0, Clang 18.1.3, Python 3.12.3. Pinned tools: CMake 3.31.10, Ninja 1.13.2,
clang-format 20.1.8. No host package, driver, WSL resource setting, hosted workflow
or branch policy changed. The SDK-free smoke uses the same PC/shared WSL kernel;
it is not an independent-machine qualification.

Exact top-level commands from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/03c-work/verify-windows.py
wsl --user root --exec bash /mnt/c/Users/mrkda/Projects/shark/out/03c-work/run-headless.sh
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/03c-work/check-final-repository.py
```

The ignored Windows helper serializes these existing entry points, followed by
both standalone presets and their smoke checks; expanded commands are retained
in the receipt and `out/03c-work/final-windows/summary.json`:

```text
python tools/ci.py --preset windows-debug --jobs 2
python tools/ci.py --preset windows-release --jobs 2
python tools/ci.py --preset windows-asan --jobs 1
cmake --preset runtime-only
cmake --build --preset runtime-only --parallel 1
cmake --preset server-only
cmake --build --preset server-only --parallel 1
build/runtime-only/bin/Runtime.exe --version
build/server-only/bin/Server.exe --run-for-ms 20
```

The WSL helper uses private mount/network namespaces and retained pinned userspaces
with read-only source and fresh build/package paths. It invokes
`python /source/tools/headless_ci.py --build-root /work/build-03c --output /evidence
--jobs 2`, then the separate SDK-free runtime smoke. Raw evidence is under
`out/ci/03c-headless`; the wrapper/input digest records are in `out/03c-work`.

Final repository checks all PASS using pinned Python/tool PATH:
`python tools/check_format.py` (47 C++ files), `python tools/validate_contract.py`,
its `--self-test` (29 negative cases), `python tools/check_repository.py --self-test`
(five skills and Git ignore boundaries), and `git diff --check`.

## Full Core gate, limits and next task

Prior 03a/03b tests are rerun by all final suites: owned diagnostics and Release
process failures, malformed configuration, stable identity, Unicode/spaced paths,
resource ownership, repeated lifetimes and partial-start reverse rollback remain
covered. New fixtures prove monotonic/rational schedules, explicit overload/debt,
callback failure boundaries, cancellation/backpressure, actual blocked-producer
wakeup and repeated close/stop/join/drain before consumer release. Thread tests use
barriers/latches and bounded watchdogs, not arbitrary sleeps; simulated clocks
control scheduling. The real-time/owner/worker/signal policy table is in the guide.

The coverage manifest adds `core.time-policies` and `core.bounded-completions`,
marks full `REQ-03` PASS and advances its pointer to 04. Component, authoring-command
and Lua arrays remain empty. CPU/thread/process fixtures are justified external
coverage before FeatureWorld in 10. Plans and the ignored local roadmap now point
to 04; historical 03a/03b handoffs and receipts retain their original state.

All new tests ran without skips on both systems; existing platform-specific
tooling fixtures retain declared skips. Actual POSIX signal and descriptor-failure
process checks run in WSL. No GPU rerun is needed for these CPU changes. Window,
raw input, editor UI, authoring APIs, game simulation, networking, persistence and
MMO capacity remain outside this gate. Callback work and producer cancellation
are cooperative; neither a callback budget nor a wait request guarantees a hard
wall-time deadline for arbitrary code or OS scheduling. No hosted run was needed.

There are no outstanding 03c/REQ-03 blockers. `.gitignore` was reviewed and already
covers roadmap, builds, caches and generated evidence; no new rule is needed.
All new source/tests/docs remain eligible for version control. Nothing was staged,
committed or pushed.

Suggested commit: `feat(core): add timing policies and bounded completions`
