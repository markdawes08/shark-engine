# 02d Windows probe packaging and lifecycle evidence

Base revision: `b6aabd7` (`feat(graphics): add validated GPU cases and lifecycle probes`).
The changes remain uncommitted; the user owns staging, committing and pushing.
This is the available Windows portion of 02d, following the user-directed
[02c Mac deferral](02c-mac-deferred.md). Assignment 02 remains current. The full
checkpoint and assignment gate remain **NOT_RUN** pending the named hardware
matrix, native Mac work and real clean-machine launch.
The implemented local Windows scope is **PASS**; the
[durable receipt](../coverage/evidence/02d-windows-local.json) retains exact commands,
input hashes, native reports, readbacks, negative checks and prior failures.

## Delivered behavior

`graphics-windows-release` provides matching configure/build/test presets for a
redistributable probe. `tools/package_graphics_probe.py` constructs a fresh
package atomically from explicit local inputs. It requires Release/x64 without
sanitizers, verifies current native/shader/validation identities, recursively
inspects all six PE binaries and rejects unresolved DLLs and Debug CRT. The
package includes the loader, validation layer, four SPIR-V shaders, three pinned
Release CRT DLLs, license notices, source/toolchain provenance and file hashes.
It contains no DXC, SPIR-V validator executable or source shader. This is an
assignment 02 fixture, not an engine game exporter or release qualification.

`Run-GraphicsProbe.ps1` runs without Python, Visual Studio, Vulkan SDK or the
checkout. Windows PowerShell 5.1 and built-in .NET provide strict JSON parsing,
UTF-8 output and Win32 process containment. Creation assigns a private job
atomically, restricts handle inheritance, bounds streams and deadlines, and
terminates owned descendants on timeout or abrupt launcher exit. Package hashes
are checked before/after execution. Native results must match fixed readback
tolerances, artifacts, explicit UUID, validation settings and requested workload.
Elevated validation returns NOT_RUN; nothing changes global loader settings.

Native lifecycle failures now preserve completed counters, current phase and
cycle indices. A scoped clock captures presentation duration during unwinding,
before teardown can inflate it. Failed runs remain unqualified. Deterministic
clock tests cover retained failure progress and successful timing; the original
GPU deadlines, tolerances and 100/20/600 workload are unchanged.

The new CRT lock entry records actual version `14.51.36247.0`, toolset source
directory `14.51.36231`, hashes and license provenance. The
[Visual Studio 2026 redistribution list](https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution)
identifies the app-local Release files; the copied runtime EULA supplies end-user
terms, not a separate redistribution grant. The app-local pin requires explicit
servicing/rebuilds. No downloaded installer or machine-wide runtime was installed.
The package also retains the loader's distinct cJSON, loader JSON and Windows
directory-enumeration notices from the verified source archive.

## Local verification

Host: Windows 11 build 26200 x64, MSVC 19.51.36248, Windows SDK 10.0.28000.0,
CMake 3.31.10, Ninja 1.13.2 and Python 3.13.5. The final package binds dependency
lock `d3d4aed4861389f081232b7ed85f1ced6682192285717b943c4a694c871de5de`
and probe source `9d21bfd2bfd59e51d9737a6361f6a9d2eea6070563d17725c2be50a4ece3fd90`.

| Check | Observed result |
| --- | --- |
| Windows Debug, Release and AddressSanitizer | PASS, 20/20 CTests each, no skips |
| Graphics Release | PASS, 20/20 CTests, including actual Unicode native arguments and deterministic failure-progress tests |
| Package builder | PASS, 16 CPU cases including Debug/unknown DLL/shader/path/fresh-publication rejection; actual six-binary closure PASS |
| Windows portable launcher integration | PASS, 24 checks including real timeout, output bounds, arbitrary arguments, strict report types, Windows layer path and abrupt descendant cleanup |
| Isolated Ubuntu 24.04 headless workflow | PASS, 18/18 Debug plus 18/18 ASan/UBSan tests, Release source/link/package closure and separate pristine-runtime startup |
| Client runtime-only | PASS, configure/build and transitive boundary |
| Relocated Intel GPU cases | PASS, 14 readback checks and 11 artifacts, zero validation warnings/errors; short lifecycle only |
| Relocated NVIDIA full lifecycle | PASS, 100 resize/minimize/restore cycles, 20 device lifecycles, 100 actual zero-framebuffer observations, 600.0015573 presentation seconds, 14 readback checks and 11 artifacts; zero validation warnings/errors |
| Actual relocated negative checks | PASS, corrupted shader and missing CRT rejected, elevated validation NOT_RUN, one-second native deadline exits 124 with owned-tree cleanup |
| Package ZIP | PASS, all 52 manifest payloads and required empty layer directory verified |
| Formatting, contract and repository checks | PASS; 20 contract negative cases and skill/ignore boundary checks |

