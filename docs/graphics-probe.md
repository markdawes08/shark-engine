# Graphics feasibility probe (02a / 02b)

The opt-in `GraphicsProbe` is a developer fixture using GLFW, NVRHI and Vulkan.
It inventories devices, creates the GLFW/Vulkan/NVRHI stack, executes analytic
graphics/compute/readback cases and exercises bounded presentation lifecycle.
The JSON distinguishes individual case execution from the complete hardware gate.
Full assignment qualification remains `NOT_RUN`; Mac feasibility, the nine named
hardware rows and packaged launch are separate checkpoints. The [checkpoint plan](roadmap/graphics-plan.md)
and [platform contract](architecture/0003-platforms.md) retain the full gate.

## Build

Use the pinned tools and native compiler environment in [building](building.md).
The normal CPU, Runtime and Server presets leave `SHARK_BUILD_GRAPHICS_PROBE=OFF`.
Only the explicit graphics preset downloads/builds graphics inputs. For Windows:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --preset graphics-windows
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset graphics-windows --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset graphics-windows --output-on-failure
```

Use `graphics-linux` or `graphics-macos` on those native systems. Linux requires
X11 development headers/libraries for the GLFW build; Wayland is disabled.
Mac requires Xcode/Cocoa development tools and a separately qualified Vulkan ICD.
Presets establish build commands, not evidence that every native build has run.
Source archives and options come from [the dependency lock](../dependencies.lock.json),
including a source-built Vulkan loader with matching headers. An arbitrary
installed SDK is not selected instead. Source overrides are rejected before
dependency population. OS SDK/display libraries and installed vendor ICDs remain
platform inputs and must be identified by actual evidence.

Graphics dependencies are private to this fixture. EngineClient/Runtime still
provide their existing bootstrap behavior. Shader compilers are offline tools
under `.tools/graphics`, never linked into Core, Server or Runtime.

## Noninteractive use and results

Direct informational commands need no display or Vulkan device:

```text
build/graphics-windows/bin/GraphicsProbe.exe --help
build/graphics-windows/bin/GraphicsProbe.exe --version
build/graphics-windows/bin/GraphicsProbe.exe --describe
```

`--describe` emits JSON schema 2, its compiled dependency-lock and probe-source digests, available
modes and the unqualified case list. `--help`/`--version` emit human text.
Machine modes reserve stdout for one JSON document; diagnostics go to stderr.
Use the bounded runner for device work, with a new output directory each time:

```text
python tools/graphics_probe.py --executable build/graphics-windows/bin/GraphicsProbe.exe --build-dir build/graphics-windows --mode inventory --output out/graphics/inventory-01
python tools/graphics_probe.py --executable build/graphics-windows/bin/GraphicsProbe.exe --build-dir build/graphics-windows --mode smoke --device DEVICE_UUID_FROM_INVENTORY --output out/graphics/smoke-01
```

Replace the device placeholder with its exact 32-character lowercase hexadecimal
Vulkan UUID. There is no implicit first-device selection. On Linux/macOS use the
matching build path and omit `.exe`. Inventory creates a hidden GLFW window and
surface, enumerates actual device/driver/features/limits/formats and then exits;
it does not create a logical device. Smoke selects the explicit device and tests
setup/teardown only. No interactive acknowledgement or LLM integration is needed.

Smoke requires `VK_LAYER_KHRONOS_validation` by default. Missing validation is
`NOT_RUN` with exit 3. `--validation off` explicitly allows a diagnostic startup
run without that layer; it never supplies validation or hardware qualification.
Prepare the pinned validation tools below and supply their receipt to the runner.
Required validation uses the selected local layer with core and synchronization
validation enabled; no warning/error waiver is applied.
NVRHI's validation wrapper remains part of smoke, but cannot replace Vulkan
validation. Do not install or replace a user's GPU driver as an automatic repair.

The runner applies a 120-second default deadline (`--timeout 1..1200`), owns process
tree cleanup and refuses an existing output directory. It saves the command,
timestamps, source/worktree fingerprints, executable and lock hashes, compiler
metadata when supplied, stdout, stderr and `summary.json`. It rejects stale lock
or probe-source digests, missing/mismatched stages or device selection,
malformed/contradictory reports and false qualification claims. The source digest
binds the probe's CMake file, owned C++ headers/sources, HLSL fixtures and Windows
process manifest; retained configure/build
commands and source fingerprints still supply the surrounding build provenance. Run it
from the configured compiler/tool environment for complete tool metadata. A raw
executable invocation has no external driver-call watchdog; use the runner in
automation. Serialize builds and commands sharing a build directory.

| Exit | Meaning |
| --- | --- |
| 0 | Requested discovery, setup or executable cases succeeded; full gate remains NOT_RUN |
| 1 | Observed operation failure or rejected evidence |
| 2 | Invalid arguments |
| 3 | Device/display/driver/validation prerequisite unavailable; NOT_RUN |
| 124 | Runner deadline exceeded; no passing setup claim |

`shark.graphics-probe.cli` exercises CPU-safe informational/error output with
device discovery deliberately unavailable. `shark.tooling.graphics-evidence`
rejects malformed, stale and contradictory evidence. `shark.tooling.graphics-dependencies`
checks early headless/probe conflict and source-override rejection without downloads.
`shark.graphics-probe.report` checks JSON escaping and integer limits in the
same Catch2 test target. These CPU tests are not GPU case execution.

## Offline validation tools and executable cases

The initial tool input is DXC 1.9.2609, using the official Windows/Linux archives
and hashes in the same dependency lock. Run the explicit preparation once, then
use the offline cache without downloading:

```text
python tools/prepare_graphics_tools.py --output out/graphics-tools/setup-01
python tools/prepare_graphics_tools.py --offline --output out/graphics-tools/setup-02
```

On Windows use the compiler wrapper from the build example to additionally record
`dumpbin` dependency inspection. Linux records native `ldd` and package versions
when those inspectors are available. Each output directory must be new.
The preparation helper preserves licenses,
checks cached bytes and records actual compilation results for the setup fixture.
No compiler download occurs in ordinary configure/build/test or at Runtime launch.
Build the independent validator and validation layer from their complete locked
source graph. This is a separate development project, never an engine dependency.
The first build can take several minutes; use the Windows compiler wrapper above.
After population, `--offline` verifies and reuses the same archives and source bytes.
The layer is selected only in the child process; no registry or system installation
is performed.

```text
python tools/prepare_graphics_validation.py --jobs 2 --output out/graphics-validation/build-01
python tools/prepare_graphics_tools.py --offline --cases --validation-manifest out/graphics-validation/build-01/summary.json --output out/graphics-tools/cases-01
python tools/graphics_probe.py --executable build/graphics-windows/bin/GraphicsProbe.exe --build-dir build/graphics-windows --mode cases --device DEVICE_UUID_FROM_INVENTORY --shader-dir out/graphics-tools/cases-01/shaders --validation-manifest out/graphics-validation/build-01/summary.json --timeout 180 --output out/graphics/cases-01
```

Run these GPU commands from a standard-user terminal on Windows. The Vulkan loader
intentionally ignores `VK_LAYER_PATH` at high integrity; the runner detects this
and returns `NOT_RUN` before launch. An elevated automation terminal can use the
bounded helper below. It verifies the same user's standard desktop token and the
child's actual integrity, launches without elevation, and owns child-tree cleanup.
It does not change registry entries, credentials, privileges or loader safeguards.
An unavailable standard desktop token is reported with exit 3.

```text
python tools/windows_unelevated.py --timeout 220 --evidence out/graphics/cases-01-token.json -- python tools/graphics_probe.py --executable build/graphics-windows/bin/GraphicsProbe.exe --build-dir build/graphics-windows --mode cases --device DEVICE_UUID_FROM_INVENTORY --shader-dir out/graphics-tools/cases-01/shaders --validation-manifest out/graphics-validation/build-01/summary.json --timeout 180 --output out/graphics/cases-01
```

Use a fresh token receipt path and allow the outer deadline enough time for the
runner's metadata collection and cleanup. Already standard-user terminals use the
same helper without changing identity; Linux and macOS run the probe runner directly.
When combining it with the compiler wrapper in PowerShell, pass an explicit array:
`& ./tools/windows_dev.ps1 -CommandArguments @('python', 'tools/windows_unelevated.py', ... , '--', 'python', 'tools/graphics_probe.py', ...)`,
replacing the ellipses with the arguments above. The ordinary `-File` wrapper form
cannot forward that literal `--` correctly. The direct Python command above is
otherwise sufficient to run the already built probe.

The opt-in Windows integration harness checks actual standard identity, exact
arguments/streams/exit codes, timeout and descendant cleanup, including abrupt
launcher termination. It needs a logged-in standard desktop when the caller is
elevated, so it is separate from ordinary CPU CTest:

```text
python tools/tests/check_windows_unelevated.py --output out/process-launch/check-01
```

Preparation compiles each of the four entrypoints twice with locked DXC, requires
identical bytes, validates every result with pinned `spirv-val --target-env vulkan1.3`,
and checks normal rejection of invalid HLSL and corrupt SPIR-V. The runner verifies
tool/shader manifests, hashes, source freshness and retained readback bytes. An
existing evidence directory is rejected. Source shader compilation never happens
in the probe or Runtime.

The runner's `--mode cases` defaults to two resize/minimize/restore cycles, one device creation and
no timed soak. It requires explicit shaders and device selection. Raw native use
also requires `--artifacts-dir`; the runner supplies a fresh directory automatically.
`--lifecycle-cycles 0..100`, `--device-cycles 1..20` and `--present-seconds 0..600`
control bounded work. Full P02-05 requires 100 resize/minimize/restore cycles, 20
create/destroy cycles, an actually observed zero framebuffer extent and 600 seconds
of presentation. Passing a shorter workload leaves that case `NOT_RUN`. A CPU
fixture separately verifies that zero/minimized framebuffers cannot present; an
OS retaining a nonzero framebuffer while minimized is recorded honestly.

```text
python tools/graphics_probe.py --executable build/graphics-windows/bin/GraphicsProbe.exe --mode cases --device DEVICE_UUID_FROM_INVENTORY --shader-dir out/graphics-tools/cases-01/shaders --validation-manifest out/graphics-validation/build-01/summary.json --lifecycle-cycles 100 --device-cycles 20 --present-seconds 600 --timeout 1200 --output out/graphics/lifecycle-01
```

The small native window appears during lifecycle tests. Presentation serializes a
Vulkan attachment clear, fences and queue/device idle; actual draw/compute cases
use NVRHI. This deliberately simple fixture does not provide production frames
in flight. Acquire uses a host fence and submission completion precedes present;
there are no recycled present semaphores. Portability of swapchain destruction
and the maintenance-extension route remain part of 02c/05. See the
[Khronos synchronization guidance](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).

P02-02 verifies foreground/background, conventional depth, counterclockwise
culling, 16/32-bit indices, UV orientation and sRGB decoding. P02-03 transforms 256
integers exactly, writes RGBA16F storage values above one and consumes them in a
graphics draw. P02-04 exercises mandatory formats/usages, alpha blending, linear
filtering, cube faces/mips and every rendered/sampled layer of a 2048x2048x12 D32
array. References are analytic, with explicit edge exclusions; no image baseline
is generated or approved. Limits remain ADR0003's 1/255 UNORM, 2e-3 absolute or
relative HDR, and 1e-5 controlled depth.

NVRHI's pinned backend flips viewport height internally. The fixture supplies
reversed Y bounds to that adapter, yielding a positive native Vulkan viewport,
then negates projection Y once in HLSL. The real culling/UV readbacks verify this
choice; there is no third-party source patch.

These readbacks measure sRGB input decoding. The presentation attachment is
configured for sRGB output, but its encoded pixels are not read back here;
numerical verification of the output transfer remains with assignment 25.

`--inject-failure missing-feature|shader|readback` exercises explicit missing
capability rejection, a missing shader artifact or a deliberately incorrect
readback reference. Each must fail nonzero; these flags cannot qualify a device.
Absent UUIDs, unavailable validation, corrupted manifests/artifacts and a real
nonresponsive process are covered separately. `--validation off` retains measured
execution results but leaves every case status `NOT_RUN`. Case statuses are never
a claim that a named hardware matrix row or the complete assignment passed.

## Native Mac follow-up

The user deferred 02c while working on Windows; see the
[decision record](handoffs/02c-mac-deferred.md). The Mac rows stay NOT_RUN.

The official DXC release supplies no native Mac binary. Native Mac shader-tool
preparation remains NOT_RUN and requires a separately pinned source/toolchain
route. KosmicKrisp (Apple Silicon/macOS 26) and MoltenVK (Intel Macs) remain the
ADR's candidates. Their complete build dependencies, validation, maintenance and
redistribution need the native 02c evidence. Do not invoke floating upstream fetch
scripts or substitute a system package graph. See the
[DXC release](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609),
[KosmicKrisp guide](https://docs.mesa3d.org/drivers/kosmickrisp.html) and
[MoltenVK sources](https://github.com/KhronosGroup/MoltenVK/tree/v1.4.2).

The pinned NVRHI backend and probe share Vulkan-Hpp compile definitions, including
`VK_ENABLE_BETA_EXTENSIONS` to expose the official portability-subset queries.
This keeps their dispatcher layout consistent; it does not enable an experimental
Metal driver feature. No NVRHI source patch or production renderer API is introduced.

## Standalone Windows probe package

02d supplies a Windows 11 x64 probe package for hardware verification. It is not
a game export. Configure/build with `graphics-windows-release`; the package
builder rejects Debug, sanitizer, stale-input and unresolved-DLL configurations.
Use the Windows developer wrapper for these build/construction commands:

```text
cmake --preset graphics-windows-release
cmake --build --preset graphics-windows-release --parallel 2
ctest --preset graphics-windows-release
python tools/package_graphics_probe.py --executable build/graphics-windows-release/bin/GraphicsProbe.exe --build-dir build/graphics-windows-release --shader-dir out/graphics-tools/cases-01/shaders --validation-manifest out/graphics-validation/build-01/summary.json --crt-dir "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Redist/MSVC/14.51.36231/x64/Microsoft.VC145.CRT" --output out/graphics-package/package-01
```

The explicit CRT directory must match `graphics_distribution.WindowsCRT` in the
dependency lock. Changing a locally installed toolset requires an intentional
pin/update and rebuilt evidence, not a broad DLL search or automatic download.
The package contains the Release probe, locked Vulkan loader, pinned validation
layer, three Release CRT DLLs, four prevalidated SPIR-V shaders and redistribution
notices. It excludes DXC, SPIR-V validator executables, source shaders and SDKs.
The builder recursively inspects PE imports, including the dynamically selected
validation DLL, against packaged files and an explicit Windows 11 system list.
Driver ICDs remain host prerequisites. App-local CRT servicing requires rebuilding
the package when its reviewed pin changes; see Microsoft's
[redistribution list](https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution).

Copy the complete package outside the checkout. From an ordinary standard-user
Windows PowerShell terminal, run its launcher with a fresh output directory:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ./Run-GraphicsProbe.ps1 -Mode describe -Output C:/probe-results/describe-01
powershell -NoProfile -ExecutionPolicy Bypass -File ./Run-GraphicsProbe.ps1 -Mode inventory -Output C:/probe-results/inventory-01
powershell -NoProfile -ExecutionPolicy Bypass -File ./Run-GraphicsProbe.ps1 -Mode cases -Device DEVICE_UUID_FROM_INVENTORY -Output C:/probe-results/cases-01
powershell -NoProfile -ExecutionPolicy Bypass -File ./Run-GraphicsProbe.ps1 -Mode cases -Device DEVICE_UUID_FROM_INVENTORY -LifecycleCycles 100 -DeviceCycles 20 -PresentSeconds 600 -TimeoutSeconds 1200 -Output C:/probe-results/soak-01
```

