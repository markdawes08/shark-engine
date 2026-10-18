# 05a - Owned graphics device foundation

Date: 2026-10-18. Base: `4dbb80d00f63394166883fa5e9ef8361425548c4` on `main`, initially clean.
Result: uncommitted worktree; the user owns staging, commits and pushes.

**05a is PASS under the current-machine scope.** The exact next checkpoint is
**05b: swapchain, submission and presentation lifecycle**. Full `REQ-05` remains
NOT_RUN until 05b and 05c complete. The [checkpoint plan](../roadmap/graphics-device-plan.md)
and [device guide](../graphics-device.md) define delivered behavior and remaining work.
No image rendering or presentation is implemented in this checkpoint.

## Delivered behavior

EngineClient now owns the existing Window, Vulkan instance, Win32 surface,
logical device and NVRHI validation wrapper. Public headers expose engine-owned
configuration, adapter/capability metadata and diagnostics. The existing Window
lease prevents simultaneous native owners. UUID selection is exact; automatic
selection prefers supported discrete hardware, then integrated hardware, with a
stable UUID tie-break. Software and unsupported adapters reject before device
creation, identifying missing profile requirements. The capability profile and
all dependency revisions remain unchanged from assignment 02.

Every attempted startup stage unwinds in reverse order on failure or exception.
Explicit close is idempotent and preserves diagnostics. Startup failures retain
the primary error and any additional teardown validation errors. Main-thread
ownership is explicit, and foreign-thread calls reject. Fixed callback slots
bound diagnostic writes without allocation or I/O; snapshots own and sanitize
native bytes outside callbacks. No GPU commands are exposed or submitted, so
05a does not need a device-idle wait. Submission and safe retirement must land
together in 05b.

Windows native clients now share pinned Vulkan/NVRHI inputs with the existing
probe, privately through EngineClient. The loader's Windows SDK `cfgmgr32` edge
is explicitly classified, and boundary tests accept only its exact control-flow
and exception-continuation security flags. Core/Server and device-free clients
remain outside this dependency graph. The dependency lock changes only owner
metadata. The UTF-8 process manifest propagates to native client consumers.

`GraphicsDeviceSmoke` exposes strict, noninteractive JSON description, inventory
and repeated-lifetime commands. Help, description and invalid arguments require
no device initialization. Hardware runs use current-lock validation receipts,
source/binary identity checks and an external 60-second process-tree deadline.
Required validation enables Vulkan core/synchronization plus NVRHI validation;
a validation-off run is explicitly diagnostic and never qualifies the checkpoint.
Editor/Runtime still use the existing empty `--window` sample. Neither that sample
nor the new fixture is a game-authoring API or a player protocol.

## Verification

The [durable receipt](../coverage/evidence/05a-local.json) records commands,
source and artifact hashes, hardware/driver/toolchain identities and limits.

| Configuration/check | Result |
| --- | --- |
| Windows Debug / Release / AddressSanitizer | 33/33 CTests each PASS |
| WSL CPU client GCC Debug / Clang ASan+UBSan | 32/32 CTests each PASS |
| WSL headless GCC Debug / Clang ASan+UBSan | 24/24 CTests each PASS |
| Windows Runtime-only / Server-only | Configure/build/boundaries and executable smoke PASS |
| WSL Release Server install/closure and SDK-free runtime | PASS |
| Both GPUs in Windows Debug / Release / ASan | 16 device lifetimes per GPU per configuration PASS |
| Debug automatic GPU selection | 16 lifetimes, NVIDIA selected, PASS |
| Existing native window regressions, all three Windows configurations | 11 cases / 17 window lifetimes each PASS |
| Existing opt-in probe configure/build and two CPU checks | PASS |

The seven principal configurations total **211 passing CTests**, with no CTest
skips. The device family passes **29 cases** with seed 1337 in all five client
configurations: 904 assertions on Windows and 912 in WSL, whose native-unavailable
branch checks add eight assertions. It covers each capability threshold, missing and
ambiguous UUIDs, software rejection, malformed UTF-8 metadata, partial startup,
exceptions, primary/cleanup error retention, ownership/thread violations,
idempotent close, callback overflow and concurrent diagnostic producers.
Twelve parser tests and 22 device-free native CLI checks verify truthful reports.
Platform-specific tooling retains its declared internal skips; these are distinct
from missing GPU qualification or skipped graphics-owner cases.

Native required-validation runs total **112 device lifetimes**: 48 explicit
lifetimes on Intel, 48 on NVIDIA, and 16 automatic-selection lifetimes on NVIDIA.
Every cycle rejects a second owner, closes twice, retains matching adapter/profile
metadata and reports zero warnings/errors/dropped diagnostics before and after
teardown. Inventory passes independently in all three Windows configurations.

