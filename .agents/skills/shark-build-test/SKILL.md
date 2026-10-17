---
name: shark-build-test
description: Configure, build and test Shark C++ targets, investigate CTest or sanitizer failures, and verify the runtime dependency boundary.
---

# Shark build and test

Read [AGENTS.md](../../../AGENTS.md), [building](../../../docs/building.md) and
[CI](../../../docs/ci.md). Run from the repository root with the native compiler
environment; Windows Ninja presets need an x64 MSVC developer shell.

Use matching configure/build/test preset names: `windows-debug`, `linux-debug`
or `macos-debug`; corresponding `-release` variants; `windows-asan`,
`linux-asan-ubsan` or `macos-asan-ubsan` for sanitizers.

```text
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
python tools/check_format.py
python tools/validate_contract.py
```

Replace the example preset for the host. `python tools/ci.py --preset
windows-debug --jobs 2` runs the recorded CPU workflow. Inspect the first failed
step and retained evidence before changing code; do not relabel an unavailable
compiler/runner as PASS. List tests with `ctest --preset windows-debug --show-only`
and select real test names/labels. Do not run multiple builds against one build
directory concurrently.

After changes to target dependencies, configure/build `runtime-only` in the host
compiler environment; its ALL boundary target must pass. This preset disables
tests, so a native debug preset must still run CTest. Engine cannot depend
on EngineTools; Runtime cannot acquire authoring or test libraries transitively.
Keep exact dependency inputs in the checked-in lock; do not resolve a newer
floating version as a repair.

For CI failure propagation, use the documented
`python tools/ci.py --preset windows-debug --inject-test-failure` path and expect
a nonzero result. Restore a normal run and record both results. Do not create a
commit or stage files to test CI. This project leaves all staging/commits/pushes
to the user; maintain `.gitignore` and provide suggested commit text.

Report exact command, revision/worktree state, toolchain, platform, outcome and
unverified gates. CPU tests do not qualify GPUs, audio or packaged games. Current
bootstrap executables are not working game/editor applications.