The relocated package runs from a fresh directory containing spaces and Chinese
characters outside the checkout, with another fresh working directory and only
Windows system paths on PATH. Development/SDK/loader override variables are
removed for the test. The same-user standard desktop helper is used only by the
development controller; the packaged PowerShell process does not require it.
The package has 52 payload files (47,813,249 bytes); the verified ZIP is
11,676,967 bytes. Package manifest SHA-256 is
`741c38812c365f188733dbea004900a37af4c23d8dd33035b0c356afbabea1b9`.

Reproduce build, preparation, package construction and portable execution with
the exact commands in the [probe guide](../graphics-probe.md), substituting fresh
output paths. Local preparation receipts use `out/graphics-validation/02d-windows-portable/summary.json`
and `out/graphics-tools/02d-windows-portable/shaders`; the package is
`out/graphics-package/02d-windows-portable`. Native launcher integration uses
`python tools/tests/check_graphics_package_launcher.py --output out/graphics-package-launcher-check05`.
Windows CPU workflows use `tools/ci.py --preset windows-debug|windows-release|windows-asan --jobs 2`
through `tools/windows_dev.ps1` (one preset per invocation). The full NVIDIA
request uses UUID `863d6f59a51894ab8b4829aa9460c78d` and
`-LifecycleCycles 100 -DeviceCycles 20 -PresentSeconds 600 -TimeoutSeconds 1200`.
Intel's short request uses UUID `868088a7040000000002000000000000` with defaults.
Evidence directories are immutable inputs to the handoff; reruns need fresh names.
The NVIDIA run completed at 2026-10-18 02:53 UTC after 667.275 seconds total,
recorded 240,417 serialized fixture frames and completed owned-process cleanup.
This frame count is workload evidence, not an MMORPG performance measurement.
Its P02-01 through P02-06 results pass locally; P02-07/P02-08 retain NOT_RUN in
the native report because packaging/failure qualification is assessed separately.
The Intel run passes P02-01 through P02-04 and P02-06; its short P02-05 is NOT_RUN.
Drivers were NVIDIA 610.62 (Vulkan 1.4.341) and Intel 101.6790 (Vulkan 1.4.311).

## Investigation and retained failures

The first NVIDIA full-workload attempt stopped with `window_closed` after all
100 resize/minimize/restore cycles and 223.014527 seconds of timed presentation.
The close-request origin was not observed. It is retained as FAIL, not counted
as a completed soak. Partial diagnostics show 100 actual zero-framebuffer
observations, zero completed device cycles and no validation messages. Nothing
relaxed the requested duration or hid that failure.

Actual package relocation exposed two independent problems that synthetic
process tests could not establish. The builder's forward-slash layer library
path was not treated as relative by the pinned Windows loader, resulting in
`LoadLibraryExW` error 87. The builder now retains a Windows backslash-relative
path, and a regression binds that behavior. The loader converts environment
paths to UTF-8 but its Windows directory scanner uses narrow CRT APIs; a native
CPU reproducer demonstrated failure on a Unicode directory under code page 1252
while wide enumeration succeeded. The probe's process-local UTF-8 manifest
addresses this path and argument boundary without changing system locale or
patching a dependency. See Microsoft's
[process code-page contract](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page).
The initial Unicode CLI assertion also needed to respect the probe report's
existing U+00xx byte-escaping policy: it now compares recovered bytes with exact
UTF-8 input. The writer is unchanged; both old-executable failure and corrected-
executable success are retained. No comparison tolerance was widened.

The earlier 02b resize timeout remains preserved and unexplained. Subsequent
successful resize cycles are evidence of those executions, not proof of its
root cause or a retroactive waiver.

## Scope and next work

The local RTX 4070 Laptop and Raptor Lake-S Mobile Intel GPU are supplemental;
they do not replace GTX 1660, RX 5500 XT or Tiger Lake Iris Xe rows. All nine
named rows remain open. Development-host relocation with a Windows-only PATH
does not prove tools are absent from the machine. The clean-machine gate still
needs an independent target. Mac code/graphs and Linux native GPU runs are not
qualified by these Windows results. No new hosted CI result is claimed.

Coverage adds `graphics.probe-package` and its CPU/native fixtures. Component,
game-authoring command and Lua arrays remain empty; FeatureWorld starts in 10.
The seamless MMORPG and equal human/terminal-agent plans remain intact. No
production rendering or authoring API was added. `.gitignore` covers generated
packages, caches, evidence and the local roadmap; source/notices/tests remain
eligible for version control.

Next remains assignment 02: finish the outstanding native matrix, deferred 02c
and clean-target package verification. Assignment 03 and dependent renderer work
have not begun.

Suggested commit: `feat(graphics): add portable Windows probe packaging and soak evidence`
