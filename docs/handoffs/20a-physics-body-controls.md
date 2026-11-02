# 20a: checked dynamic physics controls

Date: 2026-11-01. Base: `c33265a`, initially clean `main`. The user owns staging,
commits and pushing. [ADR 0053](../architecture/0053-physics-body-controls.md),
the [control contract](../physics-controls.md) and [assignment checkpoints](../roadmap/physics-interaction-plan.md)
define this increment. Full REQ-20 remains NOT_RUN until connected acceptance.

The existing PhysicsWorld now exposes owned linear/angular velocity, checked
velocity assignment, center-of-mass linear impulse and bounded per-body force.
All require a live dynamic body, matching Scene lease/generations and creating
thread. Inputs use bounded physics Scene axes, finite normal native values and
explicit magnitude limits; native packed velocity and predicted impulse are
checked against the same squared limits Jolt applies. Native invalid state or
post-mutation failure faults the owner without claiming rollback. These are Core
capabilities for trusted owners, not player protocol or a Lua/terminal binding.

Velocity and impulse act immediately in owner order. Forces accumulate for one
fixed tick, after pre-native event-capacity admission. Rejection leaves earlier
accepted forces intact; successful step, teleport, end and teardown clear them.
Force/gravity integration retains the existing native speed saturation. No
additional scheduler or persistent Scene/source field is introduced.

The existing FeatureWorld rigid-body case retains its original gravity/contact
workload and additionally runs an independent zero-gravity, mass-two control
scenario. Its expected impulse, force and displacement are calculated separately;
the saved scene and case IDs remain unchanged. Coverage adds `physics.body-controls`.

## Selected verification

| Check | Result |
| --- | --- |
| Windows Debug | PASS 5/5 selected CTests in 10.35 s: Core physics and ScriptRuntime, Editor physics, FeatureWorld and real FeatureWorld process; `out/20a-work/windows-final.xml`. |
| WSL Ubuntu Clang ASan+UBSan | PASS 3/3 affected Core physics, ScriptRuntime and FeatureWorld CTests in 2.29 s; `out/20a-work/wsl-final/tests.log` and `tests.xml`. |
| Repository checks | Requirement/link validation, skill/ignore boundaries, pinned C++ formatting and diff whitespace PASS. `.gitignore` already covers generated output and the optional local roadmap. |

The Windows build used `tools/windows_dev.ps1 cmake --build --preset windows-debug`
with `CorePhysicsTests ScriptRuntimeTests FeatureWorldTests SceneRunner
EditorPhysicsTests`, then listed and ran the selected inventory through the
matching CTest preset. WSL build/test commands and inventory are recorded in
`out/20a-work/wsl-final-commands.sh` and `wsl-final/inventory.log`. Tests cover
independent mass/impulse/force motion, collision substeps, pause/single-step,
input and packed-float bounds, Scene/thread/generation rejection, teleport/end
cleanup and pre-native contact backpressure. The initial WSL selection also
passed before the final focused regressions; final results above include them.
No GPU, package or full assignment acceptance is claimed for this CPU subchunk.

Next: 20b's bounded filtered raycasts and overlaps. Preserve owner-thread Scene
and physics leases, historical contact events and checked generations; do not
claim client-authoritative or cross-region gameplay. Lua contact delivery and
debug presentation remain later checkpoints.
