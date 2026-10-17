# ADR 0001: Targets and protected boundaries

Status: accepted contract; implementation starts in 01. Date: 2026-10-17.

## Decision

Use C++20 and target-based CMake, with CMake 3.28 as the initial minimum to
confirm against the locked dependencies in 01. No compiler extensions in engine
interfaces. Exact compiler, SDK, dependency and CI-image versions belong in the
build evidence and dependency lock introduced in 01.

| Target | Kind | Responsibility and allowed direct engine dependencies |
| --- | --- | --- |
| Engine | Static library | Runtime scene, lifecycle, graphics, input, physics, Lua, audio, cooked loading; no authoring libraries |
| EngineTools | Static library | Engine; project/source persistence, commands/undo, imports, cook/export, discovery and authoring services |
| Editor | Executable | EngineTools and Engine; Dear ImGui/ImGuizmo views of the shared services |
| Runtime | Executable | Engine only; startup configuration and game loop |
| SharkCLI | Executable, introduced in 14/15 | EngineTools; export first, then CLI and JSON-RPC stdio authoring host |
| Tests | Test target(s) under one CTest suite | Split engine-only tests from authoring integration tests; one primary C++ framework |

There is no Engine-to-EngineTools dependency, even through a shared utility,
generated header, static initializer, transitive target, or runtime plugin.
Runtime must neither link nor load Editor, EngineTools, ImGui, ImGuizmo, cgltf,
source image importers, or offline shader compilers. Inspect both the CMake
dependency graph and final runtime package in 01, 14, 16 and 31. Runtime loads
engine-owned development payloads for the early export in 14 and only cooked
payloads when 31 replaces that path. It never imports glTF at launch.

Planned file locations are `engine/`, `engine_tools/`, `editor/`, `runtime/`,
`cli/`, `tests/`, `assets/FeatureWorld/`, `cmake/`, `tools/`, `docs/architecture/`,
`docs/coverage/`, and `docs/handoffs/`. Assignment 01 creates the initial library,
executable and test directories; CLI and FeatureWorld remain future work.
Assignment 01 establishes actual commands and provides local
skills in `.agents/skills/<skill-name>/SKILL.md`, with links from AGENTS.md. The
user controls staging and committing those files.

## Shared behavior

The engine owns component validation and the safe scene mutation boundary.
EngineTools wraps that behavior in revision-checked authoring transactions,
history and project services. UI, CLI, protocol handlers and integration tests
call those same services; widgets never edit authoritative components directly.
Gameplay uses validated engine operations without authoring undo history.
Read queries return snapshots or checked handles, not mutable storage pointers.

EnTT stays behind an engine-owned interface. Start with an ordered forward-render
pass list, explicit resources and dependency injection through constructors.
Do not introduce a general render graph, service locator, job framework or plugin
system as a foundation requirement.

## Lifetime, threading and failure

Each application root exclusively owns its services. A scene owns its entity
registry and per-scene script/physics state; resource managers own shared immutable
assets through typed handles. Use RAII and unique ownership by default. Reference
counting is reserved for shared asset lifetimes; it does not make mutable scene
access thread-safe. Startup records successful stages and unwinds in reverse
order on failure. Shutdown stops producers, joins workers, drains/invalidates
queues, waits for submitted GPU use, and releases consumers before providers.

The process main thread controls scene mutation, VM calls, GLFW event processing,
and command commits. CPU import and physics workers return data/events; they
cannot call Lua or touch authoritative scene storage. The audio callback consumes
preallocated bounded messages, never file I/O, blocking locks, general allocation
or scene mutation. GPU resources retire after their last submission completes.
The scheduling details are in [0005](0005-simulation-and-rendering.md). This also
satisfies GLFW's main-thread initialization/window/event requirements, which
matter particularly for Cocoa. [GLFW threading contract](https://www.glfw.org/docs/latest/intro_guide.html#thread_safety)

Expected failures return an engine-owned `Result<T>`/error record: stable code,
human-readable message, operation, and relevant entity/asset/path/request context.
C++20 does not supply `std::expected`; the small owned result type is implemented
in 03. Assertions diagnose internal invariants, never replace validation of files,
commands or scripts. Third-party exceptions are caught at subsystem entry points;
exceptions never cross Lua C callbacks, audio callbacks or worker boundaries.
Cleanup must not throw. Release failures remain actionable in structured logs.
Fatal device loss reports diagnostics and terminates the affected rendering
session cleanly; recovery is limited to explicitly tested surface/swapchain cases
in 05, never an unbounded retry loop.

## Acceptance ownership

01 proves clean configuration, real tests and the runtime link boundary; 03 proves
partial initialization and teardown; 07/09 prove mutation validation and common
commands; 14/31 inspect exported runtime dependencies. No such tests are claimed
to have run in 00.
