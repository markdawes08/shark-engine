# 20b: bounded filtered physics queries

Date: 2026-11-01. Base: `6678bfe`, initially clean `main`. The user owns staging,
commits and pushing. [ADR 0054](../architecture/0054-physics-queries.md), the
[query contract](../physics-queries.md) and [assignment checkpoints](../roadmap/physics-interaction-plan.md)
define this increment. Full REQ-20 remains NOT_RUN until connected acceptance.

The existing owner-thread PhysicsWorld now exposes raycast, sphere-overlap and
axis-aligned box-overlap queries under its active original Scene lease. Queries
use the existing native primitive shapes and current committed poses. Inputs and
the complete query bounds are validated before native float conversion. Bilateral
layer/mask rules, trigger inclusion and a checked ignored body are explicit. A
collapsed packed ray, stale body or Scene, wrong thread and out-of-range input
rejects. Query results own body/entity references, world, Scene revision and tick;
callers must re-resolve before later mutation. A fraction-zero ray hit has no
normal. This is a Core capability for trusted owners, not a player protocol,
Lua binding, global-cell query or render-interpolated picking authority.

Callbacks use bounded fixed storage and no Scene access. A query returns all
admitted hits, sorted by distance/identity for rays or identity for overlaps, or
an explicit capacity error without a partial result. Each hit rechecks native slot,
generation and Scene identity. Queries do not step simulation, consume events or
apply pending force/kinematic targets. Native geometry and float tolerance govern
near-boundary observations; impossible native inconsistencies fault the world.

The existing FeatureWorld rigid-body case retains its saved source and prior
workloads, and adds an isolated trigger-sphere query scenario with independent
ray fraction, point, normal and sphere/box overlap expectations. Coverage adds
`physics.queries`; no source, cooked or wire format changes.

## Selected verification

| Check | Result |
| --- | --- |
| Windows Debug | PASS 5/5 selected CTests in 12.79 s: Core physics and ScriptRuntime, Editor physics, FeatureWorld and real FeatureWorld process; `out/20b-work/windows-final.xml`. |
| WSL Ubuntu Clang ASan+UBSan | PASS 3/3 affected Core physics, ScriptRuntime and FeatureWorld CTests in 3.00 s; `out/20b-work/wsl-final/tests.log` and `tests.xml`. |
| Repository checks | Requirement/link validation, skill/ignore boundaries, pinned C++ formatting and diff whitespace PASS. `.gitignore` already covers generated output and the optional local roadmap. |

The Windows build used `tools/windows_dev.ps1 cmake --build --preset windows-debug`
with `CorePhysicsTests ScriptRuntimeTests FeatureWorldTests SceneRunner
EditorPhysicsTests`, followed by the selected CTest inventory. WSL build/test
commands and inventory are in `out/20b-work/wsl-final-commands.sh` and
`wsl-final/inventory.log`. Tests cover independent ray and overlap geometry,
rotated box and capsule shapes, start-inside hits, bilateral masks, triggers,
ignored/stale bodies, deterministic ties, output overflow, Scene/thread fences,
packed input limits and committed versus pending poses. The initial WSL selection
passed before the final numeric preflight fix; final results above include that
fix and its maximum-double regressions. No GPU, package, real server-region loop
or full assignment acceptance is claimed for this CPU subchunk.

Next: 20c's checked Lua physics bindings and owner-thread contact callbacks with
bounded deferred body changes. Preserve the Scene and physics leases, historical
contact generations and callback ordering; native physics callbacks never run Lua.
Debug visualization and connected acceptance remain 20d/20e.
