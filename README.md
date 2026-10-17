# Shark

Shark is a C++20 game engine project targeting a **seamless open-world MMORPG**.
The [MMORPG plan and existing-code assessment](docs/roadmap/mmorpg-plan.md) define
the authoritative server, persistent world and staged client/server work.
These are planned capabilities; the current implementation is a CPU bootstrap.
The versioned
[requirements manifest](docs/coverage/requirements.json) preserves assignment
scope and acceptance criteria for clean checkouts.

**Assignment 01: repository, build system and test foundation.** The current
executables provide build/version information and honest bootstrap help. Graphics,
scenes, gameplay and editor UI are future increments. CPU build results do not
establish graphics or release-platform support.

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
  observed macOS sanitizer failure, toolchain repair and pending red/green proof.
- [MMORPG planning handoff](docs/handoffs/mmorpg-planning-amendment.md) records
  the scope change and current repository state. Once 01's hosted gate is verified,
  M01 introduces the headless server boundary before 02's graphics work.

From an x64 Visual Studio developer shell with the documented tools installed:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Use `linux-debug` or `macos-debug` on those hosts. Sanitizer, release and isolated
Runtime configurations are documented in the build guide. All staging, commits
and pushes remain with the user; generated output and local tooling stay ignored.
