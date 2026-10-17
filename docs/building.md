# Build and test Shark

Assignment 01 supplies a C++20 CPU/build foundation: static Engine and
EngineTools libraries, separate Editor/Runtime entry points and Catch2 tests.
The entry points report bootstrap information and usage. Graphics, gameplay,
scene import, authoring UI and export are not implemented by this increment.
CPU build success does not qualify a GPU, audio device or final release platform.

The [MMORPG amendment](roadmap/mmorpg-plan.md) plans a headless Server and
EngineCore/EngineClient separation in M01. No Server target or server-only preset
exists yet. The current `runtime-only` preset builds the graphical-client bootstrap
without authoring targets; it is not proof of headless server dependency isolation.

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
| Ubuntu 24.04 x86-64 | Use GCC for debug/release and Clang for `linux-asan-ubsan`, with the native C++ standard library and development tools available. |
| macOS arm64 / x86-64 | Use native Apple Clang from the selected Xcode/Command Line Tools. Test each CPU family natively; an arm64 build does not qualify Intel. |

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

| Configuration | Windows | Ubuntu | macOS |
| --- | --- | --- | --- |
| Debug | `windows-debug` | `linux-debug` | `macos-debug` |
| Release | `windows-release` | `linux-release` | `macos-release` |
| Sanitizers | `windows-asan` | `linux-asan-ubsan` | `macos-asan-ubsan` |

For Windows debug:

```text
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Use the same three commands with `linux-debug` or `macos-debug` on those hosts,
or with the corresponding release/sanitizer preset. The default build includes
Engine, EngineTools, Editor, Runtime, Tests and the runtime dependency-boundary
check. Catch2 is the sole engine test framework; third-party tests are not enabled.
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
| `shark.runtime.cli`, `shark.editor.cli` | `cpu`, `integration` |
| `shark.boundary`, `shark.boundary.fixture` | `cpu`, `boundary` |
| `shark.contract`, `shark.repository` | `cpu`, `contract` |
| `shark.tooling.processes`, `shark.tooling.sources` | `cpu`, `tooling` |

For a focused boundary check, use `ctest --preset windows-debug -L boundary`.
Dependency source inputs live in [dependencies.lock.json](../dependencies.lock.json).
Add immutable pins there before introducing new dependencies; source downloads
must match declared hashes and must not bring in test/editor code at runtime.

## Runtime-only boundary

In the host compiler environment:

```text
cmake --preset runtime-only
cmake --build --preset runtime-only
```

This configures Engine and Runtime without EngineTools, Editor, Tests or Catch2.
The `shark_check_runtime_boundary` ALL target checks the actual CMake dependency
closure. No runtime-only CTest suite is promised: run the native debug preset to
exercise the starter tests. The boundary must also hold in the ordinary build;
turning authoring targets off is not sufficient evidence by itself.

`runtime-only` uses the host compiler selection. Windows requires the same MSVC
developer environment. On POSIX systems select `CC`/`CXX` before its first
configuration if the system defaults are unsuitable; use a fresh build directory
for a compiler change. Do not edit global machine/compiler configuration.

## Warnings, formatting and sanitizer policy

C++20 is required without compiler extensions. Engine-owned code uses target-scoped
warnings as errors: MSVC `/W4 /WX /permissive- /Zc:__cplusplus /utf-8`, or GCC/Clang
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
plus UBSan; macOS uses the native supported Apple Clang combination. Unsupported
compiler/sanitizer combinations fail configuration. Do not suppress findings or
turn instrumentation off to mark a sanitizer job passing. Record the exact
toolchain and whether the instrumented program actually ran.

The CMake options are `SHARK_BUILD_EDITOR`, `SHARK_BUILD_TESTS`,
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
Windows pass does not establish Ubuntu or either Mac build. Keep unavailable
hosted jobs and hardware visible in the handoff. Generated output belongs under
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
