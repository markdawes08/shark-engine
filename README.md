# Shark

Shark is a C++20 game engine project targeting a **seamless open-world MMORPG**.
Client and editor targets are **Windows and Linux**, with a Linux headless server.
Mac compatibility is outside the project scope under
[ADR 0009](docs/architecture/0009-windows-linux-platform-scope.md).
The [MMORPG plan and existing-code assessment](docs/roadmap/mmorpg-plan.md) define
the authoritative server, persistent world and staged client/server work.
These are planned capabilities; the current engine is a CPU bootstrap with a
separate opt-in graphics feasibility probe with analytic draw/compute/readback cases.
The versioned
[requirements manifest](docs/coverage/requirements.json) preserves assignment
scope and acceptance criteria for clean checkouts.

The engine is designed for **both human editor users and terminal-based LLM
agents**, with shared authoring services and no required model/provider integration.
The [human/agent plan and foundation assessment](docs/roadmap/human-agent-plan.md)
define noninteractive tools, discoverable schemas and safe project handoff. Those
game-authoring interfaces remain planned; terminal build/test tools work today.

**Current assignment: 02; local Windows 02d work verified, full hardware gate open.** The headless server and Core/Client
separation have passed local and hosted qualification, including deliberate test
failure and restored success. The [graphics probe](docs/graphics-probe.md) adds
locked dependencies, device inventory, validated GPU readbacks and bounded lifecycle; the
[full graphics gate](docs/roadmap/graphics-plan.md) remains open. Editor/Runtime provide
version and bootstrap help; Server adds validated startup, timed execution and
shutdown. Production rendering, scenes, gameplay and editor UI are future increments. CPU
build results do not establish graphics or release-platform support.

The [02b handoff](docs/handoffs/02b-graphics-cases.md) records local cases, validation
and limits. Checkpoint 02c is retired by the
[platform scope amendment](docs/handoffs/windows-linux-scope-amendment.md);
the six named Windows/Linux GPU rows and clean-machine checks remain open.
The [Windows 02d handoff](docs/handoffs/02d-windows-validation.md) records portable
probe packaging, lifecycle evidence and the outstanding hardware gates.

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

Use `linux-debug` on Linux. Sanitizer, release and isolated
Runtime configurations are documented in the build guide. All staging, commits
and pushes remain with the user; generated output and local tooling stay ignored.

For a server without client/editor/test targets, use `cmake --preset server-only`,
then `cmake --build --preset server-only`. Run
`build/server-only/bin/Server --run-for-ms 1000` (append `.exe` on Windows)
for a one-second headless smoke run.
With no arguments it waits for shutdown. This is an idle process foundation;
it has no player connections or world simulation yet.
