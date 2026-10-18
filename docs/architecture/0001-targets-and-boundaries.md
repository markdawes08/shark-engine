# ADR 0001: Targets and protected boundaries

Status: accepted contract, amended for the seamless-world MMORPG direction;
M01's bootstrap split is implemented; see its handoff for qualification. Date: 2026-10-17.

## Decision

Use C++20 and target-based CMake. The project declares CMake 3.28 as its floor;
01 uses pinned CMake 3.31.10. No compiler extensions in engine interfaces. Exact
compiler, SDK, dependency and CI-image versions belong in the build evidence and
dependency lock introduced in 01.

M02a adds C compilation for the private, pinned Mbed TLS/TF-PSA-Crypto graph in
every Core configuration, including `server-only`. Engine-owned interfaces stay
C++20. TLS/socket implementation types do not escape Core; GLM through
`SharkMath` remains the documented public math exception. The
[M02a handoff](../handoffs/M02a-transport-wire.md) records completed current-machine
qualification under [ADR 0011](0011-network-foundation.md).

M01 separates static `EngineCore`, `EngineClient` and `EngineTools` libraries,
retains `Engine` as an INTERFACE client composition target, and adds `Server`
alongside `Editor`, `Runtime` and Catch2 `Tests`. Core contains version/argument
behavior and the strict server configuration parser. Client contains bootstrap
help presentation, 04a device-free raw input, 04b owned native Windows GLFW
windows and 04c's [offline frame coordinator](../frame-loop.md), which reuses
Core timing for first-tick input consumption and local pause/step. 05a adds the
[owned graphics device foundation](../graphics-device.md): adapter/capability
selection, Window/instance/surface/device/NVRHI ownership and bounded validation.
It does not yet submit GPU work or present frames. Public contracts contain only
engine-owned types; native libraries stay private to EngineClient.
The [input contract](../input-contracts.md) and
[04 checkpoint plan](../roadmap/window-input-plan.md) keep native GLFW integration
separate from CPU input tests. The [window lifecycle](../window-lifecycle.md)
implements the native owner while retaining a device-free test/backend path.
Raw input and window code remain in Client, outside Core/Server.
Server has a real idle lifecycle,
bounded execution and signal-driven shutdown. `runtime-only` proves client
exclusion of authoring and tests; `server-only` enforces the stricter Core boundary.
The [01 handoff](../handoffs/01-build-foundation.md) records local evidence; the
[CI verification handoff](../handoffs/01-ci-verification.md) records hosted CPU
qualification and deliberate-failure/red-to-green proof.

## M01 target split and later responsibilities

The former plan placed simulation, graphics, input and audio together in Engine.
Before those dependencies are integrated, separate device-free shared behavior
from client presentation. [ADR 0007](0007-mmorpg-architecture.md) and the
[MMORPG plan](../roadmap/mmorpg-plan.md) own the continuous-world design and future
milestone ordering. Internal partitions/cells are ownership and streaming units;
they do not impose player-visible zone transitions.

| Target | Kind | Responsibility and allowed direct engine dependencies |
| --- | --- | --- |
| EngineCore | Static library, introduced in M01 | Device-free configuration, identity, lifecycle, scene/schema/hierarchy and M02a's private TLS/wire foundation; later simulation, physics, Lua and cooked simulation-data loading; no client or authoring dependencies |
| EngineClient | Static library, introduced in M01 | EngineCore; window/input, GLFW, NVRHI/Vulkan rendering, audio and client presentation |
| Engine | INTERFACE compatibility/composition target | EngineClient, transitively EngineCore; preserves Runtime's existing engine-facing composition |
| EngineTools | Static library | EngineCore; device-free project/source persistence, commands/undo, imports, cook/export, discovery and authoring services; client preview is a separately gated adapter/target and cannot enter the CPU host's dependency closure |
| Editor | Executable | EngineTools and client presentation services; Dear ImGui/ImGuizmo views of shared services |
| Runtime | Executable | Engine only among engine libraries; client startup and game loop |
| Server | Executable, introduced in M01 | EngineCore only among engine libraries; headless configuration and lifecycle; gameplay listener, authoritative simulation and persistence remain later work |
| SharkCLI | Executable, introduced in 14/15 | EngineTools; export first, then CLI and JSON-RPC stdio authoring host |
| Tests | Test target(s) under one CTest suite | Split engine-only tests from authoring integration tests; one primary C++ framework |

There is no EngineCore-, EngineClient- or Engine-to-EngineTools dependency, even through a shared utility,
generated header, static initializer, transitive target, or runtime plugin.
Runtime must neither link nor load Editor, EngineTools, ImGui, ImGuizmo, cgltf,
source image importers, or offline shader compilers. Inspect both the CMake
dependency graph and final runtime package in 01, 14, 16 and 31. Runtime loads
engine-owned development payloads for the early export in 14 and only cooked
payloads when 31 replaces that path. It never imports glTF at launch.

A Server/Core-only configuration must neither discover, download nor build client
presentation or authoring dependencies. Server and EngineCore must never link or
load them. This excludes EngineClient,
the Engine client composition target, Editor, EngineTools, GLFW, NVRHI, Vulkan
headers/loaders, platform window frameworks, audio-device libraries, ImGui,
ImGuizmo, source importers and offline shader compilers. Server-only configuration
must gate dependency discovery and FetchContent before those operations occur;
omitting a final link is insufficient. Core physics, scripting and simulation
data dependencies remain separately owned, pinned and qualified as they land.
Test frameworks belong only to enabled test configurations, never the server
package. Disabling graphics at runtime is not the headless build contract.

