# Graphics probe setup (02a)

The opt-in `GraphicsProbe` is a developer fixture using GLFW, NVRHI and Vulkan.
It inventories actual devices and can create/destroy a surface, Vulkan device,
NVRHI validation wrapper and swapchain. It does not draw, dispatch compute,
read back images or qualify a release platform. All eight assignment 02 cases
remain `NOT_RUN` in its JSON. The [checkpoint plan](roadmap/graphics-plan.md)
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

`--describe` emits JSON schema 1, its compiled dependency-lock and probe-source digests, available
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
02b must pin and stage validation layers before accepting validation evidence.
NVRHI's validation wrapper remains part of smoke, but cannot replace Vulkan
validation. Do not install or replace a user's GPU driver as an automatic repair.

The runner applies a 30-second default deadline (`--timeout 1..300`), owns process
tree cleanup and refuses an existing output directory. It saves the command,
timestamps, source/worktree fingerprints, executable and lock hashes, compiler
metadata when supplied, stdout, stderr and `summary.json`. It rejects stale lock
or probe-source digests, missing/mismatched stages or device selection,
malformed/contradictory reports and false qualification claims. The source digest
binds the probe's CMake file, main and report writer; retained configure/build
commands and source fingerprints still supply the surrounding build provenance. Run it
from the configured compiler/tool environment for complete tool metadata. A raw
executable invocation has no external driver-call watchdog; use the runner in
automation. Serialize builds and commands sharing a build directory.

| Exit | Meaning |
| --- | --- |
| 0 | Requested description/inventory/setup diagnostic succeeded; full gate remains NOT_RUN |
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

## Offline shader setup and Mac follow-up

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
The independent SPIR-V validator and representative rendering/compute shaders
remain 02b work; successful compilation alone is not shader/device qualification.

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
