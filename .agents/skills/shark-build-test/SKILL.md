---
name: shark-build-test
description: Configure, build and test Shark C++ targets, investigate CTest or sanitizer failures, and verify the runtime dependency boundary.
---

# Shark build and test

Read and apply the [testing policy](../../../docs/testing-policy.md) before
selecting checks. The user-approved 2026-10-27 policy governs future execution
frequency, matrix scope and evidence work, superseding older checkpoint/skill
instructions in those areas. Existing cases, behavioral acceptance, seeds,
timeouts and historical results remain intact. Preserving tests does not require
rerunning every test in every profile on every subchunk.

## Select the work

Read [AGENTS.md](../../../AGENTS.md), the assigned checkpoint and latest handoff.
Use [building](../../../docs/building.md) for setup and [CI](../../../docs/ci.md)
for the existing runners; completed handoffs are evidence, not recurring run lists.

- Documentation-only work needs relevant repository/link/diff checks, not engine
  builds, CTest, sanitizers or packages.
- During implementation, run new behavior tests and directly affected regressions.
- Close a code subchunk with one primary Debug regression of affected behavior,
  affected tests in one applicable sanitizer profile, an affected WSL build/smoke
  for portable code and new/changed public examples. Windows Debug is the ordinary
  primary; WSL is primary for WSL-owned Server work. Windows-only code does not
  acquire a WSL behavior gate.
- At assignment closure, run the relevant broader matrix, connected integration
  cases and affected package checks. Expand earlier only for a concrete affected shared
  service, dependency/build path, compiler setting, package or hardware behavior.

A Tools-only dependency-lock addition calls for dependency and protected-boundary
checks; it does not automatically require full Server behavioral suites or GPU
runs. Compile affected public examples using public headers only. Inspect actual
sanitizer objects for new or changed instrumentation-sensitive targets/sources,
instrumentation/compiler configuration changes or a concrete coverage concern,
rather than auditing unchanged objects each time.
Stop when the selected checks pass unless subsequent changes, failures or an
unresolved concern justify another run. Explain any expansion briefly.

## Run existing tools

Run from the repository root. Windows Ninja presets need the x64 MSVC developer
environment; the wrapper also selects the pinned tools:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset windows-debug --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug --show-only
```

List the actual inventory before selecting test names/labels. For example, when
texture conversion is affected:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R ^shark.tools.gltf-textures$ --output-on-failure --no-tests=error
```

Use matching configure/build/test presets: `windows-debug` or `linux-debug`,
corresponding Release variants, `windows-asan` or `linux-asan-ubsan`. Use
`tools-debug` for independent CPU Tools with Client/Editor OFF, and WSL
`server-debug` / `server-asan-ubsan` for Server tests. Follow the build guide for
retained WSL paths and offline configuration. Reuse canonical builds and caches;
never run concurrent builds in one directory or change host/WSL resource settings.

`python tools/ci.py --preset windows-debug --jobs 2` runs the existing full CPU
workflow when that profile is selected. For changed C/C++ use the pinned
`tools/check_format.py`; for contract/coverage or skill changes use the relevant
`tools/validate_contract.py` or `tools/check_repository.py` checks. Do not run
unrelated checkers merely to lengthen a report.

Investigate the first failure and retain its output. Preserve workloads, seeds,
timeouts and baseline tolerances. Do not retry until green without understanding
the failure; rerun the affected selection after a fix or justified environment
reconciliation. An empty selection or missing prerequisite is not PASS.

## Dependency and compiler boundaries

Keep exact dependency inputs in the lock. Prepare offline archives with the
existing helpers; do not fetch floating replacements to repair a build:

| Owner | Offline CMake inputs and helper |
| --- | --- |
| Core | `SHARK_ENTT_ARCHIVE`, `SHARK_GLM_ARCHIVE`, `SHARK_MBEDTLS_ARCHIVE` through `tools/core_dependencies.py` |
| Server Storage | `SHARK_SQLITE_ARCHIVE` through `tools/storage_dependency.py` |
| Tools source documents | `SHARK_JSON_ARCHIVE` through `tools/source_document_dependency.py` |
| Tools glTF source | `SHARK_CGLTF_ARCHIVE` through `tools/gltf_dependency.py` |
| Tools images | `SHARK_IMAGE_ARCHIVE` through `tools/image_dependency.py` |

Core/Server require matching pinned C and C++ compiler families for private TLS
and SQLite. Only `gltf_source.cpp` uses private MSVC `/EHc-` after `/EHsc` for its
bounded parser allocation boundary; the image decoder retains ordinary exception
settings. Keep sanitizer instrumentation on affected implementation and tests.

For affected target dependencies, configure/build protected `runtime-only` and/or
`server-only` as applicable and check their closure. They disable tests; a
successful boundary build does not substitute for behavior tests. Runtime cannot
acquire Tools/test dependencies; Core/Server exclude presentation and authoring.
Tools-only parsers/decoders stay absent when Tools is OFF. SQLite remains private
to EngineStorage/Server. Package checks must preserve all locked license notices.

## Read domain contracts when affected

- Scene/authoring: [scene](../../../docs/scene-contracts.md),
  [commands](../../../docs/authoring-commands.md),
  [transactions](../../../docs/authoring-transactions.md),
  [history](../../../docs/authoring-history.md) and
  [retries](../../../docs/authoring-retries.md).
- Persistence/network: [project files](../../../docs/project-files.md),
  [durable qualification](../../../docs/durable-qualification.md) and
  [network qualification](../../../docs/network-qualification.md). Retain actual
  native I/O faults, process interruption and authenticated peers where required
  to prove those behaviors; unit substitutes do not establish them.
- Assets/glTF: [cache](../../../docs/asset-import-cache.md),
  [jobs](../../../docs/asset-import-jobs.md),
  [source](../../../docs/gltf-source.md),
  [geometry](../../../docs/gltf-geometry.md) and
  [textures](../../../docs/gltf-textures.md), plus
  [material/asset integration](../../../docs/gltf-assets.md). Keep independent expected values,
  allocation limits/failures and licensed offline fixtures.
- [FeatureWorld](../../../docs/featureworld.md): extend real cases, independent
  expectations and coverage mappings together when a supported surface grows.

## Report and stop

Use existing logs/JUnit and a short summary of change identity, selected commands,
platform/toolchain, outcomes and justified omissions. Distinguish fresh results
from unchanged historical evidence; do not count reused results as fresh runs.
Do not create a new per-checkpoint qualification harness, frozen-source ledger
or evidence-auditing layer without a concrete need under the testing policy.
Existing artifact hashes and validators still apply when their artifacts change.

Repeat the documented injected red/green path only when testing changes to CI
failure propagation. No staging, commits or pushes, including temporary test
repositories; the user owns these actions. Maintain `.gitignore` and provide
suggested commit text. Use only this Windows PC and its retained WSL environment;
Mac, extra hardware and hosted access are not local gates. CPU results do not
qualify GPUs, audio, live MMO capacity or a packaged game.
