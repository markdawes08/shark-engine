# 04c - Frame-loop integration and qualification

Date: 2026-10-18. Base: `2d5270a` on `main`, initially clean.
Result: uncommitted worktree; the user owns staging, commits and pushes.

**04c and full `REQ-04` are PASS under the current-machine scope.** All three
assignment-04 checkpoints are complete. The exact next assignment is
**05, production graphics device and presentation lifecycle**; its implementation
and qualification remain NOT_RUN.

The [frame-loop contract](../frame-loop.md) describes the implementation and public
API, input across zero/multiple ticks, pause/step, focus cancellation, bounds and
sample launch controls. Core extends its existing offline scheduler; Server
scheduling remains independent. Runtime and Editor share the same tested pump.
No rendering, scene, authoring command or MMO gameplay protocol is introduced.

## Delivered behavior

`OfflineFrameLoop` owns one existing `OfflineScheduler` and fixed-size pending
input. Each frame retains its independent presentation snapshot. Across zero
ticks, edges coalesce, latest held/cursor state wins and text/scroll accumulate
within the existing bounds. The first tick consumes interval data; later ticks
receive held state without replaying positive actions. Focus loss cancels older
pending positives before merging any post-regain input. Invalid capture bounds,
coordinates/scalars and invalid step requests reject before mutation.

Core adds `OfflineControl` while retaining the original running overload.
Pause clears whole/fractional debt, ignores paused elapsed time and leaves tick
indices intact. The first resumed pass has zero ticks. Single-step uses the same
callback commit/error path, runs once and remains paused. Clock regression clamps
without lowering the high-water observation; reentrancy rejects without mutation.
Callback failure/exception and index overflow fault the scheduler, while
cancellation terminates further execution. Server code and its
authoritative debt policy are unchanged.

The shared native sample pump polls even while paused, reports diagnostics outside
callbacks and suppresses another tick on close. F6 toggles manual pause; F7 steps
while manually paused and active. Repeat does not add steps. These keys are
removed from tick input, and sample control changes produce small JSON diagnostic
lines. The actual callback is empty because there is no scene yet. Focus/iconify
automatically pause only this offline coordinator and cannot clear manual pause.
There is no new persistent schema, authoring command, Lua API or player protocol.

Changed code comprises the new client frame-loop header/source and private shared
pump seam, existing launch/bootstrap sources, the Core time header/source and
their CMake registrations. Tests add one frame family, extend Core/window fixtures,
and extend the native smoke from eight to eleven cases. No dependency pins,
options or host configuration changed. The public contract and progress guides
were updated; earlier handoffs and receipts remain unchanged.

## Verification and evidence

Final source/test/CMake/tool hashes were frozen before both Windows and WSL runs.
Documentation/status edits followed the source freeze and receive final contract
and repository validation. The [durable receipt](../coverage/evidence/04c-local.json)
retains exact commands, hashes, JUnit summaries and limits. All 135 source/test/
CMake/tool/lock inputs captured for WSL remained unchanged; the 15 changed
implementation/test/tool files also match the Windows pre-run snapshot.

| Configuration/check | Result |
| --- | --- |
| Windows Debug / Release / AddressSanitizer | 30/30 CTests each PASS |
| Native Windows smoke in all three configurations | 11 cases and 17 engine window lifetimes each PASS |
| Runtime and Editor bounded native windows | Both executables PASS in all three configurations |
| Windows Runtime-only | Configure/build/boundary, version and bounded window PASS |
| Windows Server-only | Configure/build/boundary and finite Server smoke PASS |
| WSL client CPU GCC Debug / Clang ASan+UBSan | 30/30 CTests each PASS |
| WSL headless GCC Debug / Clang ASan+UBSan | 23/23 CTests each PASS |
| WSL Release Server install/closure and SDK-free runtime | PASS |

The seven configurations total **196 passing CTests** with no CTest skips.
The client frame family records 26 cases / 6,220 assertions; window
35 / 1,941; input 17 / 34,172; Core timing 42 / 30,942, with seed 1337. Six shared
pump cases carry both frame/window tags, so these are not unique-case totals.
All five client configurations match these counts and have no family skips;
Core timing also passes in both headless configurations.
The frame cases cover cross-capture taps, focus loss/regain within one capture,
overflow across individually valid captures, independent presentation values,
manual/automatic pause, fresh step input, resume, long frames, invalid snapshots,
callback errors/exceptions, cancellation, reentrancy and independent server time.
Injected window cases cover native poll/query failure, diagnostic sink failure,
close-before-tick, reserved control keys and continued polling while minimized.

