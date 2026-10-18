# 02d portable packages and final local verification

Base revision: `39110d4619fdcc8340b21409172e484af416c980` on `main`.
Result: uncommitted changes; the user owns staging, committing and pushing.
Assignment 02 remains current. The user confirmed that only the current Windows
PC is available. Mac is outside scope under
[ADR 0009](../architecture/0009-windows-linux-platform-scope.md).

## Scope

02d now includes a Linux Release package builder and standalone terminal launcher,
alongside the existing Windows package. Both package types include the pinned
Vulkan loader, runtime validation layer, precompiled fixture shaders, dependency
notices and input provenance. These are graphics qualification fixtures, not a
game exporter, production renderer or editor UI.

The Linux fixture uses Ubuntu 24.04 x86-64, X11 and distro runtime libraries,
with Python 3.12+ for its standard-library-only launcher. Packaging inspects ELF
imports and symbol versions, rejects build-tree RPATHs and source compilers, and
publishes only complete verified payloads. The launcher checks package hashes,
isolates loader settings, validates native reports and readback artifacts, bounds
output/process lifetime and cleans up its owned process group on handled exits.
Its ordinary process group is not a container; deliberate session escape,
SIGKILL of the launcher or machine loss remain outside its cleanup guarantee.

The first actual Linux graphics link failed because the pinned static NVRHI
Vulkan backend did not declare its dependency on the common NVRHI archive.
`cmake/GraphicsDependencies.cmake` now declares that edge; the GNU link succeeds.
This changes the integration graph without patching upstream source or changing
dependency versions. No shader, native probe behavior, tolerance or lifecycle
budget is changed.

Actual Linux validation preparation produced a 601,149,560-byte debug layer,
larger than the existing bounded consumers accept. A Linux-only `vvl` final-link
option in `cmake/GraphicsValidation.cmake` removes debug sections, yielding
38,291,848 bytes. RelWithDebInfo compilation, debug object files, validation
features and dependency pins remain intact; no byte limit was raised. The fresh
preparation receipt binds the changed CMake source and resulting library hash.
Windows preparation retains its original recorded inputs and unchanged layer;
the new conditional does not affect Windows linking.

The new Linux suite exercises actual child processes, output limits, timeouts,
descendant cleanup and SIGINT/SIGTERM. It found that a selector could swallow an
`InterruptedError` raised by the signal handler. A dedicated interruption type now
reaches bounded cleanup; subsequent signals are ignored during teardown. The
fixtures exercise actual single-signal SIGINT and SIGTERM cancellation and
descendant cleanup; repeated-signal behavior has not been independently tested.

## Hosted evidence

