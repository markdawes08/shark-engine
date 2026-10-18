# 05c: Intel resource and ASan exit investigations

Date: 2026-10-18. Base: `9613bec` on `main`, initially clean. Result: uncommitted
code, regression tests, diagnostic tools and evidence. The user owns staging,
committing and pushing. **05c remains FAIL; full REQ-05 remains NOT_RUN. The next
assignment remains 05c, not 06.**

Intel USER growth is attributed to the installed driver's hook/thread lifetime
outside Shark, GLFW and NVRHI. The original ASan
fault-run timeout remains unexplained after a fixed, capture-ready diagnostic
sequence. A separate, demonstrated Windows queue-wait deadline defect is fixed;
that correction does not explain either original failure.

The [original qualification](05c-presentation-qualification.md),
[first investigation](05c-failure-investigation.md) and their receipts remain
historical evidence. No original failure, baseline, deadline, resource allowance
or required workload was replaced. NVIDIA's previously identified desktop-DC
retention remains open and was not reinvestigated here.

## Intel: independent resource reproduction

An isolated copy of the versioned raw Win32/Vulkan reproducer runs 40 fresh
window/instance/surface/device/swapchain lifetimes on Intel Raptor Lake-S Mobile,
UUID `868088a7040000000002000000000000`, installed driver 101.6790. It never
acquires, submits or presents, and links no Shark, GLFW or NVRHI implementation.
It creates no Shark queue-wait worker. Required pinned validation stays enabled.

Closed USER counts grow from 10 to 23; closed GDI counts stay at three and kernel
handles at 314. All native cleanup completes with zero validation diagnostics.
The bounded runner checks 163 ordered records, standard-user identity, immutable
inputs and eight deliberately corrupted traces. This reproduces the retention
outside engine ownership; it does not qualify the full presentation workload.

The final supported CDB trace of that same raw executable identifies eleven
pre-final failed removals at `igxelpicd64+0x2A26C8`. Each hook targets a thread
created from `igvk64+0x1704E9`, and each target has already entered
`ntdll!RtlExitUserThread` before removal. All eleven removals return FALSE with
last-error zero. Closed USER counts grow from 10 to 21, exactly eleven, while GDI
and kernel counts remain flat. Two intermediate samples briefly show 22. The
trace accounts for 243 successful installations: `igvk64` removes all 120 of its
hooks; `igxelpicd64` removes 106 of 123, with eleven pre-final and six later
process/DLL-exit failures. Those six late failures report invalid-hook-handle
1404 and are excluded from the measured eleven-object slope.
Debugger pauses affect timing; the untraced run's thirteen-object increase and
this traced eleven-object increase remain separate observations.

The final trace has complete entry/return pairing without duplicate entries.
Caller attribution uses module load history, and target timelines distinguish
thread-ID reuse by the latest preceding creation. The exact Windows symbols were
loaded from Microsoft's public symbol server into ignored local output, then
reused offline; the earlier incomplete trace is not a source of error codes.
The [Intel receipt](../coverage/evidence/05c-intel-user-investigation.json)
retains source/binary/driver identities, raw line references and the controlled
experiments. Together with the owned-hook control below, this identifies a
driver-owned hook lifetime hazard on this exact installed stack. Vulkan does not
expose these hooks or driver threads to Shark for cleanup, and no supported safe
application correction has been demonstrated.

Earlier supported-debugger observations of the engine transition fixture record
609 successful Intel hook installations. All 303 from `igvk64` have successful
removals; `igxelpicd64` has 306 installations, 296 successful removals and ten
failed removals. Three failures occur at process exit and also appear in the
no-resize control. Seven additional failures before close match the seven extra
USER objects retained after complete owner destruction and five seconds of
message pumping. Calls are paired by API, OS thread, stack address and caller;
returned hook handles are matched to later removal arguments. A second trace
with thread debug events has repeated-entry ambiguity and unavailable error
symbols; its incomplete API attribution is preserved, not treated as a clean
trace.

An independent CPU-only control owns its own three thread-specific message
hooks. Ten lifetimes remove hooks while the target thread remains alive and
return to their preceding USER count. Ten alternating lifetimes let the target
exit first: each subsequent removal fails, and each lifetime retains three USER
objects. Counts rise from one to 31 and remain 31 through a fixed five-second
message pump. Freshly cleared last-error values remain zero on these failures.
Thus failure does not imply the hook was already reclaimed. No diagnostic removes
or modifies a driver-owned hook. Microsoft documents hook ownership and removal
in [SetWindowsHookEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowshookexw)
and [UnhookWindowsHookEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-unhookwindowshookex).

A separate thread-placement counterfactual moved `vkQueueWaitIdle` onto the main
thread while retaining the otherwise identical newly created worker. Both fixed
100-resize/20-restore/20-lifetime runs still exceeded the original resource
limits. This rules out that call placement as a correction, not every possible
thread-lifetime mechanism. Blocking the main thread this way is diagnostic only
and is not shipped. Driver patches, foreign-handle cleanup, process-global owner
reuse and increased resource allowances are not introduced.

## ASan: captured boundaries, no reproduced incident

The [ASan receipt](../coverage/evidence/05c-asan-exit-investigation.json) preserves
the original 30-second timeout and exact old executable identity. Its last
`closed` record follows all seven native teardown calls and zero owned native
resources. The original record does not identify whether the native process,
Python worker or venv redirector stalled afterward. No original stack exists.

