# 05c: presentation failure and stress qualification

Follow-up: the [failure investigation](05c-failure-investigation.md) records work
after commit `cd97e87`, including the independent NVIDIA reproducer, persistent
Intel USER-resource observations and bounded ASan exit diagnostics. This handoff
and its original failed evidence remain the historical implementation record.

Date: 2026-10-18. Base revision: `e05df12ce1d5977d23cdcac76ed8bba94bd917b2`
on `main`, initially clean. Changes remain uncommitted; the user owns staging,
committing and pushing. Current checkpoint status: **FAIL; qualification remains
open**. Full `REQ-05` remains **NOT_RUN**. NVIDIA GDI-object retention and an Intel
ASan fault-child exit timeout remain unresolved; assignment 06 cannot begin.

The [local evidence receipt](../coverage/evidence/05c-local.json) records the
failed checkpoint, completed checks, exact source/binary/tool identities and
retained partial runs. It keeps the original regression baseline separate from
the subsequent one-file fault-observer correction and focused rechecks.

## Delivered behavior

The [qualification contract](../presentation-qualification.md) declares the fixed
workload, resource allowances, failure policy and terminal commands. New explicit
`PresentationStress` and `PresentationFaults` fixtures use the production
`GraphicsDevice` and existing bounded lifecycle. The private backend snapshot
counts owned swapchains, images, command slots, synchronization objects and
unresolved workers/recordings. Native handles remain private. Fault controls exist
only in test-enabled native builds and require explicit private construction;
Runtime-only has neither their factory implementation nor an activation flag.

Native recording error/exception injection occurs after actual NVRHI command-list
opening. The production destructor must retain unresolved owners and terminate
the isolated child with the expected diagnostic. Surface/device error injections
occur before acquisition and require real healthy-driver completion and safe
cleanup; they never simulate a lost driver to bypass draining. CPU child fixtures
exercise pending, error and exception fatal-drain paths. Physical GPU removal,
driver reset and actual device loss are not induced or claimed.

Strict wrappers bind raw records to source, binary, lock, compiler, tool,
validation and selected-device identities. Parser mutation tests reject missing
work, budget changes, bad peaks, invented GPU work, incomplete cleanup, diagnostic
execution and malformed identities. Ordinary CTest remains device-free.

`tools/package_runtime.py` creates a protected Release Runtime deployment fixture
with the pinned Vulkan loader, locked Release CRT, dependency metadata and notices.
Its standard-user check verifies an external Unicode/space relocation, isolated
environment and actual loaded modules on both GPUs. It does not implement game
export, cooking or release acceptance for later assignments.

## Investigations retained

- The first package preflight rejected the always-scheduled CMake boundary check
  as stale compilation. The freshness checker now permits only that known check
  or Ninja's no-work result and still rejects recompilation/relinking. A negative
  fixture covers this distinction. The corrected package preflight passed both
  GPUs; its earlier failure remains retained.
- Initial stress preflights and the existing short presentation fixture failed a
  700x420 resize that Windows reported as 694x403 at 150% scaling. Added diagnostics
  retain the last content/framebuffer, DPI context, styles and native frame sizes.
  Native clients now declare the existing intended PerMonitorV2 policy in their
  embedded manifest before DLL/UI initialization. The unmodified short fixture,
  without added diagnostic queries, then passed four lifetimes and all 16 resizes.
  This is an observed correction and explicit startup policy; the transient
  upstream cause was not established. The native window fixture now verifies
  window/thread awareness as well as actual DPI/pixel relationships.
- A nonqualifying 60-second trace completed 100 resizes, 20 minimize/restore pairs,
  55,942 sustained presentations and 20 subsequent device lifetimes. The first
  restore loaded `CoreUIComponents.dll` before rendering and increased process
  handles from 619 to 640. Later active samples stayed between 640 and 643;
  closed lifetimes 5–20 stayed at 366. All 127 original active-budget failures
  remain failures. The fixed warmup was explicitly amended to include one
  minimize/restore among its 32 frames before establishing the baseline. The
  measured 100/20 workload and eight-handle/memory limits were preserved.
- The same diagnostic trace exposed a GDI-object increase across full device
  lifetimes. A separate standard-user, Vulkan-free isolation completed 32 Shark
  Window lifetimes and 32 raw GLFW lifetimes with closed GDI/USER counts constant
  at 9/2. NVIDIA device-foundation-only lifetimes also stayed flat. With a
  swapchain, 32 NVIDIA lifetimes increased closed GDI counts from 10 to 41,
  including a separate run with no image acquisition, submission or presentation.
  Instrumentation localized the increment to `vkCreateSwapchainKHR`; image
  wrappers/views added none. Full swapchain/device/surface/instance/window
  destruction did not reclaim it. Four additional unsubmitted lifetimes retained
  the increment after 100 ms, 500 ms and two seconds of Win32 message pumping.
  Thirty-two Intel presentation lifetimes kept GDI counts at 10. This localizes
  the observed API boundary, but does not establish an internal vendor cause or
  a safe application fix. Kernel handles and direct engine counters alone would
  miss this failure. No foreign GDI object is deleted as a workaround.
- The Intel ASan `device-error` child reached its final `closed` record with one
  submitted/completed/presented frame, one retired generation, all ten owned
  resource counters zero, all seven teardown calls and zero validation
  diagnostics. It nevertheless exceeded the 30-second process deadline and was
  terminated by the wrapper. This is a failed finite-exit check after recorded
  native cleanup, not a passing child or evidence of a GPU drain timeout. The
  original raw trace and timeout remain retained.

