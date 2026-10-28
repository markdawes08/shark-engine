---
name: shark-release-packaging
description: Validate Shark standalone exports and packages for cooked dependency completeness, runtime separation, licenses and isolated deployment on the current machine.
---

# Shark release and package qualification

Apply the [testing policy](../../../docs/testing-policy.md). Use this workflow for
an explicit package/export gate or changes to install rules, binary closure,
licenses, artifact formats or deployment behavior. Ordinary CPU edits and a private
Tools lock entry do not automatically require package redeployment. Read [AGENTS.md](../../../AGENTS.md),
[target boundaries](../../../docs/architecture/0001-targets-and-boundaries.md),
[format rules](../../../docs/architecture/0004-formats-and-identity.md) and the
assigned export increment. Bootstrap launch is not a playable exported game.

## Scope and protected closures

The [current-machine scope](../../../docs/architecture/0010-current-machine-scope.md)
uses native Windows relocation and the retained SDK-free WSL Ubuntu userspace.
No additional physical machine, native Linux GPU, Mac, cloud, signing service or
external distribution channel is required. Describe same-machine isolation honestly;
development tools remain installed on Windows.

Runtime cannot acquire Tools/editor/GUI authoring, JSON/cgltf/source decoders,
source shader compilers, test libraries or private fixture interfaces. Server uses
Core and private Storage without presentation/authoring dependencies. Typed authoring
commands, retries, cache/job services, credentials and process/recovery fixtures
never enter protected packages. Core/Client/Tools/Runtime cannot acquire SQLite.

Use existing `runtime-only` / `server-only` presets and the builders/inspectors in
[building](../../../docs/building.md). Server installation uses the Server component;
tests are disabled in these protected presets, so a build does not prove behavior.
Core offline inputs are `SHARK_ENTT_ARCHIVE`, `SHARK_GLM_ARCHIVE` and
`SHARK_MBEDTLS_ARCHIVE`; Server additionally uses `SHARK_SQLITE_ARCHIVE`. Reuse exact
cached archives. Source closure compares populated inputs with verified archives;
missing, changed or extra dependency files must reject. Read-only source mounts
need absolute inputs and a writable archive/build location.

Retain all locked redistribution notices, including EnTT, GLM's complete MIT-selected
`copying.txt`, complete Mbed TLS/TF-PSA nested notices and SQLite's upstream notice.
Private static dependencies do not authorize shipping shared TLS/SQLite libraries,
upstream shells, test certificates, keys, database files/journals or source fixtures.
Tools-OFF configurations must neither populate nor package JSON/cgltf/image decoders.

## Build, inspect and relocate

Use `tools/package_runtime.py` for the existing Runtime package. Graphics-probe
packages use the exact Release presets and builders documented in the
[probe guide](../../../docs/graphics-probe.md); they are diagnostic packages, not
game exports. Windows rejects Debug CRT, stale artifacts and unresolved DLLs.
Its bounded PowerShell launcher needs no checkout, Python or SDK. Linux's launcher
uses Python 3.12+ standard library on Ubuntu24.04 x86-64; inspect every bundled ELF,
RPATH, import and baseline symbol requirement. Native graphics also needs its
explicit app-local loader, vendor ICD and display; CPU startup claims none of that.

For a selected package gate, inspect actual source/link/binary/license closure,
relocate through Unicode/space paths outside the checkout and launch from a different
working directory. Clear development overrides and use system-only search paths;
retain standard-user execution and bounded process/output cleanup. Use SDK-free WSL
for Server launch. Verify manifests/helper/payload hashes and selected negative
inputs with the existing launcher. Synthetic child tests cannot replace native
GPU execution, and construction alone cannot establish isolated launch.

Use managed temporary deployment directories with exact-path cleanup. Keep passing
manifests/results instead of repeated executable copies; retain unexpected failure
evidence. Follow [storage retention](../../../docs/building.md#storage-retention)
and preserve canonical builds/caches. Do not delete whole output or WSL trees.

## Later export owners

Assignment14 may use versioned engine-owned development payloads;31 owns final
cooked dependency closure. When implementing those owners, include declared roots
for dynamically/script-selected assets, scenes/prefabs, shaders, IBL, scripts,
fonts/audio and licenses. Missing dependencies fail atomically with structured
status. Test startup scenes and assets chosen only by script, host/target restrictions
and repeatability under pinned inputs; signing need not be byte-identical.

The [human/agent plan](../../../docs/roadmap/human-agent-plan.md) requires public,
noninteractive discovery/commands, structured jobs/cancellation/artifact results and
human continuation of the same project. CPU validation/import/cook/package work needs
no editor/window/GPU. Only claim those adapter and GUI behaviors when implemented
and exercised; a terminal success cannot pass unavailable GUI/device work.

The [MMORPG plan](../../../docs/roadmap/mmorpg-plan.md) assigns separate client/server
packages, private server roots, compatible wire/content/database versions and
recovery to M08; M09/M10 qualify the reference world and load. Desktop installation
alone is not an MMO release. Report precise actual scope, outcomes and limitations;
leave staging, commits and pushing to the user.
