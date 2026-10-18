# 03b - Application ownership and lifecycle

Date: 2026-10-18. Base: `a8e11a2` on `main`, initially clean.
Result: uncommitted worktree changes; the user owns staging, commits and pushes.

**03b is complete within its checkpoint scope.** Full `REQ-03` remains NOT_RUN
until **03c, time, bounded work and final qualification**, under the
[Core plan](../roadmap/core-plan.md). No assignment 04 work is started.

## Delivered behavior and compatibility

`ServerApplication` explicitly owns its configuration/state and a `ShutdownSignals`
resource, which exclusively owns an injectable signal backend. The process root
owns its logger; borrowed output streams must outlive their consumers. Runtime
and Editor retain explicit bootstrap logger ownership, with no absent game
services represented by lifecycle stubs. The
[lifecycle guide](../application-lifecycle.md) documents ownership and transitions.

Startup validates the complete candidate before claiming process signal resources.
It acquires SIGINT then SIGTERM, then publishes readiness before becoming running.
Partial acquisition and failed readiness unwind successful acquisitions in reverse
order. Cleanup tries both restorations even if one fails, without allocating or
throwing during signal-resource restoration. Structured reports retain the primary
error and each cleanup error; state is committed before allocating those diagnostics.

Explicit stop restores handlers before successful completion output. Failed
restoration faults the application, suppresses successful completion and prevents
unsafe reuse of the native process lease. Clean stop is idempotent; clean failures
can retry. Reentrant transitions are rejected, and the API requires serialized
owner-thread calls. The destructor performs silent, bounded fallback cleanup.

The native signal bridge contains only process-level ownership and a `sig_atomic_t`
request value; its handler assigns that value without logging, allocating or
accessing an application pointer. Each fresh lease resets stale stop requests.
Native registrations are inherently process-wide, not a global service registry.

`server/main.cpp` uses this application and reports every retained failure through
the existing logger. CLI flags, successful output and the idle timing algorithm
remain unchanged. Invalid configuration still exits 2; lifecycle/output failure
exits 1. No game-authoring protocol or data-format migration is introduced.

New headers/implementations are
`engine/core/{include/shark,src}/{server_application,shutdown_signals}.{hpp,cpp}`;
tests are `tests/{server_application,shutdown_signal}_tests.cpp`. Core and test
CMake source lists register them and the `shark.core.lifecycle` family with
`cpu;unit;server` labels, seed 1337 and a 15-second timeout. Existing process tests
remain valid. No dependency, window, GPU, audio or authoring library enters Core/Server.

## Verification and evidence

The [durable receipt](../coverage/evidence/03b-local.json) records the full base
SHA, changed source/test digests, exact commands, artifact hashes, toolchains,
results and limitations. Raw evidence stays ignored in `out/`.

| Configuration/check | Result |
| --- | --- |
| Native Windows Debug | 23/23 CTests PASS |
| Native Windows Release | 23/23 CTests PASS |
| Native Windows AddressSanitizer | 23/23 CTests PASS |
| WSL pinned Ubuntu 24.04 GCC Debug, headless | 21/21 CTests PASS |
| WSL pinned Ubuntu 24.04 Clang ASan+UBSan, headless | 21/21 CTests PASS |
| Lifecycle family in all five configurations | 26 cases, 2568 assertions each, seed 1337; zero lifecycle skips |
| Windows `runtime-only` and `server-only` | Configure/build, protected boundaries and executable smoke PASS |
| WSL Release `server-only` | Configure/build/install, compiler/header/link/package closure and installed smoke PASS |
| Separate SDK-free WSL userspace | Fresh installed Server path with isolated network: timed startup/shutdown PASS |

The full Windows Debug run preceded a final header/test refinement; its final
incremental build and focused lifecycle rerun passed. Release, Windows ASan and
both WSL suites tested the final source. WSL source hashes remained unchanged
through the run and were checked again while creating the receipt.
One final manifest explanation was corrected after the WSL run; the receipt
records both hashes, and final contract/repository checks cover that documentation
change. No C++ or test source changed after final verification.

Windows: Windows 11 build 26200 x64, MSVC tools 14.51.36231, SDK 10.0.28000.0,
Python 3.13.5. WSL: existing kernel 6.18.33.2, pinned Ubuntu 24.04 headless
userspace, GCC 13.3.0/Clang 18.1.3 and Python 3.12.3. Pinned development tools:
CMake 3.31.10, Ninja 1.13.2 and clang-format 20.1.8. No host package, driver,
WSL resource setting, hosted workflow or remote branch policy changed.

Full native commands from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python tools/ci.py --preset windows-debug --jobs 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python tools/ci.py --preset windows-release --jobs 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python tools/ci.py --preset windows-asan --jobs 1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/03b-work/verify-final-windows.py
```

The ignored final helper runs Debug rebuild/lifecycle CTest, `runtime-only` and
`server-only` configure/build, `Runtime.exe --version`, and
`Server.exe --run-for-ms 20` serially. The receipt preserves all expanded
arguments and exit codes; logs are in `out/03b-work/final-windows`.

WSL verification uses a private mount namespace, the retained pinned build/runtime
userspaces, read-only source and fresh 03b build/output/package paths:

```text
wsl --user root --exec bash /mnt/c/Users/mrkda/Projects/shark/out/03b-work/run-headless.sh
```

It invokes `python /source/tools/headless_ci.py --build-root /work/build-03b
--output /evidence --jobs 2`, then tests the installed Server in the separate
SDK-free runtime userspace with an isolated network. Logs are under
`out/ci/03b-headless`. This is isolation on the same PC/shared WSL kernel, not
an independent machine. The receipt preserves the script and output hashes.

Final checks use the pinned interpreter/tool PATH:
`python tools/check_format.py` (41 C++ files),
`python tools/validate_contract.py`, its `--self-test` (29 negative cases),
`python tools/check_repository.py --self-test` (five skill sources and ignore
boundaries), and `git diff --check`. All PASS. The initial standalone format
command lacked the pinned executable on PATH; rerunning through the documented
Windows wrapper passed without source changes.

## Coverage, limits and next checkpoint

The implemented feature `core.application-lifecycle` maps REQ-03/REQ-M01 to the
new lifecycle family and existing Server process fixture. Tests inject every
actual acquisition-stage failure, combined startup/cleanup errors, completion
and readiness write/flush exceptions, reentrant transitions, repeated lifetimes,
ownership/destructor behavior and real native handler restoration. No fixture
pretends to own workers or game services that do not exist yet.

Component, authoring-command and Lua coverage arrays remain empty. Native signal
and process fixtures are justified external coverage; FeatureWorld begins in 10.
The manifest retains current assignment 03 and overall NOT_RUN pending 03c.
README, AGENTS, architecture index, MMO/agent plans and the ignored roadmap now
point to 03c. There are no outstanding 03b blockers.

All new lifecycle cases ran on Windows and WSL. Existing platform-specific tooling
fixtures retain their declared skips: actual process SIGINT/SIGTERM and descriptor
failure checks run in WSL; Windows additionally exercises native `std::raise`
handler ownership/restoration. No GPU rerun is required for these CPU-only changes.
Timing, queues, cancellation and stop/join/drain qualification belong to 03c.
Graphical editor, game authoring, networking, persistence and MMO capacity remain
planned. No new hosted run was needed or claimed.

`.gitignore` was reviewed and already covers the local roadmap, builds, output,
caches and generated fixtures. No new rule is needed; all new source/tests/docs
remain eligible for version control. Nothing was staged, committed or pushed.

Suggested commit: `feat(core): add owned server lifecycle and startup rollback`