Raw investigation artifacts are retained under `out/05c-work/` and
`out/ci/05c-*-preflight*/`. Earlier 05a/05b receipts and handoffs remain historical.
No dependency version, validation tolerance, GPU target or host setting was
changed to hide a failing run.

## Recorded local checks

Windows uses MSVC 19.51.36248 and Windows SDK 10.0.28000.0. The selected GPUs are
NVIDIA RTX 4070 Laptop, UUID `863d6f59a51894ab8b4829aa9460c78d`, driver 610.62;
and Intel Raptor Lake-S Mobile, UUID `868088a7040000000002000000000000`, driver
101.6790. Required-validation receipts use the unchanged pinned tools from
`out/05a-work/validation-tools/summary.json`.

| Check | Observed result |
| --- | --- |
| Windows Debug, Release and ASan CTest | 43/43 each |
| WSL GCC Debug and Clang ASan/UBSan CPU client CTest | 40/40 each |
| WSL headless Debug and ASan/UBSan CTest | 28/28 each |
| Presentation CPU family | 37 cases, 1,744 assertions per client configuration |
| Short native presentation, both GPUs and all three Windows builds | 24 lifetimes, 2,304 frames, 96 resizes, 48 minimize/restore pairs; PASS |
| Native device foundation | 96 lifetimes; PASS |
| Native window and application regression | Three window runs, 14 graphical application samples; PASS |
| Runtime-only and Server-only configure/build boundaries | PASS |
| Installed Server in isolated SDK-free WSL runtime | PASS |
| Relocated Release Runtime dependency check | Both GPUs PASS |

The 265 CTests passed against the recorded frozen source inventory. Original
Windows regression orchestration retained 50 passing commands and the one failed
Intel ASan native-fault wrapper. Successful CPU and short native checks do not
override either failed stress gate or the child exit timeout.

Review then found an independent test-harness lifetime defect: the terminate
observer retained a pointer to expired stack/backend storage on ordinary exit.
`tests/presentation_faults.cpp` now scopes that observation, clears it before
leaving the scope and dereferences it only for an explicitly expected fatal exit.
Only that fixture changed after the full regression baseline; every other source
input and the original receipts were checked unchanged. Focused rebuilds and
fault CTests passed on all three Windows configurations and both WSL client
configurations (15 CPU child cases). All six fresh native fault runs passed
(24 children), including Intel ASan. Receipts:
`out/05c-work/fault-observer-amendment/summary.json`,
`out/05c-work/wsl-faults-amendment-verification.json` and
`out/ci/05c-faults-final/`.

Before that correction, a copied ASan fixture with post-close markers also exited
normally through owner reset, local destruction, main return, `atexit` and static
destruction. Neither the diagnostic pass nor the later passing fault runs prove
the cause of the original timeout. Its failed record is not replaced, and the
observer correction is not claimed as a demonstrated shutdown-hang fix.

The final Runtime package was relocated to
`C:/Users/mrkda/Shark deployment 05c final Ω`. NVIDIA and Intel presented 73 and
64 frames respectively, with all submitted work completed. Standard-user launches
used system-only PATH, cleared development overrides and the exact app-local
Vulkan loader/three Release CRT DLLs; observed remaining modules came from
Windows. Dependency, license, manifest and module polling checks passed.
Its validation-off launch remains **NOT_RUN for graphics correctness** and does
not replace the required-validation native tests. Receipt:
`out/ci/05c-runtime-package-final/summary.json`.

The current normal Debug stress attempts are both **FAIL**, preserved under
`out/ci/05c-stress-final/windows-debug/<UUID>/`:

- NVIDIA completed 60,001 ms, 92,092 measured presentations, all 100 resizes and
  all 20 measured minimize/restore pairs. The sixth subsequent closed lifetime
  had 16 GDI objects against the fixed fifth-lifetime baseline of 15. Kernel
  handles stayed at 366 and USER objects at four. All engine-owned resources
  were zero and validation was clean. The run correctly stopped; only six of
  the required 20 subsequent lifetimes completed.
- Intel stopped after ten resizes and two measured minimize/restore pairs when
  a restore sample had 35 USER objects against the initialized baseline of 34.
  GDI objects and kernel handles stayed at 17 and 412. All 992 submitted frames
  completed, failure cleanup released every owned resource, and validation was
  clean. This observed zero-growth budget violation does **not** establish a
  USER-object leak: asynchronous Windows UI cleanup needs separate investigation.

Release and ASan sustained runs remain **NOT_RUN** while the Debug resource
failures are unresolved. No 600-second Release pass or complete six-run stress
matrix is claimed. The fixed workloads and budgets remain unchanged.

## Coverage and boundaries

The manifest adds `client.presentation-qualification` and
`foundation.runtime-package` with executable fixture mappings. There are no
components, authoring commands or Lua APIs to map yet. FeatureWorld begins in 10.
Core/Server remain free of native presentation dependencies. This work establishes
neither a scene renderer nor MMO capacity, persistence or seamless-world behavior.
Human and terminal users share the existing explicit graphical diagnostic and
machine-readable failure paths; authoring remains with its planned owners.

The next work is still **05c**: resolve or explicitly account for the observed
native resource behavior, investigate the bounded-exit failure, then execute the
entire declared stress/fault matrix with current-source evidence. A controlled
experiment or later passing retry does not erase a retained failure. Only after
that gate passes may **06, shader pipeline and basic GPU resources**, begin.
