---
name: shark-release-packaging
description: Validate Shark standalone exports and packages for cooked dependency completeness, runtime separation, licenses and isolated deployment on the current machine.
---

# Shark export and release packaging

M03e adds explicit offline database maintenance within EngineStorage. Keep
independent legacy/current SQL fixtures, temporary backups, restored candidates
and migration probes outside Runtime/Server packages. Maintenance public examples
compile without SQLite headers. Isolated same-machine restore is distinct from
host/disk-loss protection and must not silently roll back acknowledged outcomes.

M03a adds private static SQLite through EngineStorage to the Server build only.
Offline Server builds also require `SHARK_SQLITE_ARCHIVE`; prepare it separately
with `tools/storage_dependency.py --output`. The installed Server closure includes
`licenses/SQLite/LICENSE.txt` derived from the exact upstream notice. Reject
database files, journals, SQLite shared libraries, the upstream shell and fixtures
in packages. Runtime/Tools/Core must not acquire the SQLite dependency.

08c [project file storage](../../../docs/project-files.md) belongs only to
EngineTools. Its native I/O, source readers, lock/stage files and crash fixtures
must not enter Runtime/Server packages. Recheck protected source/dependency and
installed file closures after changing authoring file services.

08d closes the combined persistence gate using the same local boundaries. Keep
the integrated migration/reopen and interrupted authored-save fixtures outside
Runtime/Server packages; no source-file runtime mode or live player durability
is implied. Verify all three public C++ examples alongside the protected packages.

Read [AGENTS.md](../../../AGENTS.md), the exact assigned export/release increment,
[target boundaries](../../../docs/architecture/0001-targets-and-boundaries.md),
[platform matrix](../../../docs/architecture/0003-platforms.md) and
[cooked-format rules](../../../docs/architecture/0004-formats-and-identity.md).
Apply [ADR 0010](../../../docs/architecture/0010-current-machine-scope.md): Windows
client/editor acceptance uses this PC, while Linux headless/CPU/package acceptance
uses its existing WSL environments. No additional physical machine, native Linux
desktop/GPU or broad shipping compatibility gate is active.
Follow the [human and terminal-agent contract](../../../docs/architecture/0008-human-agent-authoring.md)
and [implementation plan](../../../docs/roadmap/human-agent-plan.md) at the owning
export and acceptance gates; those authoring interfaces remain planned.

Assignment 01 has no game export, cooker, installer or signing workflow. Bootstrap
Runtime is not a playable distributed game. The available boundary check is the
host's `runtime-only` configure/build preset and ALL boundary target in
[building](../../../docs/building.md); `python tools/validate_contract.py` checks
contracts. Do not invent export commands or treat a bootstrap launch as the
acceptance gate for 14, 31 or 35. Add real commands when their owner implements them.