[Run 39092472102](https://github.com/markdawes08/shark-engine/actions/runs/39092472102)
passed all nine jobs and 156 CTests with no CTest skips at the base revision.
The [durable audit](../coverage/evidence/02d-hosted-ci.json) verifies all eight
artifact ZIP digests, toolchains, runtime-only and headless package closures,
the clean headless runtime smoke and the nine required GitHub checks.

Earlier `21239e7` and `b6aabd7` runs failed only their six then-required Mac jobs:
strict input-path fixtures rejected symlinked temporary paths. Their Windows/Linux
jobs passed. Those failures remain historical evidence; no Mac repair or retest
is claimed. The newly added Linux packaging code and link fix are local changes
and require the user's push for a new hosted run.

## Local verification

Windows host: Windows 11 build 26200 x64, MSVC 19.51.36248, Windows SDK
10.0.28000.0, CMake 3.31.10, Ninja 1.13.2 and Python 3.13.5. The current dependency
lock is `1231da5703cd19d9da35a03e623815fff70f82a8aca72c33421d631c2249fc56`;
probe-source digest is
`9d21bfd2bfd59e51d9737a6361f6a9d2eea6070563d17725c2be50a4ece3fd90`.
The shared NVRHI link edge changed the Windows binary, so the final package was
rebuilt and both GPUs are checked against that new identity. The successful
intermediate NVIDIA run is retained separately and is not substituted for it.

| Check | Observed result |
| --- | --- |
| Windows Debug CPU suite | PASS, 21/21 CTests |
| Windows graphics Release suite | PASS, 21/21 CTests after explicit regeneration |
| Windows runtime-only and server-only | PASS, configure/build and actual dependency-boundary targets |
| Isolated Ubuntu 24.04 graphics Release | PASS, GNU build and 21/21 CTests |
| New Linux packaging/launcher suite | PASS, 20/20 methods on Ubuntu; Windows executes 10 portable methods and explicitly skips 10 Linux-only methods |
| Windows package construction and archive | PASS, 52 payload files, recursive PE closure, hashes and required empty layer directory |
| Windows relocated negative fixtures | PASS, corrupt shader and missing CRT rejected; elevated validation correctly unavailable |
| Final relocated NVIDIA lifecycle | PASS, 100 resize/minimize/restore cycles, 20 device lifecycles, 100 actual zero-framebuffer observations and 600.0008163 presentation seconds; 14 readback checks/11 artifacts, zero validation warnings/errors |
| Final relocated Intel lifecycle | PASS, 100 resize/minimize/restore cycles, 20 device lifecycles, 100 actual zero-framebuffer observations and 600.0028867 presentation seconds; 14 readback checks/11 artifacts, zero validation warnings/errors |
| Actual Windows package watchdog | PASS, one-second native deadline returns 124 and completes owned-process cleanup |
| Linux package construction and archive | PASS, 51 payload files, three bundled ELF files inspected, notices and provenance retained |
| Separate SDK-free Ubuntu runtime userspace | PASS, unprivileged Unicode/space relocation and describe; corrupt shader/missing loader rejected; missing-display inventory correctly NOT_RUN |
| Formatting, contract, repository and CI configuration | PASS, including 20 contract negative cases and ignore/skill checks |

The [Windows refresh receipt](../coverage/evidence/02d-windows-refresh.json)
retains the exact package/controller commands, standard-user token evidence,
native reports, lifecycle counters and raw artifact hashes. Both final runs pass
P02-01 through P02-06 on the supplemental GPUs. The collector rehashed all 22
retained readbacks and the archive before publication; no earlier binary's
result is counted as final-package evidence.
Neither CTest suite has a skipped CTest entry; the Windows Python-method skips
above remain visible and do not count as Linux execution.

Windows build and preparation commands used the repository developer wrapper:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python tools/ci.py --preset windows-debug --jobs 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --preset graphics-windows-release
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset graphics-windows-release --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset graphics-windows-release --output-on-failure --output-junit C:/Users/mrkda/Projects/shark/out/02d-final-work/windows-graphics-current-ctest.xml
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python tools/prepare_graphics_validation.py --offline --output out/graphics-validation/02d-final --jobs 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python tools/prepare_graphics_tools.py --offline --cases --validation-manifest out/graphics-validation/02d-final/summary.json --output out/graphics-tools/02d-final
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python tools/package_graphics_probe.py --executable build/graphics-windows-release/bin/GraphicsProbe.exe --build-dir build/graphics-windows-release --shader-dir out/graphics-tools/02d-final/shaders --validation-manifest out/graphics-validation/02d-final/summary.json --crt-dir "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Redist/MSVC/14.51.36231/x64/Microsoft.VC145.CRT" --output out/graphics-package/02d-final-linked
```

Both isolated boundary presets used `cmake --preset runtime-only` /
`cmake --preset server-only`, followed by matching `cmake --build --preset ...
--parallel 2`, through the same wrapper. Formatting used the wrapper to select
the locked formatter; contract/repository checks used `--self-test` and
`tools/check_ci_config.py` checked the nine-job YAML.

The relocated Windows package lives outside the checkout under
`%LOCALAPPDATA%/Temp/Shark 02d Linked 20261018 測試/package`, with a separate
working directory and only Windows system paths on PATH. The controller removes
SDK/development/loader overrides and starts the packaged PowerShell launcher
with a same-user standard desktop token. That Python controller is a test tool;
the actual Windows package requires no Python. The
[probe guide](../graphics-probe.md) documents the standalone user commands.
The final package has 52 payload files totaling 47,812,147 bytes; manifest SHA-256
is `0bd36726f65567a1b0f9dd2dac60ed882c49708e8e8d3d1fc6a129188db9beee`.
The probe executable hash is
`a8532e809eb9695ac8fc2aebf049a1d1d2ec4c321f1de2c1294d4107518ccb6c`.
The verified ZIP is 11,673,367 bytes, SHA-256
`33fa3a129395201aa01f9950c2ba48147e20e13ab10cb0c25487e0063436f63f`.
The supplemental devices are NVIDIA RTX 4070 Laptop, driver 610.62 / Vulkan
1.4.341, UUID `863d6f59a51894ab8b4829aa9460c78d`, and Intel Raptor Lake-S
Mobile, driver 101.6790 / Vulkan 1.4.311, UUID
`868088a7040000000002000000000000`. Full package requests use
`-LifecycleCycles 100 -DeviceCycles 20 -PresentSeconds 600 -TimeoutSeconds 1200`.
P02-07/P02-08 remain NOT_RUN in native reports because package/failure gates
are assessed separately. Fixture frame counts are workload evidence, not MMO
performance measurements.

Linux compilation used a fresh isolated Ubuntu 24.04 userspace under the shared
WSL kernel, with exact signed-snapshot package inventories. The previously
qualified pure headless root was left unchanged. Build configuration explicitly
selected `/usr/bin/gcc-13` and `/usr/bin/g++-13`; a fresh configuration established
the actual Release flags after an initial compiler-name failure. The original
GNU link failure and unstripped layer identity are preserved with the repaired
results. Validation and shaders were prepared natively, including negative
shader/SPIR-V checks, before package construction.

The separate runtime root starts from the locked Ubuntu base, has distro Python
and runtime libraries but no compiler, CMake, Ninja, Git, DXC, Vulkan SDK or source
checkout. The actual package runs as UID 65534 from a Unicode/space directory and
an unrelated working directory. It has no display or GPU devices: the native
inventory exits 3 with NOT_RUN, rather than becoming simulated graphics evidence.
The complete package, runtime inventory and exact reproduction commands are
retained in the [Linux receipt](../coverage/evidence/02d-linux-local.json).
The Linux package has 51 payload files totaling 42,411,362 bytes; its manifest
SHA-256 is `896e8470e23548f3d41689c22d3e25df8461e1bf81ca9fdfe11ca0a964b2a7a8`.
The verified archive is 11,499,587 bytes, SHA-256
`53be7bc4521f01833d6772a70419541f62886f57496475bb398d5a5d062b0805`.
Preparation receipts use `out/graphics-validation/02d-final-linux03/summary.json`
and `out/graphics-tools/02d-final-linux`; package output is
`out/graphics-package/02d-final-linux`. The receipt embeds exact native commands,
source and artifact hashes, package/environment inventories and reproduction
scripts; the [guide](../graphics-probe.md) provides the reusable native workflow.

Coverage extends `graphics.probe-package` with the Linux test and implementation
paths. No component, authoring-command or Lua API was invented; FeatureWorld
remains assignment 10. Both contributor workflows are noninteractive and return
structured evidence. The seamless MMORPG and shared human/agent design are
unchanged. Existing `.gitignore` rules already cover packages, caches, logs and
the amended local roadmap; new source, tests and durable records remain visible.

## Retained resize investigation

Reviewed the preserved `02b-nvidia-stress-1` failure and current lifecycle
predicates. The old run requested 100 resize cycles, 20 device cycles and two
presentation seconds. It exited 1 after 20.048 seconds with `window_timeout`,
before the 300-second process deadline, with zero validation warnings/errors and
no process cleanup errors. The archive does not identify the failed cycle or its
requested/observed dimensions. Its `120x64` extent is initial swapchain creation,
not the failing resize, so it cannot establish the cause.

The current probe already preserves cycle/dimension context, partial lifecycle
counters and presentation time, and waits for restored logical size, positive
framebuffer and cleared iconification. Subsequent successful runs establish those
executions only. The original failure remains preserved and unexplained; no
deadline was widened and no failure was retroactively waived.

## Remaining gate and next assignment

The six required native rows W-NV/W-AMD/W-INTEL and L-NV/L-AMD/L-INTEL remain
NOT_RUN as complete hardware gates. The local RTX 4070 Laptop and Raptor Lake
Intel GPU do not substitute for the named GTX 1660, RX 5500 XT and Tiger Lake
Iris Xe baselines. Ubuntu userspace under WSL is not a native Linux GPU result.
Actual graphical package execution on an independent clean target also remains
open. The user has no additional machine available for these checks.

After the user's commit/push, verify the new nine-job hosted run for these local
changes. Continue 02d with the native hardware and clean-target runs when access exists;
assignment 03 remains next only after the retained gate passes or the user
explicitly changes it. Mac work is not a prerequisite.

Suggested commit: `feat(graphics): add Linux probe packages and refresh 02d evidence`
