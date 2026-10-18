---
name: shark-release-packaging
description: Validate Shark standalone exports and release packages for cooked dependency completeness, runtime separation, licenses and clean-target behavior.
---

# Shark export and release packaging

Read [AGENTS.md](../../../AGENTS.md), the exact assigned export/release increment,
[target boundaries](../../../docs/architecture/0001-targets-and-boundaries.md),
[platform matrix](../../../docs/architecture/0003-platforms.md) and
[cooked-format rules](../../../docs/architecture/0004-formats-and-identity.md).
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
out/server-package --component Server` for an executable-only closure fixture.
The minimal Ubuntu job independently inspects its source, link and ELF closure
and starts the installed executable. It is not a game export or licensed release
package; do not claim M08/35 acceptance from this bootstrap fixture.

Early export 14 may use explicitly versioned engine-owned development payloads;
31 replaces them with validated cooked dependency closure. Include declared roots
for script-selected/dynamically assembled prefab, scene and audio IDs, precompiled
shaders, derived IBL, scripts, fonts/audio and redistribution notices. Missing
dependencies fail atomically with machine-readable errors and progress status.
Keep source importers, authoring services, GUI libraries and offline compilers out
of Runtime; distinguish qualified Vulkan-on-Metal runtime translation libraries.

Launch exports outside the checkout on clean targets without editor, source,
compiler or Vulkan SDK. Verify host/target restrictions, startup scenes and a
prefab plus sound chosen only by script. Test repeatable cooking under pinned
inputs; signing/notarization does not need byte-identical package output. Preserve
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

Apply signing/notarization only through the selected authorized channel, with
secrets outside the repository. Missing credentials or hardware remain explicit
release blockers. Do not stage, commit or push: the user's repository policy
reserves these source-control actions to them. Provide a reviewable change set,
remaining release gates, handoff and suggested commit text.

The [MMORPG release plan](../../../docs/roadmap/mmorpg-plan.md) also requires separate
client/server packages in M08, compatible wire/content/database versions, private
server roots, authenticated content, drain/restart and tested backup/restore.
The Server package must run on its qualified Linux target without presentation
dependencies. M09/M10 supply multi-client seamless-world, failure and active-load
evidence; Tetris/desktop install checks alone cannot qualify an MMORPG release.