The permitted current Core dependency graph is private EnTT, public `SharkMath`
over GLM, and private static Mbed TLS/TF-PSA-Crypto. The TLS graph contains three
static libraries and seven explicitly reviewed object targets; the Windows
`ws2_32`/`bcrypt` imports are classified OS dependencies. It adds no display,
client or authoring dependency. A test-only `NetworkProbe` exercises real local
connections; this does not make the Server executable a gameplay listener.

M01 uses `engine/core/`, `engine/client/` and `server/`, retaining
`engine/include/shark/bootstrap.hpp` as the common public bootstrap header.
`engine_tools/`, `editor/`, `runtime/`, `tests/`, `cmake/`, `tools/` and the `docs/`
directories remain established locations. `cli/` and `assets/FeatureWorld/`
remain future work. Table responsibilities beyond bootstrap configuration/help
are assigned future behavior, not implemented subsystem claims.
Assignment 01 establishes actual commands and provides local
skills in `.agents/skills/<skill-name>/SKILL.md`, with links from AGENTS.md. The
user controls staging and committing those files.

## Shared behavior

EngineCore owns component validation and the safe scene mutation boundary.
07a implements the [scene ownership boundary](../scene-contracts.md) in Core:
private EnTT, checked scope/identity/lifetime, owner-thread access and deferred
create/destroy. 07b adds Name/Transform and hierarchy; 07c adds schema validation
and immutable revisioned queries.
EngineTools wraps that behavior in revision-checked authoring transactions,
history and project services. UI, CLI, protocol handlers and integration tests
call those same services; widgets never edit authoritative components directly.
Gameplay uses validated engine operations without authoring undo history.
Server-owned state and client presentation have separate authority under ADR 0007;
sharing Core code does not make a client an authorized world writer. The existing
local authoring CLI/JSON-RPC contract is not a multiplayer transport.
Read queries return snapshots or checked handles, not mutable storage pointers.

[ADR 0008](0008-human-agent-authoring.md) makes terminal and human authoring
first-class workflows. CLI access requires no editor window or LLM integration;
CPU authoring excludes device dependencies while capture uses an optional client
adapter. Editor and CLI share one project owner or explicitly reject a second
writer, with revision-checked edits, common history and tested save/reopen handoff.
The current Tools library contains only build metadata; these services land in
their assigned increments, with the public terminal surface completed in 15.

EnTT stays behind an engine-owned interface. Start with an ordered forward-render
pass list, explicit resources and dependency injection through constructors.
Do not introduce a general render graph, service locator, job framework or plugin
system as a foundation requirement.

## Lifetime, threading and failure

03b implements this policy for the current Server's actual signal resources
and readiness transition; see the [lifecycle contract](../application-lifecycle.md)
and [handoff](../handoffs/03b-application-lifecycle.md). Application roots explicitly
own their resources; there is no registry, service lookup or dependency graph.
Later subsystems extend that ownership at their integration gates.
[03c time/work](../time-and-work.md) adds monotonic timing, distinct scheduling
profiles and bounded value completions with real producer/owner fixtures. The
Server remains worker-free until a real subsystem requires asynchronous work.

Each application root exclusively owns its services. A scene owns its entity
registry and per-scene script/physics state; resource managers own shared immutable
assets through typed handles. Use RAII and unique ownership by default. Reference
counting is reserved for shared asset lifetimes; it does not make mutable scene
access thread-safe. Startup records successful stages and unwinds in reverse
order on failure. Shutdown stops producers, joins workers, drains/invalidates
queues and releases consumers before providers. Client shutdown also waits for
submitted GPU use; Server has no GPU or audio lifecycle.

In client applications the process main thread controls scene mutation, VM calls,
GLFW event processing and command commits. CPU import and physics workers return data/events; they
cannot call Lua or touch authoritative scene storage. The audio callback consumes
preallocated bounded messages, never file I/O, blocking locks, general allocation
or scene mutation. GPU resources retire after their last submission completes.
The scheduling details are in [0005](0005-simulation-and-rendering.md). This also
satisfies GLFW's main-thread initialization/window/event requirements.
[GLFW threading contract](https://www.glfw.org/docs/latest/intro_guide.html#thread_safety)

The Server prototype starts in one process with one writer per internal world
partition; the initial simulation writer can run on its process main thread.
Network and persistence work introduced later returns bounded messages/results
to that writer and cannot mutate its live storage directly. Server simulation
does not depend on presentation, focus, minimization, client disconnection or a
GLFW event pump. M05 owns multi-process ownership transfer while preserving a
continuous player world; M01 does not introduce a general job framework. The
server tick/overload contract belongs to ADRs 0005 and 0007.

Server startup validates configuration before accepting work and unwinds partial
initialization. Shutdown stops new work, terminates bounded pending activity and
joins owned workers within a documented deadline, with a diagnostic/nonzero
outcome when graceful completion fails. Durable recovery semantics arrive with
M03; a foundation shutdown test does not prove persistence or failover.

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

M01 extends the target-domain graph to distinguish Core, client, authoring and
test dependencies, checking Server/EngineCore separately from Runtime. Negative
fixtures must reject direct and transitive client/authoring leakage, including
generator-expression links. A fresh server-only configure/build must succeed
without discovering/downloading client dependencies, and its package must pass
an independent linked-library inventory. Native Ubuntu 24.04 headless CTests must
cover actual Server CLI/config validation, no-display startup, bounded smoke
execution, partial-startup failure and clean shutdown. Preserve the existing
client CPU matrix and runtime boundary tests. New presets, test names, CI jobs
and evidence are documented in the [M01 handoff](../handoffs/M01-headless-server.md).
This does not establish any multiplayer, simulation or capacity result.