Native smoke exercises the same Runtime/Editor pump through real GLFW callbacks
using targeted Win32 messages to an owned HWND. It checks zero/multiple fixed
ticks, F6/F7 press-versus-repeat, real focus/iconify/restore, a separate progressing
ServerScheduler and an injected long interval that executes eight callbacks and
reports seven discarded whole ticks. The unchanged lifecycle/DPI cases still
observe the current 144-DPI monitor at scale 1.5, resize, Unicode, bounded callback
diagnostics, close and repeated RAII cleanup. No physical device input,
multi-monitor change or GPU execution is claimed. Controlled time tests simulate
a stall; the actual Runtime/Editor runs separately use the real steady clock.

An initial native smoke passed before final formatting and the added CPU pump
fixtures. Final native smoke reruns use frozen source. A first format invocation
without pinned tools on PATH correctly rejected the environment; the Windows
developer wrapper selects the pinned formatter for final checks. No correctness
failure was hidden or baseline regenerated during this increment.

Exact top-level commands from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/04c-work/verify-windows.py
.tools/venv/Scripts/python.exe out/04c-work/run-wsl-verification.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/04c-work/check-final-repository.py
.tools/venv/Scripts/python.exe out/04c-work/collect-evidence.py
```

The Windows helper runs `tools/ci.py` serially for Debug/Release with two jobs
and ASan with one, then runs `tools/window_smoke.py` into fresh
`out/ci/04c-window/<preset>` paths and both applications with `--window --window-ms
100`. It configures/builds Runtime-only and Server-only with one job and runs
their executable smokes. All expanded commands and process-cleanup outcomes are
retained. Previous 04b Windows CPU outputs were copied to an ignored preservation
directory before replacement.

The WSL helper reuses retained pinned Ubuntu build/runtime userspaces with fresh
`/work/build-04c`, `/work/build-04c-client` and `/package-04c` paths, read-only
source and private mounts. `tools/headless_ci.py` runs Server-only Release and
GCC/Clang headless suites; the separate SDK-free userspace runs the installed
package with an isolated network. The two CPU client presets add the frame/window
tests without enabling native graphics. Explicit cache/target checks require
native-window OFF, no C compiler and no GLFW/Vulkan/NVRHI dependencies in all
five WSL configurations. These are same-PC CPU/package checks, not native Linux
window qualification or independent-machine deployment.

Windows uses Windows 11 build 26200 x64, MSVC 14.51.36231, SDK 10.0.28000.0 and
Python 3.13.5. WSL uses the retained Ubuntu 24.04 profile with GCC 13.3.0,
Clang 18.1.3 and locked minimal-profile Python 3.12.3. Shared pinned tools are
CMake 3.31.10, Ninja 1.13.2 and clang-format 20.1.8. No host packages, WSL resource
settings, display scaling, hosted workflow or remote branch policy changed.
Final formatting checks pass for 65 C++ files, contract validation covers all 50
requirements and 46 assignments, all 29 contract negative checks pass, repository
self-tests pass for five skills and three negative fixtures, and `git diff
--check` is clean. The staging area remains empty.

## Scope and next assignment

The new `client.offline-frame-loop` coverage entry connects the implementation to
real deterministic and native fixtures. `core.time-policies` now also contributes
to REQ-04. FeatureWorld/FrameTiming attaches in assignment 10; no component,
command or Lua surface is fabricated here. Existing platform-specific tooling
fixtures retain declared internal skips; input/window/frame/Core families do not.

The window remains empty and has no graphics context. Per-pass callback counts
cannot preempt unbounded user work or stalled native calls; qualification uses
external process-tree deadlines. Event timing is frame-coalesced rather than an
ordered timestamped gameplay intent stream. Offline pause/step does not define
MMORPG prediction, replication or world recovery. Those remain with their owners.

`.gitignore` already covers all generated builds, evidence, dependency/tool caches
and the local roadmap, so no rule change was needed. No staging, commit or push
was performed. With final 04c qualification complete, the exact next assignment is
**05: production graphics device and presentation lifecycle**, preserving the
current-PC Windows graphics and WSL headless scope.

Suggested commit: `feat(loop): integrate fixed-tick input and offline pause controls`
