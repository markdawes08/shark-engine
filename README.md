# Shark

Shark is a C++20 game engine project targeting a **seamless open-world MMORPG**.
Development and acceptance target **this Windows PC**, with its RTX 4070 Laptop
and Intel GPUs for the editor/client and existing **WSL2 Ubuntu** for the headless
server. [ADR 0010](docs/architecture/0010-current-machine-scope.md) limits the plan
to available resources; other hardware, native Linux graphics, cloud deployment
and Mac compatibility are outside current scope.
The [MMORPG plan and existing-code assessment](docs/roadmap/mmorpg-plan.md) define
the authoritative server, persistent world and staged client/server work.
These are planned capabilities; the current engine provides Core services, raw
input, owned Windows windows, offline frame/input scheduling, graphics-device
ownership, clear presentation and fixed textured-cube rendering. CPU scenes provide
checked entity ownership, Name/Transform, hierarchy, fixed component schemas and
bounded revisioned inspection. M02a adds bounded loopback TLS/wire APIs and a
deterministic fault model. M02b adds [mutual TLS and owned sessions](docs/network-sessions.md);
local qualification is complete. M02c adds [authoritative pose replication](docs/network-replication.md)
with bounded baselines, deltas and resynchronization. M02d adds
[spatial interest](docs/network-interest.md), recipient privacy and bounded
publication with measured per-connection traffic. M02e adds retained-storage
observations and [integrated network qualification](docs/network-qualification.md)
through active local processes and deterministic faults. CPU authoring now includes
[versioned project/scene value codecs](docs/source-documents.md), with validation
and legacy migration, plus [authored Scene capture and activation](docs/authored-scenes.md).
08c adds [project file saves and external-edit conflicts](docs/project-files.md);
its current-machine qualification is complete.
A separate opt-in graphics
feasibility probe contains analytic draw/compute/readback cases.
The versioned
[requirements manifest](docs/coverage/requirements.json) preserves assignment
scope and acceptance criteria for clean checkouts.

The engine is designed for **both human editor users and terminal-based LLM
agents**, with shared authoring services and no required model/provider integration.
The [human/agent plan and foundation assessment](docs/roadmap/human-agent-plan.md)
define noninteractive tools, discoverable schemas and safe project handoff. Those
game-authoring interfaces remain planned; terminal build/test tools work today.

