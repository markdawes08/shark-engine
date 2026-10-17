---
name: shark-release-packaging
description: Validate Shark standalone exports and release packages for cooked dependency completeness, runtime separation, licenses and clean-target behavior.
---

# Shark export and release packaging

Read [AGENTS.md](../../../AGENTS.md), the exact assigned export/release increment,
[target boundaries](../../../docs/architecture/0001-targets-and-boundaries.md),
[platform matrix](../../../docs/architecture/0003-platforms.md) and
[cooked-format rules](../../../docs/architecture/0004-formats-and-identity.md).

Assignment 01 has no game export, cooker, installer or signing workflow. Bootstrap
Runtime is not a playable distributed game. The available boundary check is the
host's `runtime-only` configure/build preset and ALL boundary target in
[building](../../../docs/building.md); `python tools/validate_contract.py` checks
contracts. Do not invent export commands or treat a bootstrap launch as the
acceptance gate for 14, 31 or 35. Add real commands when their owner implements them.

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

Apply signing/notarization only through the selected authorized channel, with
secrets outside the repository. Missing credentials or hardware remain explicit
release blockers. Do not stage, commit or push: the user's repository policy
reserves these source-control actions to them. Provide a reviewable change set,
remaining release gates, handoff and suggested commit text.
