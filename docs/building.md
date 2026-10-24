# Build and test Shark

M01 extends the C++20 foundation with static EngineCore, EngineClient and
EngineTools libraries, an Engine interface composition, separate
Editor/Runtime/Server entry points and Catch2 tests. Editor/Runtime provide
informational commands and an explicit native window mode; Server has validated
startup and an idle lifecycle.
03a adds shared [Core contracts](core-contracts.md), including owned errors,
structured stderr diagnostics, UUIDs and project paths. `shark.core.contracts`
runs their CPU fixtures with seed 1337. The [03b lifecycle](application-lifecycle.md)
uses `shark.core.lifecycle`, also with seed 1337, for ownership and failure/cleanup
fixtures. [03c timing/work](time-and-work.md) adds `shark.core.timing` and
`shark.core.work`, each with seed 1337 and a 20-second CTest timeout. The latter
uses real bounded producer/consumer threads; it does not start production workers.
04a adds [raw input contracts](input-contracts.md) in EngineClient.
`shark.client.input` uses seed 1337 and a 15-second timeout, registered only in
client-enabled builds. It runs without initializing GLFW, a display or real keyboard input;
Server-only configurations exclude its source and test. The
[04 plan](roadmap/window-input-plan.md) records the completed 04c gate.
04b adds the [owned GLFW window](window-lifecycle.md). `shark.client.window` uses
seed 1337 and a 20-second timeout for display-free ownership/callback fixtures.
On Windows, `SHARK_NATIVE_WINDOW=ON` by default for client builds: this enables
the pinned static GLFW dependency. All Core builds now require a C compiler for
M02a's private TLS graph. 05a also defaults
`SHARK_NATIVE_GRAPHICS=ON` for these Windows clients, privately adding the locked
Vulkan loader/headers and NVRHI. Set both native options OFF for a device-free
client build; graphics requires native windows. Server-only and WSL CPU
configurations leave both OFF and never fetch presentation dependencies.
Ordinary CTest does not open native windows or initialize graphics.
04c adds the [offline frame loop](frame-loop.md). `shark.client.frame` uses seed
1337 and a 20-second timeout for deterministic input/time, pause/step, overflow,
failure and server-independence fixtures. `shark.core.timing` also covers the
compatible Core pause/step extension. These tests use controlled times without
display initialization or wall-clock sleeps. Final local qualification is PASS
in the [04c handoff](handoffs/04c-frame-loop.md).
02a/02b add a separate opt-in [graphics probe](graphics-probe.md) with locked
dependencies, terminal diagnostics, analytic GPU cases and offline validation tools.
02d adds matching `graphics-windows-release` and `graphics-linux-release`
configure/build/test presets for standalone probe packages; see the
[probe guide](graphics-probe.md) for packaging and bounded launch commands.
Scene rendering, gameplay, scene import, authoring UI and export remain unimplemented.
CPU build success does not qualify a GPU, audio device or final release platform.

05a adds the [owned graphics device](graphics-device.md), with CPU lifecycle and
capability tests in `shark.client.graphics` (seed 1337, 20-second timeout),
device-free native CLI checks, and evidence-parser tests. `GraphicsDeviceSmoke`
explicitly opens hidden windows and exercises Vulkan/NVRHI ownership on a named
GPU. Its bounded wrapper requires a current locked validation receipt and retains
JSON evidence. See the [05 checkpoint plan](roadmap/graphics-device-plan.md).
05b adds [clear presentation](presentation-lifecycle.md), CPU lifecycle/launch
checks in `shark.client.presentation`, device-free native fixture CLI checks and
`shark.tooling.presentation-evidence`. `PresentationSmoke` explicitly qualifies
the declared presentation workload on each GPU. Editor/Runtime `--graphics`
opens the clear sample; `--window` retains the existing empty-window sample.
05c adds [failure and stress qualification](presentation-qualification.md).
`PresentationStress` has a compiled Debug/Release/ASan workload and bounded
resource samples. `PresentationFaults` uses isolated children for CPU fatal
destruction and explicitly selected native failure injections. Their ordinary
CTest entries remain device-free. The three `shark.tooling.presentation-stress`,
`shark.tooling.presentation-faults` and `shark.tooling.runtime-package` families
verify evidence and deployment inputs without initializing graphics.
Native tests require separate, explicit commands and retained raw output.

06b adds [owned resources and offscreen rendering](gpu-resources.md).
The CPU-only `shark.client.gpu-resources` and `shark.client.offscreen` tests
exercise shader/upload validation and ownership. Native `OffscreenSmoke` and
`OffscreenFaults` require explicit commands; ordinary CTest never starts a GPU.
Their readback/failure wrappers preserve source, shader, device and validation
identity. The
[06b handoff](handoffs/06b-gpu-resources.md) records final local results.