- NVIDIA GeForce RTX 4070 Laptop GPU: UUID `863d6f59a51894ab8b4829aa9460c78d`, driver `610.62`.
- Intel RaptorLake-S Mobile Graphics Controller: UUID `868088a7040000000002000000000000`, driver `101.6790`.

Real native negative checks return nonzero for an unknown UUID and missing
required validation layers, preserving the expected selection/instance operation.
A separate validation-off run completes successfully while retaining NOT_RUN
qualification. These negative/diagnostic runs do not inflate the 112 qualifying
lifetimes. No validation mode, capability minimum, driver or host setting was
changed to obtain a pass.

The Windows helper uses two build jobs for Debug/Release and one for ASan, then
runs native window regressions, both bounded Runtime/Editor samples, required
GPU inventories/lifetimes and standalone boundary builds. The WSL helper uses
fresh build/package paths in retained pinned Ubuntu build/runtime userspaces,
read-only source and isolated runtime networking. All five WSL configurations
explicitly disable both native options and audit absence of C/GLFW/Vulkan/NVRHI
presentation dependencies. This is same-PC CPU/package evidence, not native
Linux graphics or deployment to an independent physical machine.

Exact orchestration commands and their expanded subprocesses are retained:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/05a-work/verify-windows-final.py
.tools/venv/Scripts/python.exe out/05a-work/run-wsl-verified.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/05a-work/check-final-repository.py
.tools/venv/Scripts/python.exe out/05a-work/collect-evidence.py
```

Generated helpers and raw reports remain ignored; checked-in tooling and the
receipt preserve reproducible commands and observed results. Windows uses MSVC
19.51.36248.0 / toolset 14.51.36231 and SDK 10.0.28000.0; WSL uses GCC 13.3.0,
Clang 18.1.3 and Python 3.12.3. Pinned CMake/Ninja/clang-format remain
3.31.10 / 1.13.2 / 20.1.8. Windows Python is 3.13.5.

## Failures investigated and verification limits

Initial integration exposed the protected checker rejecting the pinned loader's
security flags and raw SDK library edge. Exact allowances/classification and
positive/negative boundary fixtures resolve that without accepting arbitrary
link flags. An early CPU test run used stale `invalid_configuration` expectations;
the public malformed-argument contract uses `invalid_argument`, and final rebuilt
tests pass. Evidence tests found boolean cycle numbers, oversized metadata and
duplicate inventory UUIDs were accepted; the parser now rejects all three.

The first Windows ASan link failed because NVRHI used different MSVC STL
annotation settings from its instrumented consumers. Both NVRHI targets now use
the existing sanitizer helper, matching the established Catch2 treatment without
disabling annotations or imposing engine warning flags. The final full Windows
matrix and hardware checks were rerun against the repaired source identity.

The first WSL server Debug run failed one existing package fixture when reading
`/proc/<pid>/stat` raced process exit and raised `ProcessLookupError`. The narrow
test-observation repair accepts that precise disappearance alongside
`FileNotFoundError`, while preserving permission/other I/O failures. Deterministic
coverage tests both cases. The failed evidence is preserved and the fresh full
WSL matrix passes. No production cleanup behavior changed. Final source hashes
account for the repair; the final Windows and WSL runs use unchanged final sources.
An intermediate WSL run passed its tests but correctly failed its input-consistency
gate because the CMake repair landed during execution. That receipt remains FAIL.
The authoritative rerun reconfigures and builds the retained checkpoint objects,
reruns every suite, and uses fresh reports and a fresh Server package destination.

Native lifecycle evidence qualifies ownership and capability admission only.
It proves no swapchain, submitted-resource retirement, presentation recovery,
rendered image, performance budget or populated MMO world. Native driver calls
are not preemptible in-process; external watchdogs bound qualification runs.
Mac, native Linux graphics, additional hardware and hosted CI are outside the
current gate. Existing hosted workflows and branch policy are unchanged.

The new `client.graphics-device` coverage entry connects CPU and native fixtures
to REQ-05 without marking the whole requirement PASS. FeatureWorld attaches in
assignment 10; no component, authoring command or Lua surface is fabricated.
Planning documents preserve the seamless-MMORPG and equal human/terminal-agent
scope, with CPU authoring independent of graphics initialization.

`.gitignore` already covers generated builds, validation tools, evidence and the
local roadmap, so no new rule was needed. No staging, commit or push was performed.

Suggested commit: `feat(graphics): add owned Vulkan device foundation and validation`
