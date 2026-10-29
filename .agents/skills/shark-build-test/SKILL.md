---
name: shark-build-test
description: Configure, build and test Shark C++ targets, investigate CTest or sanitizer failures, and verify the runtime dependency boundary.
---

# Shark build and test

Apply the [testing policy](../../../docs/testing-policy.md) before selecting checks.
Read [AGENTS.md](../../../AGENTS.md), the assigned checkpoint and relevant handoff.
Use [building](../../../docs/building.md) for setup and [CI](../../../docs/ci.md)
for existing workflows. Historical handoffs are evidence, not recurring run lists.

## Select the affected owner

Test changed behavior and affected consumers. Assignment closure covers the full
affected subsystem, not every test in the configured build. Tools-only work does
not acquire Server/storage/network or unrelated graphics checks merely because
they share a preset. Bare CTest and `tools/ci.py` are for explicit whole-project/CI
qualification. Documentation-only work needs relevant validators and diff review.

Use one relevant sanitizer configuration by default and an affected WSL smoke for
portable code. Expand for actual shared-Core, concurrency/persistence, toolchain,
dependency, packaging or GPU changes under the policy. A Tools-only lock entry calls
for its protected dependency boundary, not automatically every Server test or
package deployment. Compile changed public examples with public headers only;
inspect sanitizer objects only when instrumentation changed or coverage is doubtful.

## Existing commands

Windows presets need x64 MSVC and the locked tools, selected by the wrapper:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset windows-debug --target GltfImportTests --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug --show-only
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R ^shark.tools.gltf-import$ --output-on-failure --no-tests=error
```

Choose the actual target/test for the change; the glTF command is an example.
List the inventory before selecting names/labels. Use matching configure/build/test
presets. `tools-debug` is independent CPU Tools with Client/Editor OFF;
`server-debug` / `server-asan-ubsan` are Server configurations. Reuse canonical
native and retained WSL builds; never build one directory concurrently or alter
host/WSL resource settings. Use absolute JUnit paths when a preset changes directory.

For changed C++ use pinned clang-format; for guidance/coverage changes use
`tools/validate_contract.py` or `tools/check_repository.py` as applicable. Keep
unrelated formatting/checkers out of the change. Preserve failure output and
investigate the first failure. Rerun only affected checks after a fix or justified
environment reconciliation. Missing prerequisites and empty selections are not PASS.

## Dependency and compiler boundaries

| Owner | Offline input/helper |
| --- | --- |
| Core | `SHARK_ENTT_ARCHIVE`, `SHARK_GLM_ARCHIVE`, `SHARK_MBEDTLS_ARCHIVE`, `SHARK_JOLT_ARCHIVE`; `tools/core_dependencies.py` (Jolt population uses `tools/physics_dependency.py`) |
| Server Storage | `SHARK_SQLITE_ARCHIVE`; `tools/storage_dependency.py` |
| Tools source documents | `SHARK_JSON_ARCHIVE`; `tools/source_document_dependency.py` |
| Tools glTF | `SHARK_CGLTF_ARCHIVE`; `tools/gltf_dependency.py` |
| Tools images | `SHARK_IMAGE_ARCHIVE`; `tools/image_dependency.py` |

Use exact locked inputs, not floating replacements. Core/Server require matching
pinned C/C++ compiler families for private TLS/SQLite. Only `gltf_source.cpp` uses
MSVC `/EHc-` after `/EHsc`; its bounded parser failure boundary must remain covered.
The image decoder retains ordinary exception settings.

Affected target changes may require `runtime-only` / `server-only` closure builds.
These disable tests and cannot substitute for behavior tests. Runtime excludes
Tools/test/source-decoder dependencies; Core/Server exclude presentation and
authoring. SQLite is private to server Storage. Preserve all locked package notices.

## Read the relevant contract

- Scene/authoring: [scene](../../../docs/scene-contracts.md),
  [commands](../../../docs/authoring-commands.md), [batches](../../../docs/authoring-transactions.md),
  [history](../../../docs/authoring-history.md), [retries](../../../docs/authoring-retries.md).
- Persistence/network: [files](../../../docs/project-files.md),
  [durable qualification](../../../docs/durable-qualification.md),
  [network qualification](../../../docs/network-qualification.md).
- Assets: [cache](../../../docs/asset-import-cache.md), [jobs](../../../docs/asset-import-jobs.md),
  [glTF qualification](../../../docs/gltf-qualification.md).
- [FeatureWorld](../../../docs/featureworld.md): extend actual cases, independent
  expectations and coverage mappings together when a public surface grows.

Real I/O, process interruption, authenticated peers and GPU readbacks remain
necessary where they establish behavior; unit substitutes do not prove those
boundaries. Preserve existing workloads, seeds, limits and historical evidence.

Report commands, relevant input/revision identity, selected results, logs and
justified omissions. Do not create per-checkpoint freeze/receipt frameworks or
repeat red/green CI sentinel exercises unless failure propagation changed. Stop
after selected checks pass. Staging, commits and pushing remain the user's work.
