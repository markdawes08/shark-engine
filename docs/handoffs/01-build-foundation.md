# Assignment 01 handoff

Date: 2026-10-17. Assignment: **01 - Repository, build system and test foundation**.

Historical implementation handoff. The later [CI verification handoff](01-ci-verification.md)
supersedes the hosted-run status below. The [MMORPG planning amendment](mmorpg-planning-amendment.md)
inserts M01 after this gate and before 02.

Implementation is present. The full assignment gate is **NOT RUN / OPEN** until
the hosted clean-checkout matrix and actual red/green workflow proof run after
the user publishes the repository. Assignment 02 has not started.

## Starting and resulting revision

The starting directory contained assignment 00's documents and validator, with
no Git repository, branch or commit. Git is now initialized on `main`. There is
still no base/result commit and no remote. No files were staged, committed or
pushed, including in temporary verification repositories. The user explicitly
owns those operations and receives suggested commit text after each update.

The original roadmap is preserved unchanged, with SHA-256
`a4a80adf31e90e26da78d966f95e44624dbd84e9877e7554bc95ebf28d63e80c`.
It is intentionally ignored. Source-only checks must not depend on that file.

## Delivered behavior and changed contracts

- Static Engine and EngineTools libraries; separate Editor and Runtime programs;
  Catch2 Tests registered with CTest. Informational bootstrap supports help and
  version output; unknown/conflicting arguments fail with exit 2. No renderer,
  editor UI, scene service or future API is simulated.
- Native Debug/Release and sanitizer presets for Windows/Linux/macOS, plus
  runtime-only with authoring and tests disabled. Target-scoped C++20, warnings
  as errors, formatting and a transitive runtime dependency guard with a failing
  authoring-dependency fixture.
- Catch2 3.16.1 archive hash and build options in the CMake-consumed dependency
  lock; development wheels and GitHub actions pinned by hashes. Catch2 and its
  consumers share C++20 to avoid mismatched standard-library feature detection.
- Shared configure/build/test automation, compiler/tool/image evidence, explicit
  failure injection, CPU CI, format/workflow/repository/contract checks and a
  fresh source-only snapshot check. Hosted image/compiler limitations are
  explicitly amended in ADR 0002 and [CI documentation](../ci.md).
- AGENTS.md and five repository skills at `.agents/skills/<name>/SKILL.md`;
  actual Codex discovery was exercised. Git ignore policies cover the local
  roadmap, generated output, tools, caches, IDE state and secrets without hiding
  source assets. Future agents must maintain `.gitignore` and leave Git writes
  to the user.
- Requirements manifest schema 1 migrated to 2, adding current-assignment and
  actual bootstrap/build-boundary test mappings. The checker rejects unsupported
  manifest versions. Component, authoring-command and Lua API lists remain empty;
  FeatureWorld belongs to 10. No runtime/source scene format changed.

Primary files: root CMake/configuration/locks; `engine/`, `engine_tools/`, `editor/`,
`runtime/`, `tests/`, `cmake/`, `tools/`, `.github/workflows/cpu.yml`, AGENTS.md,
`.agents/skills/`, build/CI docs, coverage and architecture amendments.

## Verified environment and commands

Windows 11 Home x64 (`10.0.26200`); Visual Studio 2026; MSVC
`19.51.36248.0`, toolset `14.51.36231`; Python 3.13.5; CMake 3.31.10;
Ninja wheel 1.13.2; clang-format 20.1.8; Catch2 3.16.1. The selected local
Windows SDK is recorded in `out/ci/*/toolchain.json`; local versions are not
evidence that GitHub's configured SDK/runner was executed.

