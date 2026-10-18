# 02a graphics dependency and probe setup

Date: 2026-10-18 UTC (2026-10-17 America/New_York). Base: clean `main` at
`f9b9ea0` (`docs: close M01 hosted CI and headless server gate`). Result: uncommitted
working-tree changes; staging, commits and pushes remain with the user.
**02a setup: PASS. Full assignment 02: NOT_RUN. Next: 02b, not started.**

## Delivered behavior and boundaries

The [checkpoint plan](../roadmap/graphics-plan.md) keeps one final assignment-02
gate. `graphics-windows`, `graphics-linux` and `graphics-macos` opt into a private
`GraphicsProbe` target. Ordinary CPU, Runtime and Server builds leave that option
OFF. Core/Server do not discover, fetch, compile or link graphics dependencies.
Conflicting headless/probe settings and unverified source overrides fail before
any dependency population, including Catch2.

[dependencies.lock.json](../../dependencies.lock.json) now records verified
archive pins, upstream revisions, licenses, options and ownership for GLFW 3.5.1,
NVRHI `6b96fb03e07539f08327aea76c56d55f1de9d906` and matching Vulkan-Headers/Loader
1.4.352. The loader is built from source; an arbitrary installed SDK is not used.
NVRHI uses only its Vulkan backend with validation; DirectX, NVAPI, RTXMU and
Aftermath integrations and upstream dependency downloads are disabled. Optional
ray-tracing backend source remains upstream code, but no such device feature is
enabled. No third-party source patch or production renderer architecture is added.

The probe has CPU-safe help/version/JSON discovery; real hidden-window surface and
device inventory; explicit device-UUID selection; Vulkan device, NVRHI validation
wrapper and swapchain startup; reverse-order teardown. It reports queried features,
formats, limits, queues, driver identity, portability restrictions and missing
requirements. It uses modern Metal surface creation on Mac, enables portability
enumeration/subset only when advertised, and shares Vulkan-Hpp definitions with
the NVRHI backend. The beta-extension definition exposes portability queries and
keeps the dispatcher ABI consistent; it does not select experimental driver modes.

The [bounded runner](../../tools/graphics_probe.py) emits machine-readable results
and captures commands, lock/probe-source/executable hashes, source state, environment,
stdout and stderr. It rejects stale probe sources, inconsistent exit/status values,
missing setup stages, wrong device selection and false hardware claims. Each run
uses a fresh evidence directory. The probe submits no GPU work, renders no frame
and performs no readback; all eight P02 cases remain NOT_RUN.

Explicit offline tool preparation locks DXC 1.9.2609 Windows/Linux release archives,
checks all selected compiler/library/license bytes on reuse, compiles a small
engine-owned HLSL setup fixture twice and requires identical SPIR-V. A genuinely
invalid shader must fail normally, not crash. Traversal, links, duplicate/missing
files, changed cache contents and unsafe extraction bounds are rejected. These
tools never enter Runtime/Server/Probe link or install closures.

## Verification

The [durable local receipt](../coverage/evidence/02a-local.json) records commands,
source/lock/artifact identities, actual reports and qualification limits. Raw build,
JUnit, driver and package reports stay under ignored `out/` and `build/`.

| Check | Actual result |
| --- | --- |
| Windows ordinary CPU Debug / Release / ASan | PASS: 18/18 tests each, 54 total; no skips |
| Windows opt-in graphics configure/build | PASS with MSVC 19.51.36248.0, Windows SDK 10.0.28000.0, CMake 3.31.10 and Ninja 1.13.2 |
| Windows graphics preset CPU suite | PASS: 18/18, including probe CLI and serializer fixtures; no GPU qualification |
| Probe CLI/report fixtures | PASS: 17 CLI/report scenarios with nonexistent ICD/no display and two Catch2 serializer cases |
| Evidence/input/tool negative fixtures | PASS: eight runner tests, five forbidden configure scenarios and fourteen offline shader-tool tests |
| Runtime-only Release | PASS: configure/build, client dependency boundary and bootstrap version |
| Fresh pinned Ubuntu 24.04 userspace under WSL | PASS: Release Server-only configure/build/closure/install/smoke; GCC 13 Debug 16/16 and Clang 18 ASan/UBSan 16/16 tests |
| Separate pristine Ubuntu runtime userspace | PASS: installed Server runs for 20 ms with network isolated, no build tools/display dependencies; chroot root is writable |
| Native Windows DXC / supplemental WSL Linux DXC | PASS: repeated 640-byte SPIR-V 1.6 output identical across both hosts; invalid shader rejected, native dependency inspection retained |
| Supplemental Linux probe syntax compilation | PASS: GCC 15.2.0, C++20 and all warnings as errors against locked headers; no linking/device claim |
| Repository checks | PASS: contract/self-tests, skill/ignore self-tests, CI wiring, formatting and whitespace checks |

The Windows host is Windows 11 build 26200 x64 with Python 3.13.5. Device inventory
uses the pinned loader, API 1.4.352. The local GPUs are supplemental hardware:

| Device | Observed driver / Vulkan API | Diagnostic result |
| --- | --- | --- |
| NVIDIA GeForce RTX 4070 Laptop GPU | NVIDIA 610.62 / 1.4.341 | PASS: explicit UUID startup/teardown with Vulkan validation OFF, NVRHI wrapper ON |
| Intel RaptorLake-S Mobile Graphics Controller (Windows lists Intel UHD Graphics) | Intel 101.6790 / 1.4.311 | PASS: explicit UUID startup/teardown with Vulkan validation OFF, NVRHI wrapper ON |
| Required Vulkan validation | Khronos validation layer unavailable | NOT_RUN, exit 3; no automatic fallback |
| Explicit absent device UUID | All-zero UUID absent from inventory | NOT_RUN, exit 3; no selection of another GPU |