The fixed diagnostic sequence runs eight blocks of NVIDIA's four native faults
followed by Intel's four faults, matching the immediate original ordering. It
alternates the preserved original and corrected fixtures, with and without late
shutdown markers: 64 children, 32 per GPU, sixteen per variant. All exit with the
expected codes in 0.705–1.429 seconds; none reaches the eight-second capture
trigger, and the original 30-second deadline is unchanged. All sixteen ordinary
instrumented exits reach owner reset, local destruction, main return, `atexit`
and the static marker. Fifteen emit the final EXE TLS marker; that marker is not
proof that all DLL teardown finished, and its absence on a finite exit is not a
hang.

The versioned observer binds each owned process to both PID and creation time,
captures a minidump and bounded wait chain, and obtains offline stacks with the
installed CDB. Controlled native-`atexit` and Python-worker stalls prove the
process boundary distinction and finite cleanup. Wrong creation times are
rejected; duplicate process identities are captured once. Original executed tool
sources and the later observer correction have separate evidence. No debugger
attaches to the 64 live graphics children. The original ASAN_OPTIONS and complete
host activity were not recorded, so exact ambient reconstruction is unavailable.
Successful repetitions cannot establish a causal fix. **The incident remains
UNRESOLVED.** Invocation and prerequisites are in the
[diagnostic guide](../coverage/reproducers/README.md).

## Fixed: queue-wait deadline includes Windows thread exit

The original waiter published its action result, then joined a thread that could
still be running TLS destructors or DLL detach callbacks. A controlled CPU proof
made `wait(0)` block for approximately 257 ms until a watchdog released TLS
cleanup. This is a separate engine defect; the original ASan `closed` record was
already beyond this join.

The Windows implementation now waits once on its owned OS thread handle within
the existing budget, then joins only after termination. Zero and sub-millisecond
budgets poll; larger values round down to milliseconds, capped at one second.
Timeout and native wait errors preserve ownership and reject overlapping work.
There is no detach, cancellation, force-exit or extra timeout budget. Native
Windows details stay in a private EngineClient translation unit. Core/Server and
the non-Windows CPU path retain their dependency boundaries. The
[lifecycle contract](../presentation-lifecycle.md) records the platform limit.

The new gated TLS-destructor test covers zero, sub-millisecond and finite waits,
retained ownership, overlap rejection, one-time result consumption and reuse.
The exact test fails twelve assertions with the old header and passes with the
fix; the old run exits normally through Catch rather than hanging. An independent
code review found no ownership or synchronization issue.

The [queue-fix receipt](../coverage/evidence/05c-queue-exit-fix.json) records exact
commands, source/tool/binary hashes and these results:

| Verification | Result |
| --- | --- |
| Focused Windows queue tests | Six cases, 315 assertions PASS; old-header red control fails as expected |
| Windows Debug / Release / ASan CPU suites | 43 checks each; Debug initially 42/43 due to a documentation link published before its target, then the single contract recheck PASS; original failure retained |
| Existing WSL GCC Debug / Clang ASan+UBSan CPU client suites | 40/40 each, no skips or source changes; native graphics/window options OFF |
| Both GPUs, all three Windows profiles | Six short presentation runs and six native-fault wrappers (24 fault cases) PASS with required validation |
| Runtime-only and relocated Release package | Build/boundary, DLL/license closure and restricted-environment launches on both GPUs PASS |

The implementation changes `graphics_queue_waiter.hpp`, adds its private Win32
helper, selects that helper in EngineClient CMake, and extends the existing queue
test family. Coverage maps the helper and TLS behavior to that family; no public
API, component, authoring command, Lua or FeatureWorld surface is added. Generated
traces, dumps, symbols and package staging remain under ignored `out/`/`build/`.
`.gitignore` was reviewed and remains sufficient, including the local roadmap.

Final review ran through `tools/windows_dev.ps1`; raw logs are retained in
`out/05c-intel-asan/final-review/`. All three new evidence receipts were hashed
after review, and the four production/test input hashes still match the verified
queue-fix receipt. The original qualification/investigation receipts have no Git
diff. Nothing is staged and the base revision is unchanged.

| Final command | Result |
| --- | --- |
| `python tools/check_format.py` | PASS, 85 engine-owned files |
| `clang-format --dry-run --Werror` on the four diagnostic C++/header files | PASS |
| `python tools/validate_contract.py --self-test` | PASS, 29 negative checks, 50 requirements/46 assignments and resolved document links |
| `python tools/check_repository.py --self-test` | PASS, source/ignore/skill checks and three negative metadata cases |
| `git diff --check` | PASS |

All three diagnostic Python files also pass syntax parsing. The Intel reproducer
passes 24 control/parser/CLI/source-equivalence checks; its current owned-hook
control executes under CDB, while its newly built raw40 executable is build-only
evidence. The generated raw source exactly matches the separately executed and
traced source. The ASan observer has separate executed-native and corrected-CPU
selfcheck receipts, preserving which tool version produced each observation.

The full Release/ASan sustained stress matrix is not rerun while the independent
driver retention and unexplained original timeout remain open. Short regressions
and a package pass do not replace that gate. No new Server package, Mac, native
Linux GPU or additional-machine qualification is claimed. After actual blocker
corrections, rerun the complete unchanged 05c qualification before starting
assignment 06, shader pipeline and basic GPU resources.
