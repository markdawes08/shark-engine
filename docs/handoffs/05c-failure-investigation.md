# 05c: unresolved failure investigation

Date: 2026-10-18. Base: `cd97e87` on `main`, initially clean. Result: uncommitted
diagnostic artifacts, evidence and documentation; the user owns staging,
committing and pushing. **05c remains FAIL, full REQ-05 remains NOT_RUN, and 06
cannot begin.** No safe production correction was established in this investigation.

The [original handoff](05c-presentation-qualification.md) and
[original receipt](../coverage/evidence/05c-local.json) retain the failed stress
runs and post-close timeout without modification. The
[follow-up receipt](../coverage/evidence/05c-investigation.json) binds the new
observations to source, executable, validation, driver and raw-log identities.
Successful diagnostic execution is not passing qualification. The existing
[workload, baselines and limits](../presentation-qualification.md) are unchanged.

## NVIDIA desktop device-context retention

The failure is reproduced outside Shark, GLFW and NVRHI. A standalone raw
Win32/Vulkan program creates and destroys eight fresh windows, instances,
surfaces, devices and swapchains. It never acquires an image, submits commands or
presents. Each explicit GPU selection runs separately as a standard user with
required pinned core/synchronization validation. The build links only the pinned
Vulkan loader and Windows libraries. The source and explicit link command
establish independence; DLL import inspection alone cannot exclude static code.

On NVIDIA RTX 4070 Laptop, UUID `863d6f59a51894ab8b4829aa9460c78d`, driver 610.62,
closed GDI counts increase by one on every lifetime. Intel's corresponding GDI
counts stay flat. Both minimal runs report zero validation warnings/errors.
This reproduces the original growth without Shark resource ownership or rendered
work. It does not qualify either GPU for the full production workload.

Separate bounded instrumentation observes each NVIDIA swapchain creation calling
the driver's imported `GetDC(NULL)`. Four lifetimes return four distinct desktop
DCs, each still a live `OBJ_DC` immediately after its corresponding lifetime's
complete teardown; GDI counts rise 10 to 13. The observer does not resample all
four handles together after the final lifetime. A separate uninstrumented
two-second post-close message-pump experiment also retains the GDI increments.
All ten inspected import slots are verified against the real exports and restored
before driver unload. No `ReleaseDC` is observed through the driver's replaced
static import slot. That observation alone cannot exclude dynamic calls or calls
through another DLL.

An independent Windows-only control acquires and releases only its own desktop
DCs on the same thread. Eight matched acquisitions/releases repeatedly return
GDI counts from one to zero; the released handles report no object type and are
reused. Holding eight owned DCs instead produces eight distinct live objects and
counts one through eight; releasing them returns the count to zero. This
distinguishes the driver's retained objects from ordinary successfully released
common DCs. No captured driver-owned handle is released or deleted by a test.

Read-only call-site inspection identifies the observed return RVA `0xE1781A`
inside `nvoglv64.dll` version `32.0.16.1062`, SHA-256
`4bddd0139e1641fe6984580e4ae827ef0d91e442c113cf37d0ae803aee9e690f`.
The routine resolves `opengl32.dll!wglDescribePixelFormat`, obtains `GetDC(NULL)`,
passes it to that function, then returns without storing or releasing the DC.
Together with the independent reproducer and owned-DC control, this supports a
driver-side desktop-DC retention diagnosis for this exact installed binary.
It does not establish behavior of other driver versions or systems.

Microsoft documents that `GetDC(NULL)` obtains a screen DC and that common DCs
must be released by the acquiring thread using `ReleaseDC`.
[GetDC](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdc),
[ReleaseDC](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-releasedc).
The returned driver-private HDC is not exposed through Vulkan swapchain ownership.
No documented safe application workaround was found. Driver updates or host
settings were not changed, and no driver binary, import hook or foreign-handle
cleanup is introduced into production.

Two controlled alternatives did not resolve the growth: removing `CS_OWNDC`
from an isolated copy of GLFW, and destroying the HWND before the Vulkan instance
after its surface/device were gone. Each alternative ran eight unsubmitted and
eight presented lifetimes; each closed sequence remained 10 through 17. The
shared GLFW source, dependency lock and production teardown were left unchanged.

## Intel USER-resource investigation

The original Debug failure was 35 USER objects against a baseline of 34 at the
second restore. Snapshots targeting 0, 16, 50, 100, 250, 500 and 1,000 milliseconds
after restores confirm a transient component, but also continuing settled growth.
The raw records retain actual elapsed times, including inventory overhead.
A second instrumented run enumerates all process threads' top-level, child and
message-only windows while exercising 100 resizes and 20 minimize/restore pairs.