06c adds `--cube` to Runtime/Editor, using the shared offline frame/presentation
loop and executable-adjacent precompiled shader bundle. Device-free
`shark.client.shader-bundle` tests framing, integrity, typed stage/layout validation,
Unicode paths and the shared diagnostic geometry. Window readback and relocated
cube-package qualification remain separate native checks; the completed
[06c handoff](handoffs/06c-window-distribution.md) records their results. The [resource contract](gpu-resources.md#window-cube-and-capture)
describes immutable draw ownership and actual presentation-attachment capture.

07a adds the device-free [scene ownership and identity contract](scene-contracts.md)
in EngineCore. `shark.core.scene` and `shark.core.world-position` use seed 1337
and 20-second timeouts to exercise checked handles, scene/authority scope, bounded
deferred mutation and canonical global-cell/local-coordinate values. They run
in client-enabled and headless test builds. 07b adds Name, Transform and hierarchy
under the [transform contract](transforms.md). `shark.core.transform` has a
20-second timeout and `shark.core.hierarchy` has a 30-second timeout; both use
seed 1337 and the `cpu;unit;server` labels. These CPU services do not provide
authoring commands, networking or a running world simulation. 07c adds the
[fixed schemas and bounded revisioned inspection](scene-inspection.md) under the
[07 plan](roadmap/scene-plan.md). `shark.core.schema` and `shark.core.scene-query`
both use seed 1337, 20-second timeouts and `cpu;unit;server` labels. They run in
client and headless test builds without a display and test schema validation,
metadata agreement, query limits, revision conflicts and owned snapshot lifetime.
`shark.tooling.scene-dependencies` checks exact EnTT/GLM/TLS archive selection,
offline population, private EnTT/TLS includes and public math configuration without a
display (240-second tooling timeout). M02a requires C/C++ compilers. The fixture
compiles the math wrapper with the
central definitions and defaults, then rejects prior GLM includes and conflicting
configuration. The [07a handoff](handoffs/07a-entity-ownership.md) and
[07b handoff](handoffs/07b-transform-hierarchy.md) record their qualification;
the [07c handoff](handoffs/07c-schema-inspection.md) and
[receipt](coverage/evidence/07c-schema-inspection.json) close full REQ-07 as PASS
under the current-machine scope. All 376 CTests pass with no failures/skips:
Windows Debug/Release/ASan 62 each, WSL client Debug/ASan+UBSan 55 each, and
WSL Server Debug/ASan+UBSan 40 each. Protected builds and isolated package checks
also pass. M02 follows the [network checkpoints](roadmap/network-plan.md);
these results do not add a terminal/editor adapter.

M02b adds `shark.core.net-mtls`, `shark.core.net-session` and
`shark.core.net-sessions-process` to the same prefix. The last test launches the
test-only `SessionProbe` in separate processes with explicit public fixture keys;
it does not add a login flag or test bypass to Runtime/Server. Follow the
[session contract](network-sessions.md). M02c adds `shark.core.net-replication-wire`, `shark.core.net-replication` and
`shark.core.net-replication-process`. The last launches the test-only
`ReplicationProbe` and writes `replication-process.json`; it uses the same public
fixture certificates and no production listener. See the
[replication contract](network-replication.md). M02d adds `shark.core.net-interest`
(30 seconds, seed 1337) and `shark.core.net-interest-process` (120 seconds).
The latter runs `NetInterestProbe` with two actual clients and a bounded opaque
TCP relay, recording `interest-process.json`. It measures active per-connection
traffic and ordered-stream delay without changing installed bootstraps. Follow
the [interest contract](network-interest.md). Its
[M02d handoff](handoffs/M02d-spatial-interest.md) records the earlier 460 passing
CPU CTests and protected package gate. M02e adds `shark.core.net-adversity`
(60 seconds, seed 1337) and `shark.core.net-adversity-process` (360 seconds).
The latter launches test-only `NetAdversityProbe` processes and bounded opaque
relays, recording `adversity-process.json`, with live owned-process memory and
handle/descriptor samples. Follow the [qualification contract](network-qualification.md).
M02e and full REQ-M02 are PASS under the current-machine scope: the
[handoff](handoffs/M02e-integrated-qualification.md) and
[receipt](coverage/evidence/M02e-integrated-qualification.json) record 474 passing
CPU CTests and protected package checks. Assignment 08 follows the
[persistence checkpoints](roadmap/persistence-plan.md); see the
[08a handoff](handoffs/08a-source-documents.md) for source-codec qualification.
Check all fourteen network families
noninteractively after a native build:

```text
ctest --preset windows-debug -R "^shark.core.net-" --output-on-failure
```

Use the corresponding WSL preset for CPU/headless checks. The integration tests
launch separate local TLS processes and write `network-process.json` and
`network-sessions.json` under `build/<preset>/tests/`. Public credentials live only
in test fixtures. The [transport](network-transport.md) and
[session](network-sessions.md) contracts document the C++ interfaces and limits.

06a adds the CPU-only [offline shader builder](shader-pipeline.md).
`shark.tooling.shader-pipeline` tests recipes, dependency tracking, artifact
integrity and failed-build publication with controlled compiler fixtures. The
explicit offline integration command exercises the cached pinned DXC and SPIR-V
validator separately. Neither the normal build nor the Runtime/Server distribution
targets discover or invoke shader tools. The
[06 checkpoint plan](roadmap/shader-resource-plan.md) records the completed
offline, resource and window/distribution checkpoints.

The active client/editor target is this Windows 11 x64 PC. Existing WSL2 Ubuntu
environments cover CPU/headless Server and local package checks.
[ADR 0010](architecture/0010-current-machine-scope.md) limits the plan to these
available resources; native Linux client/GPU shipping and additional machines
are outside scope. [ADR 0009](architecture/0009-windows-linux-platform-scope.md)
continues to exclude Mac compatibility. Existing Linux presets and package code
are retained; their presence does not create a broader platform commitment.

The [MMORPG amendment](roadmap/mmorpg-plan.md) requires the headless Server and
EngineCore/EngineClient separation implemented in M01. `server-only` provides the
isolated Server build. The `runtime-only` preset builds the graphical-client bootstrap
without authoring targets; it is not proof of headless server dependency isolation.

Human contributors and terminal agents can use the same commands below to work
on engine code now. The [human/agent development plan](roadmap/human-agent-plan.md)
assigns game-authoring CLI/discovery to 14/15 and editor integration to 16/17.
There is no game-authoring CLI or usable graphical editor in the current foundation.
`build/windows-debug/bin/Editor.exe --window` opens an empty native window;
Runtime supports the same flag. Add `--window-ms 1000` for a bounded smoke run.
F6 toggles manual offline pause and F7 advances one tick while paused and active.
The sample prints diagnostic JSON lines for those controls; no scene or gameplay
simulation runs. The first resumed frame admits no elapsed time. These reserved
keys are sample controls, not authoring commands or an action-binding API.
The [window guide](window-lifecycle.md) documents lifecycle and explicit native
smoke commands, including their brief visible-window behavior and evidence paths.
The dedicated Server is not an authoring host.

## Cube sample and Runtime package

Build a Windows preset normally, then explicitly prepare the fixed sample's
shaders with cached, locked tools. No ordinary CMake configuration/build, Runtime
startup or package inspection discovers or invokes a source compiler.

```powershell
.tools/venv/Scripts/python.exe tools/shader_pipeline.py prepare --recipe shaders/recipe.json --output out/shaders/development --validation-manifest out/05a-work/validation-tools/summary.json --bin-dir build/windows-debug/bin
.\build\windows-debug\bin\Editor.exe --cube
.\build\windows-debug\bin\Runtime.exe --cube --graphics-ms 1000 --device auto --validation off
```

`prepare` reuses the established source/include generation rules, verifies outputs
and atomically stages `assets/cube.sharkshader` beside both executables. Repeat it
after shader edits. Pass `--shader-bundle PATH` to select another verified bundle;
the default resolves the executable location independently of the launch directory.
Missing or damaged bundles fail before any window/device is created. The
[shader guide](shader-pipeline.md#prepare-and-distribute-the-cube-sample) documents
the binary format and compiler-free `export` command. Required validation remains
an explicit qualification option; interactive launches default to `off`.

The cube is a static diagnostic draw, not a scene or editor UI. It shares F6/F7
offline controls with `--graphics` and `--window`. Its final JSON record reports
the loaded bundle SHA-256 and safely retired presentation counters, with
`qualification: NOT_RUN`; actual native fixtures supply qualification evidence.

For a standalone Release payload, first configure/build `runtime-only` and then
include the verified artifact generation explicitly:

```powershell
& ./tools/windows_dev.ps1 -CommandArguments @('cmake','--preset','runtime-only')
& ./tools/windows_dev.ps1 -CommandArguments @('cmake','--build','--preset','runtime-only')
& ./tools/windows_dev.ps1 -CommandArguments @('python','tools/package_runtime.py','build','--build-dir','build/runtime-only','--crt-dir','C:/Program Files/Microsoft Visual Studio/18/Community/VC/Redist/MSVC/14.51.36231/x64/Microsoft.VC145.CRT','--shaders','out/shaders/development','--output','out/runtime-package/cube')
```

The builder copies only precompiled cube stages in the bundle, runtime DLLs and
the verified license/provenance closure. Shader source paths/hashes and tool
identities are metadata; source HLSL, DXC, spirv-val, Python and authoring libraries
are absent from the payload. Shader-tools preparation is not needed to export
or check a previously verified generation. Omitting `--shaders` retains the
older clear-only deployment fixture.

Copy the package to a fresh directory outside the checkout with spaces and
Unicode in its path. `tools/package_runtime.py check --package <relocation>
--manifest-sha256 <builder-reported-hash> --device <UUID> --device <other-UUID>
--output <fresh-output>` verifies exact inventory and loaded modules and selects
`--cube` when bundle metadata is present. Run the checker through the documented
standard-user wrapper. It uses a system-only PATH and cleared development
overrides; inspection tooling runs outside the payload. This is local dependency
isolation on the current PC, not an additional clean machine or a game exporter.

Use the documented repository root and preset explicitly in automation; on
Windows the wrapper below initializes the compiler without opening an IDE.
`tools/ci.py` returns a nonzero failure and writes `out/ci/<preset>/summary.json`,
phase logs and JUnit when tests execute. Serialize runs sharing that preset/build
and evidence directory, since each run replaces its prior evidence. Archive a
run before repeating it when both results matter. Ordinary toolchain reports
identify HEAD; record worktree modifications separately in the handoff. The
headless wrapper requires a fresh output directory and records additional state.
Neither route needs the ignored local roadmap or an LLM service.

## Prerequisites

Run from the repository root. Use Python 3.13.5 for the pinned development-tool
environment. The checked-in `requirements-dev.txt` pins downloadable CMake
3.31.10, Ninja 1.13.2 and clang-format 20.1.8
distributions with hashes. CMake's project minimum is 3.28; the exact development
versions provide a repeatable baseline. Compiler/SDK and runner versions are
recorded separately in evidence and [CI](ci.md).

Create a local environment if needed:

```text
python -m venv .tools/venv
```

Activate it with `.tools\venv\Scripts\Activate.ps1` in PowerShell or
`source .tools/venv/bin/activate` in a POSIX shell, then install the locked tools:

```text
python -m pip install --require-hashes --only-binary=:all: -r requirements-dev.txt
```

The initial tool and dependency population needs network access. Repeated tests
use local inputs and do not download test assets. A download failure or missing
compiler is a failed/unrun preparation step, not evidence of a successful build.
Do not replace locked versions with arbitrary global installations when reporting
the pinned workflow.

| Host | Native compiler preparation |
| --- | --- |
| Windows x86-64 | Install Visual Studio C++ desktop tools and Windows SDK. Open an x64 Native Tools command prompt or x64 Developer PowerShell, then activate the Python environment. Ninja must find `cl`, the linker and SDK libraries. |
| Existing WSL Ubuntu 24.04 x86-64 userspace | Use GCC for debug/release and Clang for `linux-asan-ubsan`, with that userspace's C++ standard library and development tools available. |

On Windows, the optional repository wrapper locates the installed Visual Studio
C++ tools, initializes the x64 developer environment and selects `.tools/venv`
when present. For example, from an ordinary PowerShell:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python tools/ci.py --preset windows-debug --jobs 2
```

The execution-policy option applies to that process, not a machine-wide setting.
The wrapper does not install Visual Studio or claim unsupported toolchains work.

## Presets and ordinary checks

Each normal build has matching configure, build and test presets and uses Ninja
under `build/<preset>`. Do not reuse one build directory for different compilers
or invoke concurrent builds in it.

| Configuration | Windows | Ubuntu |
| --- | --- | --- |
| Debug | `windows-debug` | `linux-debug` |
| Release | `windows-release` | `linux-release` |
| Sanitizers | `windows-asan` | `linux-asan-ubsan` |

For Windows debug:

```text
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Use the same three commands with `linux-debug` in the existing Linux userspace,
or with the corresponding release/sanitizer preset. The default build includes
EngineCore, EngineClient, EngineTools, Editor, Runtime, Server, Tests and both
dependency-boundary checks. Engine is an interface target. Catch2 is the sole
engine test framework; third-party tests are not enabled.
Inspect currently registered cases before applying a test filter:

```text
ctest --preset windows-debug --show-only
```

Use real names/labels from that output or `tests/CMakeLists.txt`. A test selection
containing no tests cannot establish a feature gate. Tests need bounded timeouts,
deterministic seeds/clocks where relevant and meaningful failure cases. GPU/audio/
scene-runner/export commands will be added with their owning increments.

| Current CTest cases | Labels |
| --- | --- |
| `shark.unit`, `shark.failure-sentinel` | `cpu`, `unit` |
| `shark.core.contracts` | `cpu`, `unit` |
| `shark.core.lifecycle`, `shark.core.timing`, `shark.core.work` | `cpu`, `unit`, `server` |
| `shark.core.scene`, `shark.core.world-position` | `cpu`, `unit`, `server` |
| `shark.core.transform`, `shark.core.hierarchy` | `cpu`, `unit`, `server` |
| `shark.client.input` (client-enabled builds only) | `cpu`, `unit`, `client` |
| `shark.client.window` (client-enabled builds only) | `cpu`, `unit`, `client` |
| `shark.client.shader-bundle` (client-enabled builds only) | `cpu`, `unit`, `client` |
| `shark.runtime.window-cli`, `shark.editor.window-cli` | `cpu`, `integration`, `client` |
| `shark.runtime.cli`, `shark.editor.cli` | `cpu`, `integration` |
| `shark.server.unit`, `shark.server.lifecycle` | `cpu`, `server`, plus `unit` or `integration` |
| `shark.boundary`, `shark.boundary.fixture` | `cpu`, `boundary` |
| `shark.contract`, `shark.repository` | `cpu`, `contract` |
| `shark.tooling.processes`, `shark.tooling.sources` | `cpu`, `tooling` |
| `shark.tooling.server-closure`, `shark.tooling.scene-dependencies` | `cpu`, `tooling`, `boundary`, `server` |
| `shark.boundary.headers` | `cpu`, `tooling`, `boundary`, `server` |
| `shark.tooling.headless-environment` | `cpu`, `tooling`, `server` |
| `shark.tools.source-documents` (Tools enabled) | `cpu`, `unit`, `tooling` |
| `shark.tools.authored-scene` (Tools enabled) | `cpu`, `unit`, `tooling`, `integration` |
| `shark.tools.project-files` (Tools enabled) | `cpu`, `unit`, `tooling`, `integration` |
| `shark.tools.project-files-process` (Tools enabled) | `cpu`, `tooling`, `integration` |
| `shark.tools.persistence-integration` (Tools enabled) | `cpu`, `tooling`, `integration` |
| `shark.tooling.source-document-dependencies` (Tools enabled) | `cpu`, `tooling`, `boundary` |
| `shark.tooling.graphics-package`, `shark.tooling.graphics-package-linux` | `cpu`, `tooling`, `boundary` |

For a focused boundary check, use `ctest --preset windows-debug -L boundary`.
Dependency source inputs live in [dependencies.lock.json](../dependencies.lock.json).
Add immutable pins there before introducing new dependencies; source downloads
must match declared hashes and must not bring in test/editor code at runtime.

07a pins EnTT 4.0.0 at commit `85c6bba014049b5de8fad49d25424df2f1f6a8c1`;
07b pins GLM 1.0.3 at commit `8d1fd52e5ab5590e2c81768ace50c72bae28f2ed`.
`SceneDependencies.cmake` and `MathDependencies.cmake` share the fixed Core archive
helper and create C++20 header interfaces locally. `EnTT::EnTT` remains private to
EngineCore: its SYSTEM headers are never a public API or an include requirement
of Runtime, Server or tests. GLM is the documented public math exception.

Consumers link `EngineCore` and include `<shark/math.hpp>`; the public `SharkMath`
interface transitively supplies the locked GLM include path and
`GLM_FORCE_RADIANS`, `GLM_FORCE_DEPTH_ZERO_TO_ONE`, `GLM_FORCE_EXPLICIT_CTOR`,
`GLM_FORCE_CTOR_INIT` and `GLM_FORCE_PURE`. The wrapper is the single GLM include
point, supplies those defaults for direct header consumers and rejects prior GLM
setup or incompatible definitions. Math uses right-handed, packed scalar double
values; see [the transform contract](transforms.md). Do not serialize GLM object
memory or add per-target math configuration.

Neither EnTT nor GLM upstream CMake project is configured. M02a separately adds
the private static Mbed TLS/TF-PSA graph and requires a C compiler alongside C++.
No upstream tests, programs or transitive downloads are enabled; upstream
documentation utility targets are excluded from the default dependency build.
[ADR 0002](architecture/0002-dependencies.md) records the header pins and selected
MIT licenses, plus the TLS pin and selected Apache-2.0 option.
Ordinary builds can fetch a missing archive; the disconnected
Server workflow below requires the three Core archives plus the separately
verified SQLite archive prepared beforehand.

M02a uses the exact official Mbed TLS 4.1.1 release archive and bundled crypto
sources recorded in the lock. `tools/core_dependencies.py` verifies and prepares
the archive; configure checks all 3,927 populated regular source files. The 147 reviewed
unused upstream test/example symlinks are omitted for portable Windows population;
regular files remain exact archive bytes. Eight redistribution notices under
`licenses/MbedTLS/` must accompany Runtime/Server packages. See
[ADR 0011](architecture/0011-network-foundation.md) and the
[network contract](network-transport.md). The test-only `NetworkProbe` target
does not add network command-line flags to Runtime or Server.

## CPU source-document tooling

08a adds a pure EngineTools project/scene codec. `SHARK_BUILD_TOOLS` can be enabled
without Editor or Client; Editor requires Tools. The `tools-debug` preset has no
client, editor or server executable, display or GPU dependency:

```text
cmake --preset tools-debug
cmake --build --preset tools-debug --parallel 2
ctest --preset tools-debug --show-only
ctest --preset tools-debug -R "^shark.tools.source-documents$" --output-on-failure
```

On Windows use the x64 developer environment or `tools/windows_dev.ps1` wrapper.
Normal editor-enabled presets enable Tools. Runtime-only, Server and graphics
probe presets explicitly disable it. The separate `SourceDocumentTests` target
uses Catch2, seed 1337 and a 60-second CTest timeout. Fixtures are authored source
under `tests/fixtures/source_documents/`; they never enter packages.

Tools privately uses nlohmann/json 3.12.0 from the exact release archive in the
dependency lock. Ordinary Tools builds can fetch that archive. Offline Tools
builds additionally pass `-DSHARK_JSON_ARCHIVE=/absolute/path/json-3.12.0.tar.xz`
or reuse its verified cache. `tools/source_document_dependency.py` verifies every
populated file against that archive; arbitrary source overrides are rejected.
No upstream CMake, examples or tests execute. Core/Server/Runtime do not require
the JSON archive or headers. This pure codec does not perform file I/O, activate
scenes or provide terminal commands; see the [format contract](source-documents.md)
and [08 checkpoint plan](roadmap/persistence-plan.md).

08b adds `AuthoredSceneTests` and `shark.tools.authored-scene` (seed 1337,
60-second timeout) for revision-consistent capture, project/world references,
candidate construction and failure-safe Scene activation. After listing the test
inventory, run both authoring families with
`ctest --preset tools-debug -R "^shark.tools.(source-documents|authored-scene)$" --output-on-failure`.
The [capture/activation contract](authored-scenes.md) includes a complete public
C++ example. These APIs still do not open files or expose terminal commands.

08c adds `ProjectFilesTests`, `ProjectFilesProcess` and the two file-service
families above, each with a 90-second timeout; the Catch2 family uses seed 1337.
List the actual inventory, then run the four authoring families with
`ctest --preset tools-debug -R "^shark.tools.(source-documents|authored-scene|project-files|project-files-process)$" --output-on-failure`.
These tests require a writable native temporary filesystem and exercise real
locks, aliases, sibling publication, external edits and child-process interruption.
They need neither a display nor a GPU. The [file contract](project-files.md)
includes the complete public C++ example and operating-system limits; the C++
services do not expose terminal commands yet.

08d adds `PersistenceIntegrationTests` and `shark.tools.persistence-integration`
(seed 1337, 90-second timeout). It follows independent legacy fixtures through
native reads, Scene activation, edits, explicit migration saves and reopen, and
preserves dirty state through invalid external sources and conflicting edits.
The existing file/process families additionally exercise failed and interrupted
`save_scene` operations through recovery and exact reactivation. Run all five
authoring families after listing the inventory:

```text
ctest --preset tools-debug --show-only
ctest --preset tools-debug -R "^shark.tools.(source-documents|authored-scene|project-files|project-files-process|persistence-integration)$" --output-on-failure
```

CTest owns writable fixture directories under the build tree. Process termination
checks establish local file visibility and recovery, not hardware power-loss
durability. See the [persistence checkpoints](roadmap/persistence-plan.md).

## Opt-in graphics package builds

The graphics fixtures use separate `graphics-windows` / `graphics-linux` debug
presets and `graphics-windows-release` / `graphics-linux-release` package presets.
Ordinary CPU, Server and Runtime configurations keep these dependencies disabled.
Ubuntu 24.04 requires its native C/C++ compiler and X11 development libraries for
GLFW (`libx11-dev`, `libxrandr-dev`, `libxinerama-dev`, `libxcursor-dev`, `libxi-dev`).
The retained backend is X11; native Wayland is not selected. These Linux commands
support local WSL CPU/build checks, not a native Linux graphics shipping gate.

```text
cmake --preset graphics-linux-release
cmake --build --preset graphics-linux-release --parallel 2
ctest --preset graphics-linux-release --output-on-failure
```

The Linux Release preset disables RPATH/RUNPATH. Its CTest preset explicitly
selects the build's `bin` directory through the test process's `LD_LIBRARY_PATH`.
Direct native execution needs the same process-local loader selection; the
standalone package launcher supplies its own `lib` directory. The Linux builder
uses `readelf` to inspect every bundled ELF and rejects unresolved imports,
unexpected interpreters, RPATHs and runtime symbol requirements beyond the
recorded Ubuntu 24.04 baseline. It does not search a developer SDK for missing
libraries. Native GPU execution remains separate from these build/CPU checks.

Prepare the pinned validation tools and four case shaders, then run
`tools/package_graphics_probe_linux.py` with explicit build, loader, shader and
validation-receipt paths; the [complete commands](graphics-probe.md#standalone-linux-probe-package)
also describe relocation and workload options. Windows uses
`tools/package_graphics_probe.py` and its pinned app-local Release CRT inputs.
Both builders require fresh outputs and retain source, dependency, toolchain and
license provenance. These packages are assignment 02 fixtures, not game exports.

The Linux package runs with an ordinary account, Python 3.12+ and its standard
library, and Ubuntu 24.04 x86-64 system runtimes. Graphical modes additionally need
an X11 desktop and the selected GPU's installed Vulkan driver; the local CPU
checks do not. Its launcher needs neither a compiler nor a Vulkan SDK.
Python 3.12+ is a package runtime prerequisite, independent of the pinned
development Python version above. Windows packages instead use Windows
PowerShell 5.1 and built-in .NET. Missing displays/devices are NOT_RUN. The revised
local gate accepts Windows dependency-isolated relocation and SDK-free Linux
description startup. Those checks do not establish native Linux graphics,
absence of installed Windows development tools or another physical machine's
behavior. The full graphics workload remains required on both local Windows GPUs.

## Runtime-only boundary

In the host compiler environment:

```text
cmake --preset runtime-only
cmake --build --preset runtime-only
```

This configures Engine/Core/Client and Runtime without EngineTools, Editor,
Server, Tests or Catch2.
The `shark_check_runtime_boundary` ALL target checks the actual CMake dependency
closure. No runtime-only CTest suite is promised: run the native debug preset to
exercise the starter tests. The boundary must also hold in the ordinary build;
turning authoring targets off is not sufficient evidence by itself.

`runtime-only` uses the host compiler selection. Windows requires the same MSVC
developer environment. On POSIX systems select `CC`/`CXX` before its first
configuration if the system defaults are unsuitable; use a fresh build directory
for a compiler change. Do not edit global machine/compiler configuration.

## Server-only durable storage

M03a adds `EngineStorage` and its private static SQLite 3.54.1 amalgamation only
when `SHARK_BUILD_SERVER=ON`. The public C++ service is documented in
[the store contract](durable-store.md); `Store::create` and `Store::open` require
absolute paths in an existing local directory. Keep generated databases and
journals under `out/` or `build/`. Current Server bootstrap flags do not create a
database, authorize a player or expose SQL. Human and terminal developers use the
same API and noninteractive tests.

M03b adds [owned durable characters](durable-characters.md) in EngineStorage,
with enrollment, ownership and character records. M03c advances the active
schema to 3 for [atomic economic transactions](durable-transactions.md). Legacy
schema-1/2 files reject explicitly; migration is a later checkpoint. `shark.storage.characters`
(60 seconds, seed 1337) and `shark.storage.characters-process` (180 seconds)
exercise service/session admission and actual mTLS server/store restart.
The original storage family also covers durable record validation and revision
conflicts. No new dependency, bootstrap listener or database flag is needed.

M03c adds `shark.storage.transactions` (90 seconds, seed 1337) and
`shark.storage.transactions-process` (180 seconds), with test-only
`StorageTransactionTests` and `StorageTransactionProbe` executables. They exercise
real SQLite conservation, operation retention/expiry and authenticated competing
intents across restart. Full game rules and live simulation remain later owners.

The new test families are `shark.storage.store` (60 seconds, seed 1337),
`shark.storage.process` (90 seconds) and `shark.tooling.storage-dependencies`
(240 seconds). They run without a display or GPU. List the configured inventory
before selecting them:

```text
ctest --preset windows-debug --show-only
ctest --preset windows-debug -R "^shark\.(storage\.|tooling\.storage-dependencies)" --output-on-failure
```

Use the matching Linux preset in WSL. `StorageTests` and `StorageProcess` are
test-only executables, absent from installed packages. The latter emits JSON
receipts through the Python supervisor and uses deterministic pipe barriers and
explicit process exits for the create-transaction boundaries. Runtime-only and
Tools-only configurations do not discover, download or compile SQLite.

## Headless Server

These presets never create EngineClient, Engine, Runtime, EngineTools or Editor:

| Preset | Configuration | Tests / compiler |
| --- | --- | --- |
| `server-only` | Release | No tests or network downloads; verified local EnTT/GLM/TLS/SQLite archives, host C/C++ compilers |
| `server-debug` | Debug | Catch2/CTest enabled, host compiler |
| `server-asan-ubsan` | Debug, ASan + UBSan | Catch2/CTest enabled, Linux Clang |

Prepare the exact EnTT, GLM, Mbed TLS and SQLite archives before the first disconnected configure.
This explicit command downloads only the locked bytes, or verifies an existing
cache; configure itself makes no network request. On Windows from ordinary PowerShell:

```powershell
& ./tools/windows_dev.ps1 -CommandArguments @('python','tools/core_dependencies.py','--output','out/dependency-cache')
& ./tools/windows_dev.ps1 -CommandArguments @('python','tools/storage_dependency.py','--output','out/dependency-cache')
$taskEnTT = (Resolve-Path 'out/dependency-cache/85c6bba014049b5de8fad49d25424df2f1f6a8c1.tar.gz').Path
$taskGLM = (Resolve-Path 'out/dependency-cache/8d1fd52e5ab5590e2c81768ace50c72bae28f2ed.tar.gz').Path
$taskTLS = (Resolve-Path 'out/dependency-cache/mbedtls-4.1.1.tar.bz2').Path
$taskSQLite = (Resolve-Path 'out/dependency-cache/sqlite-amalgamation-3540100.zip').Path
& ./tools/windows_dev.ps1 -CommandArguments @('cmake','--preset','server-only',"-DSHARK_ENTT_ARCHIVE=$taskEnTT","-DSHARK_GLM_ARCHIVE=$taskGLM","-DSHARK_MBEDTLS_ARCHIVE=$taskTLS","-DSHARK_SQLITE_ARCHIVE=$taskSQLite")
& ./tools/windows_dev.ps1 -CommandArguments @('cmake','--build','--preset','server-only')
./build/server-only/bin/Server.exe --version
./build/server-only/bin/Server.exe --run-for-ms 1000
& ./tools/windows_dev.ps1 -CommandArguments @('cmake','--install','build/server-only','--prefix','out/server-package','--component','Server')
```

In the existing WSL/Linux compiler environment:

```sh
python tools/core_dependencies.py --output out/dependency-cache
python tools/storage_dependency.py --output out/dependency-cache
cmake --preset server-only \
  -DSHARK_ENTT_ARCHIVE="$PWD/out/dependency-cache/85c6bba014049b5de8fad49d25424df2f1f6a8c1.tar.gz" \
  -DSHARK_GLM_ARCHIVE="$PWD/out/dependency-cache/8d1fd52e5ab5590e2c81768ace50c72bae28f2ed.tar.gz" \
  -DSHARK_MBEDTLS_ARCHIVE="$PWD/out/dependency-cache/mbedtls-4.1.1.tar.bz2" \
  -DSHARK_SQLITE_ARCHIVE="$PWD/out/dependency-cache/sqlite-amalgamation-3540100.zip"
cmake --build --preset server-only
build/server-only/bin/Server --version
build/server-only/bin/Server --run-for-ms 1000
cmake --install build/server-only --prefix out/server-package --component Server
```

For a read-only source mount, prepare all four archives under a writable work directory
and pass their absolute paths instead. `SHARK_ENTT_ARCHIVE`, `SHARK_GLM_ARCHIVE`,
`SHARK_MBEDTLS_ARCHIVE` and `SHARK_SQLITE_ARCHIVE`
must each match the lock's SHA-256; arbitrary extracted source overrides are
rejected. Previously populated standard FetchContent archive caches satisfy
EnTT/GLM reconfiguration; Mbed TLS uses the verified archive under
`build/<preset>/shark-dependency-archives/` unless an explicit archive was supplied.
`FETCHCONTENT_FULLY_DISCONNECTED` stays
enabled in the preset; the Core helper permits only local verified archive
extraction on that path. It does not enable remote fetching or acquire client
dependencies.

The install component contains `bin/Server` (or `bin/Server.exe`),
`licenses/EnTT/LICENSE`, `licenses/GLM/copying.txt` and the eight locked notices
under `licenses/MbedTLS/`, plus `licenses/SQLite/LICENSE.txt`. It is a dependency-closure
probe, not a shipping distribution; full deployment packaging remains future work.
A clean package directory is required for closure verification. Runtime packages also retain the
EnTT, GLM and Mbed TLS notices alongside their existing graphics/CRT notices. GLM is used
under its MIT option; preserve the complete upstream `copying.txt`.

Server accepts no arguments for an idle lifetime, `--help`, `--version`, or
`--run-for-ms N`. N is 1-6 ASCII decimal digits with a value of 1..600000;
leading zeros are accepted within that token limit. Unknown, repeated, combined,
missing, signed, overflowing and out-of-range options exit 2 before readiness.
Startup/output failure exits 1; successful help/version or shutdown exits 0.
There is no configuration file, listener, simulation tick or world state yet.

On POSIX, SIGINT/SIGTERM request graceful shutdown. The handler only records the
signal; the main loop checks it at most every 20 ms of scheduled idle time and
releases installed handlers through RAII. Tests require ready-process shutdown
within two seconds and impose five-second startup/smoke deadlines. These are
test deadlines, not hard real-time guarantees under host starvation or blocked
output. Windows tests cover finite runs; they do not emulate POSIX shutdown with
forced process termination. Failure to write readiness exercises real partial
startup cleanup on POSIX. Future 03 owns the full service lifecycle and scheduler.

```text
cmake --preset server-debug
cmake --build --preset server-debug
ctest --preset server-debug --output-on-failure
```

The `server` label selects parser/lifecycle/closure-tool tests. `shark.boundary`
checks all available Core/Server/client roots, including in the ordinary build.
Negative boundary fixtures exercise direct, transitive and generator-expression
links/options, generated source/includes and manually added build dependencies.
Compiler dependency records also reject hidden relative and generated-header
inclusions, exercised by actual fixture compilations. The server-only source
closure permits exactly the EnTT, GLM, Mbed TLS and SQLite populations, compares every
populated regular file with its SHA-256-verified archive, and verifies all
installed notices. Only the 147 reviewed unused TLS source symlinks are omitted.
It checks the three private TLS archives and seven object targets against actual
C compiler outputs. Extra dependencies, changed source/headers and missing
source/license files fail this gate. CMake
cannot infer arbitrary behavior hidden in shell recipes; the minimal Ubuntu
job additionally audits configured targets, compiled source, downloaded inputs,
installed packages and the packaged ELF's resolved libraries. See [CI](ci.md)
for the reproducible minimal-environment command and its independent closure check.

The current workload is ADR 0007's **LOCAL-MMO-1: 16 active synthetic players
across two regions**, with a provisional 20 Hz authoritative tick. The idle loop's
polling interval is not a simulation tick, and these tests establish neither tick
performance nor player capacity.

## Warnings, formatting and sanitizer policy

C++20 is required without compiler extensions. Engine-owned code uses target-scoped
warnings as errors. Shark uses ordinary headers, so CMake's C++ module scanning
is explicitly disabled; normal compiler header dependency tracking remains on.
This avoids requiring `clang-scan-deps` in the minimal Clang environment and does
not change sanitizer instrumentation. Warning options are
MSVC `/W4 /WX /permissive- /Zc:__cplusplus /utf-8`, or GCC/Clang
`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`. Do not impose
these warning settings on dependency sources or disable warnings globally to
conceal a failure.

Use matching C/C++ compiler families for every configuration. The Linux sanitizer
presets explicitly select `clang` and `clang++`; versioned environments must use
the matching pair, for example `-DCMAKE_C_COMPILER=clang-18` and
`-DCMAKE_CXX_COMPILER=clang++-18`. The retained headless driver supplies both.
The private TLS C targets receive the selected sanitizer instrumentation without
inheriting engine C++ warning flags.

```text
python tools/check_format.py
python tools/check_format.py --fix
python tools/validate_contract.py
python tools/validate_contract.py --self-test
python tools/check_repository.py --self-test
```

The format checker applies the versioned `.clang-format` policy to engine-owned
C++ sources. `--fix` modifies those files; review the changes. Contract validation
checks architecture/requirement structure and document references. Repository
validation checks skill frontmatter/discovery locations and source eligibility
under `.gitignore`. Neither proves future feature execution; run focused behavior
tests alongside these checks.

Windows uses MSVC AddressSanitizer; it does not claim UBSan. Linux uses Clang ASan
plus UBSan. Unsupported compiler/sanitizer combinations fail configuration.
Do not suppress findings or
turn instrumentation off to mark a sanitizer job passing. Record the exact
toolchain and whether the instrumented program actually ran.

The CMake options are `SHARK_BUILD_CLIENT`, `SHARK_BUILD_SERVER`,
`SHARK_BUILD_EDITOR`, `SHARK_BUILD_TESTS`,
`SHARK_SANITIZER` (`none`, `address`, `address-undefined`) and
`SHARK_INJECT_TEST_FAILURE`. Prefer the checked-in presets. The last option exists
only to prove failure propagation; never leave it enabled in ordinary evidence.

## Recorded CPU workflow and evidence

```text
python tools/ci.py --preset windows-debug --jobs 2
python tools/ci.py --preset windows-debug --inject-test-failure --jobs 2
python tools/toolchain_report.py --output out/toolchain.json --build-dir build/windows-debug
```

Use the native preset. The injected run intentionally returns nonzero when the
test harness works; run the ordinary workflow again to prove the injection is
removed and the real suite passes. CI uses the same entry point. See [CI](ci.md)
for artifact paths, job names and how each result is interpreted. Never stage,
commit or push, even to manufacture CI evidence; the user owns those operations.

Report exact commands, exit results, starting/result revision or absence of a
commit, compiler/SDK, OS/CPU, relevant logs and PASS/FAIL/NOT RUN per gate. A local
Windows pass does not establish Ubuntu. Hosted automation is supplemental to
local progression; its revision-specific results remain separate and existing
GitHub policies are unchanged. Keep unavailable checks and retired hardware
outcomes visible in the handoff without treating out-of-scope machines as active
blockers. Generated output belongs under
ignored build/evidence paths, and `.gitignore` is reviewed each increment.

To verify source eligibility and a fresh build before the user creates a commit:

```text
python tools/check_clean_source.py --preset windows-debug --jobs 2
```

Use the host's debug preset in its compiler environment. This copies Git-eligible
tracked and untracked files into a new ignored `out/clean-source-<timestamp>/`
directory, excludes local inputs such as the roadmap, rejects linked source
paths and runs configure/build/CTest. It creates an empty Git repository there
for ignore checks without staging or committing anything. Its result is a
source-only snapshot check; a committed clean checkout and hosted matrix remain
separate evidence. Logs and `snapshot-result.json` stay inside the snapshot.

## Repository skill discovery

Five focused skills live in `.agents/skills/<name>/SKILL.md` and are linked from
[AGENTS.md](../AGENTS.md). This is a documented Codex repository discovery location,
scanned from the working directory to the repository root. Skill frontmatter
contains a unique name and description; no optional UI metadata is required.
Source: [official skill documentation](https://learn.chatgpt.com/docs/build-skills),
checked 2026-10-17.

The repository validator checks these local skill files and source eligibility;
the contract validator checks document links. A structural check
does not prove that a specific client session loaded the skills. Open the
repository in Codex CLI/IDE and inspect `/skills` for a client-side discovery
check. Do not change user-level Codex settings to make repository validation pass.
The files are unignored repository sources; staging and committing them remains
the user's responsibility.

Assignment 01 also exercised the installed Codex CLI 0.163.0 app-server
`skills/list` with this repository as its working directory and `forceReload=true`.
All five skills were enabled with repository scope and no discovery errors.
The local evidence is `out/ci/skills-discovery.json`; see the
[assignment handoff](handoffs/01-build-foundation.md) for the verified result.