Windows PowerShell 5.1 and built-in .NET provide the process watchdog; no Python,
Visual Studio, Vulkan SDK or source checkout is needed to launch. The wrapper
checks every manifest payload before and after execution, validates native JSON,
fixed readback tolerances/artifacts and requested lifecycle counts, isolates
validation settings, and bounds process lifetime/output. A job assigned at process
creation contains descendants even if the launcher exits abruptly. Elevated
validation runs return NOT_RUN with instructions to use a standard terminal.
No global environment, registry or driver configuration is changed.

Retain `summary.json`, native stdout/stderr, readbacks and the package manifest.
Package construction PASS means the package was inspected and produced;
`qualification` remains NOT_RUN. A passing local run qualifies only the recorded
workload/device, not the named matrix or a clean target. Development-host
relocation with a restricted PATH is useful evidence but cannot prove absence
of installed tools; a genuinely clean second environment is still required.
Failures now preserve completed lifecycle counters, phase, cycle indices and
elapsed presentation time where the native process can report them. A hard
driver hang can instead produce only the watchdog timeout/cleanup receipt.

The separate native Windows launcher integration fixture requires the same
standard desktop prerequisites as the elevated helper. It compiles synthetic
CPU children solely for process/JSON/error tests, without substituting for GPU
qualification:

```text
python tools/tests/check_graphics_package_launcher.py --output out/graphics-package-launcher/check-01
```