Seven HWNDs remain constant while settled USER counts rise from 30 to 58. At
the final state, counts fall from 59 to 58 and are still 58 at the five-second
observation. After `GraphicsDevice::close()`, before C++ owner destruction, all
ten engine-owned resource counters are zero and validation is clean, but USER
counts fall from 50 to 45 and are still 45 at the five-second observation.
Only three HWNDs remain: `CicMarshalWndClass`, `OleMainThreadWndClass` and
`SystemUserAdapterWindowClass`. GDI counts after close are 10.

A Vulkan-free Window-only control uses the same 100 resize/20 minimize-restore
workload and observation schedule. USER counts settle at 17 after each restore,
remain 17 during the final five seconds, and fall to four after close; GDI counts
fall to nine. One system-adapter HWND remains. Thus ordinary GLFW window activity
does not reproduce the persistent growth seen in the graphics run. HWND
enumeration does not identify all USER object types, so the remaining objects'
allocation owner and a safe correction are still unknown.

The fixed observation pauses reduced the copied graphics soak's periodic samples
to ten rather than the required eleven. That copied soak remains incomplete and
does not pass the production gate. Its 165 fixed diagnostic observations remain
usable for investigation. Neither a longer automatic wait nor a new baseline or
larger USER allowance is justified by this evidence.

## Intel ASan post-close timeout

The original child exceeded its 30-second deadline after the `closed` record,
which follows all native teardown calls and zero owned counters. The subsequent
C++ owner/member and process/CRT/DLL exit stages were not separately recorded.
An audit confirms the runner uses output files rather than waiting for inherited
pipe EOF. The process chain includes the venv Python redirector, actual Python
worker and native child.

Forty-eight fixed diagnostic children all exited with their exact expected codes
in 0.705 to 1.047 seconds. These include the current executable, a copy with owner,
main, `atexit` and static-destruction markers, and the exact preserved old
executable run in the original recording-error/recording-exception/surface-error/
device-error order. All eight instrumented children reached every exit marker;
all observed Python workers also exited. No relevant bounded-window Windows
application-hang/error or display-driver event was found.

A separate bounded wait-chain/minidump capture was validated on an owned CPU
sleeper. Its fixed eight-second trigger was never reached by a native diagnostic,
so there is no dump of the original hang. The original deadline remains 30 seconds.
Because the old executable also passes these diagnostics, neither the prior
observer-lifetime correction nor these new runs establish the original timeout's
cause. It remains unresolved and its failure is not replaced by later passes.

## Delivered artifacts and verification

The [minimal reproducer and invocation](../coverage/reproducers/README.md) are
versioned for repeatable diagnosis. They are deliberately outside production
CMake targets and never patch driver imports. The follow-up receipt includes the
controlled experiments, fixed Intel observations and ASan audit, with raw local
receipts retained under `out/05c-close/`, `out/05c-closeout/` and `out/05c-work/`.
Read-only driver disassembly and native dumps remain local diagnostic artifacts.

Production source, dependencies, public contracts, runtime packaging and test
budgets are unchanged. There are no new component, command, Lua or FeatureWorld
surfaces. `.gitignore` was reviewed: generated diagnostics remain under ignored
`out/`, and the source reproducer and project evidence remain eligible for Git.

The original full Windows/WSL CPU and package evidence remains historical. No new
engine build matrix, Release/ASan sustained soak, WSL regression or package
qualification is claimed by this documentation/diagnostic change. The new native
reproducer is compiled with MSVC warnings as errors and checked on both GPUs;
repository contract, self-test and formatting results are recorded in the
follow-up receipt.

Commands run from the repository root through `tools/windows_dev.ps1`:

| Command | Result |
| --- | --- |
| `python docs/coverage/reproducers/05c-win32-vulkan.py --output out/05c-close/raw-vulkan-final` | DIAGNOSTIC_ONLY; eight lifetimes per GPU, zero validation/cleanup diagnostics, eight parser corruptions rejected per trace |
| `python tools/validate_contract.py --self-test` | PASS; 29 negative checks, 50 requirements/46 assignments, document links |
| `python tools/check_repository.py --self-test` | PASS; skill/ignore boundaries and three negative metadata checks |
| `python tools/check_format.py` | PASS; 84 engine-owned files; reproducer separately passed pinned formatter dry-run |
| `git diff --check` | PASS |

The reproducer's device-free `--help` check passed; an output outside `out/` and
reuse of the existing evidence directory both correctly returned exit code 2.
The original native receipt remained unchanged during these negative checks.

The next work remains **05c**. NVIDIA's exact driver path needs a correction
before its unchanged zero-growth gate can pass. Intel USER-object ownership and
the original ASan exit timeout still need resolution. After actual corrections,
execute the complete fixed Debug/Release/ASan stress and fault matrix, applicable
regressions and package checks with new source-bound evidence. Assignment **06,
shader pipeline and basic GPU resources**, remains planned.