Both GPUs report the queried profile capabilities; executing those capabilities
remains future work. Neither is a substitute for the named GTX 1660/Iris Xe/RX
5500 XT matrix. No screenshot/triangle or successful swapchain is treated as a
complete P02-01 PASS.

The headless check uses the existing pinned Ubuntu image and APT snapshot in a
fresh private chroot at `/var/tmp/shark-headless-02a-20261018`; it modifies no host
APT packages. GCC 13.3.0/Clang 18.1.3, glibc 2.39 and the headless Python 3.12.3
exception are recorded. WSL shares the host kernel; this is not a new Docker or
bare-metal result. The separate pristine smoke has no compiler/Python and does not
run the full lifecycle suite there. Earlier M01 hosted Docker evidence remains
historical and unchanged.

Supplemental Linux DXC/syntax checks use WSL Ubuntu 26.04, Python 3.14.4 and glibc
2.43; they do not qualify Ubuntu 24.04 graphical deployment. The minimal Ubuntu
server checks above use the separately pinned 24.04 userspace.

## Reproduction commands

From the repository root, prefix Windows commands with:
`powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1`.

```text
python tools/ci.py --preset windows-debug --jobs 2
python tools/ci.py --preset windows-release --jobs 2
python tools/ci.py --preset windows-asan --jobs 2
cmake --preset runtime-only
cmake --build --preset runtime-only --parallel 2
build/runtime-only/bin/Runtime.exe --version
cmake --preset graphics-windows
cmake --build --preset graphics-windows --parallel 2
ctest --preset graphics-windows --show-only
ctest --preset graphics-windows --output-on-failure --output-junit out/graphics-02a-tests.xml
python tools/graphics_probe.py --executable build/graphics-windows/bin/GraphicsProbe.exe --build-dir build/graphics-windows --mode inventory --output out/graphics/02a-final-inventory
python tools/graphics_probe.py --executable build/graphics-windows/bin/GraphicsProbe.exe --build-dir build/graphics-windows --mode smoke --device 863d6f59a51894ab8b4829aa9460c78d --validation off --output out/graphics/02a-final-nvidia-startup
python tools/graphics_probe.py --executable build/graphics-windows/bin/GraphicsProbe.exe --build-dir build/graphics-windows --mode smoke --device 868088a7040000000002000000000000 --validation off --output out/graphics/02a-final-intel-startup
python tools/prepare_graphics_tools.py --offline --output out/graphics-tools/windows-final-lock
python tools/validate_contract.py --self-test
python tools/check_repository.py --self-test
python tools/check_ci_config.py
python tools/check_format.py
```

Those evidence directories already exist; use new names to reproduce. The receipt
retains exact default-validation/absent-device negative commands and exit 3 results.
The initial tool preparation omitted `--offline` to retrieve hash-verified official
assets. Linux commands actually used:

```text
wsl -u root -- bash -lc 'cd /mnt/c/Users/mrkda/Projects/shark && bash tools/ci/headless/chroot.sh /var/tmp/shark-headless-02a-20261018 .tools/headless out/ci/02a-headless'
wsl --distribution Ubuntu --cd /mnt/c/Users/mrkda/Projects/shark -- python3 tools/prepare_graphics_tools.py --offline --output out/graphics-tools/linux-wsl-final-lock
```

The syntax-only compiler command is retained verbatim in the receipt. It exposed
a signed-enum conversion warning fixed with an explicit Vulkan flags cast; Windows
was rebuilt and both device diagnostics repeated afterward. Initial integration
also corrected the exact-version CMake package redirect, matched Vulkan-Hpp
definitions and typed null enumeration arguments. No warnings were globally
disabled, hardware baseline reduced or third-party source silently replaced.

## Coverage, remaining work and source control

Current assignment advances from completed M01 to 02. REQ-02 remains NOT_RUN.
`graphics.probe-setup` maps the implemented developer fixture to actual CPU tests;
component, game-authoring-command and Lua arrays stay empty. FeatureWorld begins
in 10. Human/terminal tools are documented without claiming a usable editor or
game-authoring API. Existing architecture and MMO authority boundaries remain.

Changed areas: root CMake/presets and lock; opt-in dependency module; probe sources,
fixtures and tests; runner/shader preparation; stronger Server closure checks;
format/toolchain/contract checks; README/AGENTS/build/CI/coverage/plans and renderer
skill guidance. `.gitignore` was reviewed and annotated: generated graphics tools,
builds and evidence already fall under `.tools/`, `build/` and `out/`. The local
roadmap remains ignored; no shared source, fixture or evidence document is hidden.

Linux/macOS full graphics builds are NOT_RUN (native tool/window-system inputs or
hardware absent); the strict Linux syntax check is supplemental only. Vulkan
validation layers, independent `spirv-val`, actual rendering/compute/readback,
resize/minimize stress, soak, all nine GPU rows and clean-machine probe packaging
remain unqualified. DXC has no official native Mac binary; the complete native
shader/driver input graph must be pinned before use. KosmicKrisp/MoltenVK candidates
remain as specified in ADR 0003. No hosted 02a CI or new red/green run can prove
these uncommitted changes; earlier hosted M01 receipts are not relabeled.

02a has no remaining setup blocker. Proceed next to **02b executable graphics
cases and validation**, retaining the later native-Mac and full-matrix gates.
Assignment 03/production renderer work has not begun. No staging, commits,
pushes, branch-policy changes or external messages were performed.

Suggested commit: `feat(graphics): add locked dependencies and terminal probe setup`
