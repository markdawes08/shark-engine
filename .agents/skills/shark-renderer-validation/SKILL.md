---
name: shark-renderer-validation
description: Validate Shark graphics changes against Vulkan/NVRHI requirements, readback and visual tolerances, and the separate Windows/Linux hardware gates.
---

# Shark renderer validation

Read [AGENTS.md](../../../AGENTS.md), the assigned increment and
[platform evidence](../../../docs/architecture/0003-platforms.md),
[space/color conventions](../../../docs/architecture/0005-simulation-and-rendering.md)
and [v1 lighting profile](../../../docs/architecture/0006-v1-profile.md).

The opt-in probe has executable 02b draw/compute/readback and lifecycle cases; see
the native presets and bounded JSON commands in the [probe guide](../../../docs/graphics-probe.md).
CPU-safe `shark.graphics-probe.cli` checks run only in graphics configurations.
Prepare the locked standalone validation tools and shader manifest, then use
`--mode cases`, an explicit device UUID, its validation receipt and a fresh output
directory. Core and synchronization validation are required for case PASS;
`--validation off` retains diagnostic execution only. Short lifecycle runs do not
close the 100/20/600-second gate. All named hardware rows remain separately open.
Run the native CPU build/CTest commands in [building](../../../docs/building.md)
for relevant shared code. Contract validation checks documentation only.
Windows 02d also provides the Release package and bounded PowerShell launcher in
the probe guide. Preserve failed/incomplete soaks, partial lifecycle progress,
package identity and relocated raw readbacks. A close request is an incomplete
run, not a passing soak. The user removed Mac compatibility from scope under
[ADR 0009](../../../docs/architecture/0009-windows-linux-platform-scope.md);
02c is retired and no Mac hardware qualification is required.
Never treat an empty selection or unavailable prerequisite as passing coverage.

Assignment 02 must qualify pinned GLFW/NVRHI/Vulkan on the exact real-hardware
six-row Windows/Linux matrix, treating each OS/GPU row independently. Capture
OS/CPU/GPU/driver, lock digest, compiler/shader versions, capabilities/formats,
validation settings and logs, commands, exit status and readback evidence. A bare
triangle or software renderer cannot replace required compute, depth, HDR,
texture-array, lifecycle and portability checks. A failing required path needs a
reproducer and explicit decision; never silently remove a target or swap NVRHI.

Use Vulkan and NVRHI validation, explicit timeouts, controlled input/time and
known readbacks. Apply the ADR's per-case tolerances and approved platform image
baselines. Investigate a difference before proposing a reviewed baseline change;
never automatically replace baselines, widen tolerances or retry away failures.
Test resize, minimize/restore, teardown and resource retirement as well as images.

As lighting lands, exercise PBR, IBL, shadows, AO and tonemapping separately and
combined in FeatureWorld and isolated fixtures. Preserve explicit color/depth/
winding conventions and measure cost on named hardware. Update coverage and the
handoff with PASS/FAIL/NOT RUN; absent hardware remains an open gate.

For the [seamless MMORPG profile](../../../docs/architecture/0007-mmorpg-architecture.md),
keep production presentation in EngineClient; M01's split already exists. The
02 probe has its own private presentation dependencies. Add origin-shift,
streaming, animated-crowd and LOD-cost fixtures in their owners; a static reference
sphere cannot qualify a populated world. Rendering/minimize must not advance or
pause server time. Existing depth/lighting choices require measured amendments if
large-world or crowd evidence shows a problem, not a speculative replacement.