Run Windows commands below through the native environment wrapper:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python tools/ci.py --preset windows-debug --jobs 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python tools/ci.py --preset windows-release --jobs 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python tools/ci.py --preset windows-asan --jobs 2
```

| Check / exact command after entering the native environment | Result |
| --- | --- |
| `python tools/ci.py --preset windows-debug --jobs 2` | PASS: configure, build and all 10 CTests |
| `python tools/ci.py --preset windows-release --jobs 2` | PASS: configure, build and all 10 CTests |
| `python tools/ci.py --preset windows-asan --jobs 2` | PASS: configure, build and all 10 CTests with MSVC ASan |
| `python tools/ci.py --preset windows-debug --inject-test-failure --jobs 2` | EXPECTED FAIL: only `shark.failure-sentinel` failed; CTest and ci.py recorded exit 8 |
| `python tools/ci.py --preset windows-debug --jobs 2` after injection | PASS: option explicitly reset OFF, rebuilt, all 10 CTests pass |
| `cmake --preset runtime-only` then `cmake --build --preset runtime-only --parallel 2` | PASS: Engine/Runtime only; no dependency download, Python, Editor, EngineTools or Tests required |
| `build/runtime-only/bin/Runtime.exe --version` | PASS: `Shark Runtime 0.1.0` |
| `python tools/check_format.py` | PASS: all 7 owned C++ source/header files use pinned formatting |
| `python tools/validate_contract.py --self-test` | PASS: 39 requirements, actual test/fixture files, links and 14 negative checks |
| `python tools/check_repository.py --self-test` | PASS: five skills, guidance links, ignore boundaries and three invalid-metadata cases |
| `python tools/check_ci_config.py` | PASS: workflow parses, 12 native CPU matrix rows, action pins and failure input wiring |
| `python <skill-creator>/scripts/quick_validate.py .agents/skills/<name>` for each of the five skills | PASS for all five using `.tools/venv/Scripts/python.exe` |
| Codex CLI 0.163.0 app-server `initialize`, then `skills/list` with repository cwd and `forceReload=true` | PASS: all five `shark-*` skills enabled, scope `repo`, no repository discovery errors |
| `python tools/check_clean_source.py --preset windows-debug --jobs 2` | PASS: 60 eligible source files copied without the ignored roadmap; fresh configure/build and all 10 CTests passed |
| Supplemental WSL Ubuntu 26.04 GCC Debug | PASS: configure/build and all 10 CTests; does not replace Ubuntu 24.04 CI |
| Supplemental WSL Ubuntu 26.04 Clang ASan/UBSan | PASS: configure/build and all 10 CTests with ASan/UBSan enabled; does not replace Ubuntu 24.04 CI |
| `python tools/tests/test_process_runner.py -v` | PASS: all five process/output/timeout/descendant regression cases on Windows and Linux |
| `python tools/tests/test_clean_source.py -v` | PASS: all four file/traversal/symlink cases on Windows and Linux; actual symlink checks ran |

Generated logs/JUnit/toolchain metadata are ignored under `out/ci/`; the deliberate
failure evidence is preserved separately at `out/ci/windows-debug-injected-final/`.
Codex discovery output is `out/ci/skills-discovery.json`. This handoff records the
durable outcome; ignored local logs are not artifacts in a future Git checkout.
The final source snapshot is `out/clean-source-20261017T222536105255Z/`, containing
`snapshot-result.json` and each phase log. Its Git index remains empty.

Supplemental Linux checks ran in WSL2 Ubuntu 26.04 x86-64, kernel
`6.18.33.2-microsoft-standard-WSL2`, glibc 2.43, GCC 15.2.0, Clang 21.1.8 and
Python 3.14.4. CMake, Ninja, clang-format and PyYAML used the same locked versions;
the lock includes the PyYAML wheel hash for this supplemental Python version.
Clang's `libclang-rt-21-dev` runtime package was installed before sanitizer checks.
The pinned CI Python 3.13.5 requirement remains unchanged. In the supplemental
Linux environment, these were the exact build/test commands (using its venv):

```sh
cmake --preset linux-debug -DPython3_EXECUTABLE="$VIRTUAL_ENV/bin/python"
cmake --build --preset linux-debug --parallel 4
ctest --preset linux-debug --output-on-failure --no-tests=error --output-junit "$PWD/out/ci/linux-supplemental/linux-debug.xml"
cmake --preset linux-asan-ubsan -DPython3_EXECUTABLE="$VIRTUAL_ENV/bin/python"
cmake --build --preset linux-asan-ubsan --parallel 4
ctest --preset linux-asan-ubsan --output-on-failure --no-tests=error --output-junit "$PWD/out/ci/linux-supplemental/linux-asan-ubsan.xml"
```

`out/ci/linux-supplemental/` retains the environment, commands, build/test logs and
JUnit. Independent review found two tooling defects, both fixed: timeout cleanup
must terminate descendants, and source snapshots must reject symlinks before
resolving them. The regression suites exercise both failure paths; the follow-up
review reported no remaining findings.

## Checks not run and exact remaining gate

No hosted GitHub workflow was run, no branch protection was configured, and no
committed clean checkout exists. The user retained commit/push ownership and no
remote exists. Local failure propagation is demonstrated, but a hosted job must
still be observed turning red because of the injected assertion and green after
disabling it. The workflow provides the real `workflow_dispatch` switch for this.

Ubuntu 24.04 and native macOS arm64/x86-64 CI are NOT RUN. Supplemental WSL results
are separately labeled and cannot qualify those rows. Graphics, audio-device,
packaged applications and all assignment-02 GPU probes are NOT RUN. CPU compilation
does not establish runtime release support on any platform.

The exact next task is to **close assignment 01's CI gate after the user's
commit/push**, using the red/green procedure and required checks in
[docs/ci.md](../ci.md). Fix any platform failures and archive the actual run URLs,
revision and evidence. Only then start **02 - Cross-platform graphics feasibility
gate** using the retained GLFW/NVRHI/Vulkan stack and separate Mac CPU/GPU rows.

Suggested commit text:

```text
build: establish Shark build and test foundation

- add C++20 Engine, EngineTools, Editor, Runtime and Catch2 test targets
- pin dependencies and tools; add presets, sanitizers and CPU CI
- enforce runtime boundaries and verify real test failure propagation
- add agent guidance, discoverable skills and requirement coverage
- ignore local roadmap and generated files; document remaining CI gates
```
