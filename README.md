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
input and owned Windows windows, with a separate opt-in graphics feasibility
probe containing analytic draw/compute/readback cases.
The versioned
[requirements manifest](docs/coverage/requirements.json) preserves assignment
scope and acceptance criteria for clean checkouts.

The engine is designed for **both human editor users and terminal-based LLM
agents**, with shared authoring services and no required model/provider integration.
The [human/agent plan and foundation assessment](docs/roadmap/human-agent-plan.md)
define noninteractive tools, discoverable schemas and safe project handoff. Those
game-authoring interfaces remain planned; terminal build/test tools work today.

**Assignment 03 and its [three checkpoints](docs/roadmap/core-plan.md) are complete.**
**04a input contracts and 04b native GLFW windows are complete; next is 04c
frame-loop integration and qualification.** See the [input guide](docs/input-contracts.md),
[window lifecycle guide](docs/window-lifecycle.md),
[04b handoff](docs/handoffs/04b-native-window.md) and
[window/input plan](docs/roadmap/window-input-plan.md). Full `REQ-04` remains
NOT_RUN pending fixed-tick input consumption, offline pause/step and final 04c qualification.
See the [03c handoff](docs/handoffs/03c-time-and-work.md),
[Core API guide](docs/core-contracts.md), [lifecycle guide](docs/application-lifecycle.md)
and [timing/work guide](docs/time-and-work.md). Reusable CPU scheduling and bounded
completions pass local qualification; 04c will connect them to window/input consumption.
Assignment 02 is complete under
the current-machine scope. The headless server and Core/Client
separation have passed local and hosted qualification, including deliberate test
failure and restored success. The [graphics probe](docs/graphics-probe.md) adds
locked dependencies, device inventory, validated GPU readbacks and bounded lifecycle; the
[local graphics gate](docs/roadmap/graphics-plan.md) passes on both available GPUs. Editor/Runtime provide
version, bootstrap help and explicit `--window` native window mode;
Server adds validated startup, timed execution and
shutdown. Production rendering, scenes, gameplay and editor UI are future increments. CPU
build results do not establish graphics or release-platform support.

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
  has owned requirements and observable acceptance criteria. Bootstrap and runtime
  boundary features are mapped to tests; component, command and Lua entries remain
  empty until their APIs exist.
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

Open the native window with `build/windows-debug/bin/Editor.exe --window` or
`build/windows-debug/bin/Runtime.exe --window`. These currently show a window
without rendered game content or authoring UI; close it through its title bar.
See the [window guide](docs/window-lifecycle.md) for bounded terminal smoke runs.

Use `linux-debug` in the existing WSL Linux environment. Sanitizer, release and isolated
Runtime configurations are documented in the build guide. All staging, commits
and pushes remain with the user; generated output and local tooling stay ignored.

For a server without client/editor/test targets, use `cmake --preset server-only`,
then `cmake --build --preset server-only`. Run
`build/server-only/bin/Server --run-for-ms 1000` (append `.exe` on Windows)
for a one-second headless smoke run.
With no arguments it waits for shutdown. This is an idle process foundation;
it has no player connections or world simulation yet.