**Assignment 03 and its [three checkpoints](docs/roadmap/core-plan.md) are complete.**
**04c and full assignment 04 are complete under the current-machine scope.**
**05a, graphics device foundation, is complete under the current-machine scope.**
**05b, swapchain, submission and presentation lifecycle, is complete under the
current-machine scope.** The [05b handoff](docs/handoffs/05b-presentation-lifecycle.md)
records 236 passing CTests and 2,304 validated presentations across both local
GPUs, with repeated resize, minimize/restore and teardown.
**06a, CPU/offline shader tooling, is complete.** The
[06 checkpoint plan](docs/roadmap/shader-resource-plan.md),
[shader contract](docs/shader-pipeline.md) and
[06a handoff](docs/handoffs/06a-offline-shaders.md) record the offline checkpoint
and its verification limits. The user has authorized continued development under
the [nonblocking backlog decision](docs/backlog.md). **06b is complete:**
[owned GPU resources and offscreen rendering](docs/gpu-resources.md) pass on both
local GPUs in Debug, Release and ASan. The [06b handoff](docs/handoffs/06b-gpu-resources.md)
records 240 passing CPU CTests and native readback/failure/lifecycle evidence.
**06c and full assignment 06 are complete** under the current-machine scope,
including window cube integration and precompiled Runtime distribution. The
[06c handoff](docs/handoffs/06c-window-distribution.md) records 261 CPU tests, both-GPU
validation and isolated package execution. Assignment 07 now follows [three checkpoints](docs/roadmap/scene-plan.md).
**07a is complete**: CPU scene ownership, checked identities, deferred mutation and
global-cell coordinates. **07b is complete under the current-machine scope.**
The [07b handoff](docs/handoffs/07b-transform-hierarchy.md) and
[receipt](docs/coverage/evidence/07b-transform-hierarchy.json) record 362 passing
CPU CTests, protected Runtime/Server builds and isolated package checks.
[Scene contracts](docs/scene-contracts.md) and the
[Name/Transform guide](docs/transforms.md) describe mandatory components, validated
hierarchy, local/world reparenting and subtree deletion. 07c implements
[schemas and bounded revisioned inspection](docs/scene-inspection.md) as CPU C++
APIs; file formats, terminal commands and editor controls keep their later owners.
**07c and full REQ-07 are complete under the current-machine scope.** The
[07c handoff](docs/handoffs/07c-schema-inspection.md) and
[receipt](docs/coverage/evidence/07c-schema-inspection.json) record 376 passing CPU
CTests with no failures/skips, protected builds and isolated package checks.
**M02a is complete under the current-machine scope**: 404 CPU CTests pass, including
sanitizers and real TLS client/server processes, with protected builds and isolated
package checks. The [handoff](docs/handoffs/M02a-transport-wire.md) records the
results. **M02b, authentication and owned sessions, is complete**: 425 CPU CTests
and protected package checks pass. Its [handoff](docs/handoffs/M02b-owned-sessions.md)
records the evidence. **M02c, authoritative replication and resynchronization, is complete**:
446 CPU CTests and protected package checks pass. The
[handoff](docs/handoffs/M02c-authoritative-replication.md) records the evidence.
**M02d spatial interest, privacy and connection budgets is complete**: its
[handoff](docs/handoffs/M02d-spatial-interest.md) records 460 passing CPU CTests
and protected package checks. **M02e and full REQ-M02 are PASS under the
current-machine scope.** The [handoff](docs/handoffs/M02e-integrated-qualification.md)
and [receipt](docs/coverage/evidence/M02e-integrated-qualification.json) record
474 passing CPU CTests and protected package qualification across the
[five network checkpoints](docs/roadmap/network-plan.md).
**M03a, Store foundation, is complete:** the [server-only store](docs/durable-store.md)
passes 525 CPU CTests, ten additional standalone Tools checks and protected
package/API gates. Its [handoff](docs/handoffs/M03a-durable-store.md) records the
current-machine evidence. **M03b, Owned durable characters, is complete:**
[durable character services](docs/durable-characters.md) preserve ownership,
revocation and revision-checked checkpoints across authenticated restart.
Its [handoff](docs/handoffs/M03b-owned-characters.md) records 539 passing CPU CTests,
ten additional standalone Tools checks and protected package/API gates.
**M03c, Atomic transactions, is complete:** [atomic transfers](docs/durable-transactions.md)
conserve the modeled currency/items and retain bounded durable retry outcomes.
Its [handoff](docs/handoffs/M03c-atomic-transactions.md) records 553 passing CPU
CTests, ten additional standalone Tools checks and protected package/API gates.
**M03d, Runtime failure recovery, is next** in the
[six-checkpoint plan](docs/roadmap/durable-state-plan.md). Full REQ-M03 remains NOT_RUN.
Assignment 08, Project and scene persistence, completed its
[four checkpoints](docs/roadmap/persistence-plan.md). 08a adds CPU project/scene
formats, validation and migration through [owned C++ values](docs/source-documents.md).
**08a is complete:** its [handoff](docs/handoffs/08a-source-documents.md) records
484 passing CPU CTests, standalone Tools tests and protected package checks.
**08b is complete:** [Scene capture and transactional activation](docs/authored-scenes.md)
passes 489 full-suite CPU CTests, standalone Tools tests, protected package
checks and public API audits. Its [handoff](docs/handoffs/08b-scene-activation.md)
and [receipt](docs/coverage/evidence/08b-scene-activation.json) retain the evidence.
**08c is complete:** [safe project files and conflicts](docs/project-files.md)
pass 499 full-suite CPU CTests, eight additional standalone Tools checks and
protected package/API audits. The [handoff](docs/handoffs/08c-project-files.md)
and [receipt](docs/coverage/evidence/08c-project-files.json) retain the evidence.
**08d and full REQ-08 are PASS under the current-machine scope:** 504 CPU
CTests, ten additional standalone Tools checks, all three public examples
and protected packages pass against 299 unchanged inputs. The
[handoff](docs/handoffs/08d-integrated-qualification.md) and
[receipt](docs/coverage/evidence/08d-integrated-qualification.json) record
integrated migration, save/reopen, conflict and interrupted-save evidence.
See the [transport contract](docs/network-transport.md) and the
[cube launch workflow](docs/building.md#cube-sample-and-runtime-package).
The [05a handoff](docs/handoffs/05a-graphics-device.md) records 211 passing CTests
and 112 validated device lifetimes across the two local GPUs.
The [05 checkpoint plan](docs/roadmap/graphics-device-plan.md) separates device
ownership, presentation in 05b, and full failure/stress qualification in 05c.
See the [device API and terminal commands](docs/graphics-device.md) and
[presentation lifecycle and sample](docs/presentation-lifecycle.md).
See the [input guide](docs/input-contracts.md),
[window lifecycle guide](docs/window-lifecycle.md),
[frame-loop guide](docs/frame-loop.md),
[04c handoff](docs/handoffs/04c-frame-loop.md) and
[window/input plan](docs/roadmap/window-input-plan.md). Full `REQ-04` is PASS:
196 Windows/WSL CTests, three native window smoke runs and protected
standalone/package checks passed.
See the [03c handoff](docs/handoffs/03c-time-and-work.md),
[Core API guide](docs/core-contracts.md), [lifecycle guide](docs/application-lifecycle.md)
and [timing/work guide](docs/time-and-work.md). Reusable CPU scheduling and bounded
completions pass local qualification. The client frame loop now reuses Core
timing for bounded 60 Hz ticks, pause/step and input consumption across frames.
Assignment 02 is complete under
the current-machine scope. The headless server and Core/Client
separation have passed local and hosted qualification, including deliberate test
failure and restored success. The [graphics probe](docs/graphics-probe.md) adds
locked dependencies, device inventory, validated GPU readbacks and bounded lifecycle; the
[local graphics gate](docs/roadmap/graphics-plan.md) passes on both available GPUs. Editor/Runtime provide
version, bootstrap help, explicit `--cube` textured rendering, `--graphics` clear presentation and `--window`
diagnostics through a shared offline frame/input pump;
Server adds validated startup, timed execution and
shutdown. Scene rendering, gameplay, filesystem saves and editor UI remain future
increments. CPU build results do not establish graphics or release-platform support.

The [02b handoff](docs/handoffs/02b-graphics-cases.md) records local cases, validation
and limits. Checkpoint 02c is retired by the
[platform scope amendment](docs/handoffs/windows-linux-scope-amendment.md);
the former six hardware rows and independent-machine checks were subsequently
removed from mandatory scope by the user's current-machine decision.
The [final 02d local handoff](docs/handoffs/02d-final-verification.md) records
Windows/Linux probe packaging, Windows lifecycle evidence and isolated Ubuntu
CPU/package checks under its original scope. The
[current-machine handoff](docs/handoffs/current-machine-scope-amendment.md) records
the amended gate and local MMO plan. The planned workload is 16 active synthetic
players across two local region processes, with a separate two-client graphical
fixture; this is a development target, not a production capacity claim.

- [Architecture decisions](docs/architecture/README.md): targets, dependencies,
  platform candidates, formats, simulation conventions and the MMORPG profile.
- [Requirements and coverage](docs/coverage/README.md): every roadmap assignment
  has owned requirements and observable acceptance criteria. Foundation features
  and the Name/Transform components map to executable tests; terminal/editor and
  Lua integration retain their later owners.
- [Build instructions](docs/building.md), [CI](docs/ci.md) and [agent guidance](AGENTS.md)
  describe native presets, tool versions, checks and ownership policies.
- [Assignment 01 handoff](docs/handoffs/01-build-foundation.md) records local
  verification and the hosted CI gate that was open at that handoff.
- [CI verification follow-up](docs/handoffs/01-ci-verification.md) records the
  historical hosted CPU matrix, sanitizer repair and verified
  deliberate-failure/red-to-green proof.
- [MMORPG planning handoff](docs/handoffs/mmorpg-planning-amendment.md) records
  the scope change and repository state at that handoff.
- [M01 handoff](docs/handoffs/M01-headless-server.md) records the headless server
  implementation and local evidence; its [hosted CI follow-up](docs/handoffs/M01-ci-verification.md)
  closes the native CPU, headless Docker and deliberate-failure/restored-success gate.
- [Human/agent planning handoff](docs/handoffs/human-agent-planning-amendment.md)
  records the shared-authoring amendment and review of the existing foundation.

From an x64 Visual Studio developer shell with the documented tools installed:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Prepare the fixed cube sample explicitly after the C++ build:

```powershell
.tools/venv/Scripts/python.exe tools/shader_pipeline.py prepare --recipe shaders/recipe.json --output out/shaders/development --validation-manifest out/05a-work/validation-tools/summary.json --bin-dir build/windows-debug/bin
.\build\windows-debug\bin\Editor.exe --cube
```

The receipt path identifies the retained locked tools on this machine; the
[shader guide](docs/shader-pipeline.md) describes preparation and compiler-free
export. `--cube` loads precompiled `assets/cube.sharkshader` beside the executable;
startup never compiles shaders. For a bounded run use
`build/windows-debug/bin/Runtime.exe --cube --graphics-ms 1000`.
The existing `--graphics` flag opens the clear-presentation sample.
F6 toggles offline pause and F7 advances one empty tick while paused; presentation
continues while visible. Diagnostic JSON is available in the terminal. No game
world or authoring UI runs. See the [presentation guide](docs/presentation-lifecycle.md)
for explicit device selection, validation and qualifying native fixtures.
The existing `--window` option retains the empty-window diagnostic described in
the [window guide](docs/window-lifecycle.md).

Use `linux-debug` in the existing WSL Linux environment. Sanitizer, release and isolated
Runtime configurations are documented in the build guide. All staging, commits
and pushes remain with the user; generated output and local tooling stay ignored.

After provisioning the locked Core dependency archives described in
[building](docs/building.md), use `cmake --preset server-only`
for a server without client/editor/test targets,
then `cmake --build --preset server-only`. Run
`build/server-only/bin/Server --run-for-ms 1000` (append `.exe` on Windows)
for a one-second headless smoke run.
With no arguments it waits for shutdown. This is an idle process foundation;
it has no player connections or world simulation yet.