M01 adds the `server-only` preset and `cmake --install build/server-only --prefix
out/server-package --component Server` for a Server closure fixture.
The minimal Ubuntu job independently inspects its source, link and ELF closure
and starts the installed executable. It is not a game export or licensed release
package; do not claim M08/35 acceptance from this bootstrap fixture.
07a adds the private EnTT Core dependency; 07b adds centrally configured GLM as
the approved public math exception through `SharkMath` and `<shark/math.hpp>`.
M02a adds private static Mbed TLS and its bundled TF-PSA Crypto implementation.
Prepare all three exact archives with `python tools/core_dependencies.py --output
out/dependency-cache` before a disconnected Server configure, then pass
`SHARK_ENTT_ARCHIVE`, `SHARK_GLM_ARCHIVE` and `SHARK_MBEDTLS_ARCHIVE` as described in
[building](../../../docs/building.md#headless-server). Read-only source mounts
require a writable archive directory and absolute input paths. Source closure
compares populated dependency files with their verified archives; missing,
changed or additional inputs fail. Server installation and Runtime notices must
retain `licenses/EnTT/LICENSE` and the complete `licenses/GLM/copying.txt` under
GLM's selected MIT option. Both dependencies are header-only and add no shared
library payload. Mbed TLS uses the official complete release archive, including
the pinned TF-PSA/framework inputs, and its dependency verifier records the
reviewed unused test/example symlinks that are not populated. Preserve every
locked notice under `licenses/MbedTLS/`, including nested bundled notices; the
Runtime inspector rejects each missing notice and unexpected TLS payloads.
TLS libraries are static and private to Core, so no third-party TLS DLLs, test
certificates or private keys belong in Runtime/Server packages. The dependency
needs a C compiler in the build environment, not in the installed runtime.
The GLM exception does not authorize other public third-party
types or presentation/authoring dependencies in Core. These additions do not
qualify a shipping MMO package.

08a's [source-document codec](../../../docs/source-documents.md) privately uses
locked nlohmann/json in EngineTools. Protected Runtime/Server presets set
`SHARK_BUILD_TOOLS=OFF`; verify that they neither populate JSON nor package its
headers, authoring library or source fixtures. The JSON notice belongs with a
future authoring distribution, not these protected packages. 08a does not add an
authoring installer, source-file export mode or cooked game payload.

Assignment 02d adds Windows/Linux graphics probe packages, documented in
the [probe guide](../../../docs/graphics-probe.md). Use the explicit
`graphics-windows-release` preset and `tools/package_graphics_probe.py` inputs;
the builder rejects Debug CRT, stale artifacts and unresolved DLLs. The package's
PowerShell launcher verifies files/results and bounds process trees without a
checkout, Python or SDK. Linux uses `graphics-linux-release`,
`tools/package_graphics_probe_linux.py` and the package's
`python3 -I -B run_graphics_probe.py` launcher. Inspect every bundled ELF and
reject RPATHs, unsupported imports and newer-than-baseline symbol requirements.
Require Ubuntu 24.04 x86-64, Python 3.12+ standard library and OS runtime/X11
libraries; graphical modes additionally need an installed vendor ICD and display.
The accepted local describe/unavailable-display checks do not require a GPU.
No compiler/SDK/source checkout is needed to launch.
The Linux launcher verifies helper/payload hashes, uses the shared native
evidence validator and bounds output/process groups, including SIGINT/SIGTERM.
Retain its documented limits for escaping descendants, SIGKILL and machine loss.
Exercise real Unicode/space relocation, required validation, negative inputs and
the full 100/20/600 workload on both active Windows GPUs.
CPU launcher tests use synthetic children and cannot qualify GPUs. Windows
relocation outside the checkout with system-only PATH, cleared development
overrides and inspected DLL closure is accepted for the local package gate.
The SDK-free Linux runtime userspace establishes its CPU/dependency path. Neither
claims independent physical clean-machine execution or broad compatibility.
Native Linux GPU launch is outside scope. Mac 02c is retired under
[ADR 0009](../../../docs/architecture/0009-windows-linux-platform-scope.md);
the former six hardware rows retain historical NOT_RUN outcomes but no longer
block progression. Assignment 02 closes by aggregating local package, failure and
both full GPU workloads under ADR 0010; package construction alone is insufficient.

Early export 14 may use explicitly versioned engine-owned development payloads;
31 replaces them with validated cooked dependency closure. Include declared roots
for script-selected/dynamically assembled prefab, scene and audio IDs, precompiled
shaders, derived IBL, scripts, fonts/audio and redistribution notices. Missing
dependencies fail atomically with machine-readable errors and progress status.
Keep source importers, authoring services, GUI libraries and offline compilers out
of Runtime. Client/editor packages target this Windows PC; headless Server
packages target the existing isolated WSL Ubuntu runtime.

Launch future Windows exports outside the checkout with explicit dependency
closure, isolated PATH/developer overrides and no runtime access to editor/source
payloads. Launch Linux Server packages in the separate SDK-free WSL runtime
userspace. Record that Windows development tools remain installed; do not claim a
physical clean-machine test. Verify host/target restrictions, startup scenes and a
prefab plus sound chosen only by script. Test repeatable cooking under pinned
inputs; signing does not need byte-identical package output. Preserve
logs, build inputs, symbols and precise OS/CPU/GPU/driver evidence per matrix row.

Qualify authoring from a fresh terminal-agent session using only documented
discovery, schemas and public commands, then hand the same project to a human in
the editor and back without conversion or state loss. Exercise shared validation,
revision conflicts, safe retries and undo/recovery semantics. CPU validation,
import/cook and packaging must run without an editor, window or GPU; declare
hardware-dependent operations separately. Retain structured diagnostics, job
status/cancellation results and artifact manifests so terminal automation can
determine success without parsing UI text. Missing GUI/hardware qualification
remains NOT RUN even when the terminal workflow passes.

External distribution/signing infrastructure is outside this local plan. If a
later scope amendment selects signing, use only its authorized channel with
secrets outside the repository. Missing active local prerequisites remain
explicit blockers; unavailable out-of-scope machines do not. Do not stage, commit
or push: the user's repository policy reserves these source-control actions to
them. Provide a reviewable change set,
remaining release gates, handoff and suggested commit text.

The [MMORPG release plan](../../../docs/roadmap/mmorpg-plan.md) also requires separate
client/server packages in M08, compatible wire/content/database versions, private
server roots, authenticated content, drain/restart and tested backup/restore.
The Server package must run in its qualified local WSL Ubuntu environment without
presentation dependencies. M09/M10 supply multi-client seamless-world, failure and active-load
evidence; Tetris/desktop install checks alone cannot qualify an MMORPG release.
