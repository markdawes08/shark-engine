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
[04 plan](roadmap/window-input-plan.md) tracks the remaining frame-loop work in 04c.
04b adds the [owned GLFW window](window-lifecycle.md). `shark.client.window` uses
seed 1337 and a 20-second timeout for display-free ownership/callback fixtures.
On Windows, `SHARK_NATIVE_WINDOW=ON` by default for client builds: this enables
the C compiler and the pinned static GLFW dependency, without NVRHI or Vulkan.
Set it OFF for a device-free client build. Server-only and WSL CPU configurations
leave it OFF and never fetch GLFW. Ordinary CTest does not open native windows.
02a/02b add a separate opt-in [graphics probe](graphics-probe.md) with locked
dependencies, terminal diagnostics, analytic GPU cases and offline validation tools.
02d adds matching `graphics-windows-release` and `graphics-linux-release`
configure/build/test presets for standalone probe packages; see the
[probe guide](graphics-probe.md) for packaging and bounded launch commands.
Production rendering, gameplay, scene import, authoring UI and export remain unimplemented.
CPU build success does not qualify a GPU, audio device or final release platform.

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
The [window guide](window-lifecycle.md) documents lifecycle and explicit native
smoke commands, including their brief visible-window behavior and evidence paths.
The dedicated Server is not an authoring host.

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
| `shark.client.input` (client-enabled builds only) | `cpu`, `unit`, `client` |
| `shark.client.window` (client-enabled builds only) | `cpu`, `unit`, `client` |
| `shark.runtime.window-cli`, `shark.editor.window-cli` | `cpu`, `integration`, `client` |
| `shark.runtime.cli`, `shark.editor.cli` | `cpu`, `integration` |
| `shark.server.unit`, `shark.server.lifecycle` | `cpu`, `server`, plus `unit` or `integration` |
| `shark.boundary`, `shark.boundary.fixture` | `cpu`, `boundary` |
| `shark.contract`, `shark.repository` | `cpu`, `contract` |
| `shark.tooling.processes`, `shark.tooling.sources` | `cpu`, `tooling` |
| `shark.tooling.server-closure` | `cpu`, `tooling`, `boundary`, `server` |
| `shark.boundary.headers` | `cpu`, `tooling`, `boundary`, `server` |
| `shark.tooling.headless-environment` | `cpu`, `tooling`, `server` |
| `shark.tooling.graphics-package`, `shark.tooling.graphics-package-linux` | `cpu`, `tooling`, `boundary` |

For a focused boundary check, use `ctest --preset windows-debug -L boundary`.
Dependency source inputs live in [dependencies.lock.json](../dependencies.lock.json).
Add immutable pins there before introducing new dependencies; source downloads
must match declared hashes and must not bring in test/editor code at runtime.

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

## Headless Server

These presets never create EngineClient, Engine, Runtime, EngineTools or Editor:

| Preset | Configuration | Tests / compiler |
| --- | --- | --- |
| `server-only` | Release | No tests, FetchContent fully disconnected, host compiler |
| `server-debug` | Debug | Catch2/CTest enabled, host compiler |
| `server-asan-ubsan` | Debug, ASan + UBSan | Catch2/CTest enabled, Linux Clang |

```text
cmake --preset server-only
cmake --build --preset server-only
build/server-only/bin/Server --version
build/server-only/bin/Server --run-for-ms 1000
cmake --install build/server-only --prefix out/server-package --component Server
```

On Windows use the developer environment and `Server.exe`. The install component
contains only `bin/Server` (or `bin/Server.exe`), and is a dependency-closure probe,
not a shipping distribution. Release licensing and deployment packaging remain
future work. A clean package directory is required for closure verification.

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
inclusions, exercised by actual fixture compilations. CMake
cannot infer arbitrary behavior hidden in shell recipes; the minimal Ubuntu
job additionally audits configured targets, compiled source, downloaded inputs,
installed packages and the packaged ELF's resolved libraries. See [CI](ci.md)
for the reproducible minimal-environment command and its independent closure check.

The provisional world target remains **20 Hz and 256 active players** from
ADR 0007. The idle loop's polling interval is not a simulation tick, and these
tests establish neither tick performance nor player capacity.

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
