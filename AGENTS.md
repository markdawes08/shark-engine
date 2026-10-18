# Shark repository instructions

## Scope and source control

Read the assigned roadmap increment, [architecture decisions](docs/architecture/README.md)
and latest [handoff](docs/handoffs/) before changing code. Work on that increment
only and extend existing systems. Inspect `git status --short`,
`git branch --show-current` and `git log -1 --oneline`; if no commit exists,
report that fact rather than inventing a base revision. Preserve unrelated work.
If the ignored local roadmap is absent, use the versioned
[assignment requirements](docs/coverage/requirements.json) and handoff; report
any genuinely missing instruction rather than inventing source text.

The product target is a **seamless open-world MMORPG**. Read the versioned
[MMORPG execution plan](docs/roadmap/mmorpg-plan.md) and
[ADR 0007](docs/architecture/0007-mmorpg-architecture.md) before system design.
They supersede the original offline-only scope. Original assignments 00-35 retain
their IDs; M01-M10 are inserted in the manifest's explicit `assignment_order`.
Assignment 01's hosted CPU and red/green gate is complete; see the
[CI verification handoff](docs/handoffs/01-ci-verification.md). M01's gate is
complete. The headless Server bootstrap and Core/Client
split are implemented; the [M01 handoff](docs/handoffs/M01-headless-server.md) records
local qualification and the [hosted follow-up](docs/handoffs/M01-ci-verification.md)
records the completed native CPU, headless Docker and red/green gate. **05b,
swapchain, submission and presentation lifecycle, is complete under the
current-machine scope.** **05c failure/stress fixtures are implemented, but its
qualification gate remains open:** native investigation found one retained GDI
object per NVIDIA swapchain/window lifetime, and an Intel ASan fault child timed
out after recording native cleanup. Do not mark full 05 PASS or start 06 while
these remain unresolved. Read the
[05c handoff](docs/handoffs/05c-presentation-qualification.md) and
[qualification contract](docs/presentation-qualification.md) before continuing;
preserve its failed evidence and fixed resource limits. Follow the
[05 checkpoint plan](docs/roadmap/graphics-device-plan.md),
[graphics device contract](docs/graphics-device.md),
[presentation lifecycle contract](docs/presentation-lifecycle.md) and
[05b handoff](docs/handoffs/05b-presentation-lifecycle.md). The completed
[05a handoff](docs/handoffs/05a-graphics-device.md) retains device-foundation
evidence. Full `REQ-05` remains NOT_RUN until 05c passes. Native
Windows clients privately use the exact pinned Vulkan/NVRHI stack; Core/Server
and WSL CPU clients keep both native options OFF. Required device validation
passes on both current-PC GPUs. 04c and full assignment 04 remain complete. Follow the
[window/input checkpoint plan](docs/roadmap/window-input-plan.md),
[input contract](docs/input-contracts.md),
[window lifecycle guide](docs/window-lifecycle.md),
[frame-loop guide](docs/frame-loop.md) and
[04c handoff](docs/handoffs/04c-frame-loop.md). Full `REQ-04` is PASS after final
Windows/WSL CPU, native window and protected standalone/package qualification.
`OfflineFrameLoop` retains input over zero-tick frames and consumes one-shot
actions on the first fixed tick. Existing Core timing owns offline pause/step;
the first resumed frame admits no elapsed time. Editor/Runtime expose explicit
`--graphics` clear presentation and `--window` diagnostics through the same offline
frame/input pump, with F6 pause/F7 step sample controls. These are not authoring
commands; there is no game-authoring editor UI, scene renderer or world simulation.
Assignment 03 and its [Core checkpoints](docs/roadmap/core-plan.md)
are complete. Consult the
[03c handoff](docs/handoffs/03c-time-and-work.md) for final local qualification,
the [lifecycle guide](docs/application-lifecycle.md) before extending ownership,
and the [time/work guide](docs/time-and-work.md) before scheduling or queue changes.
Reuse the existing clocks, distinct offline/server policies and bounded value
completion primitive; do not create a second scheduler or a generic job framework.
02 is complete under the user-directed
[current-machine scope](docs/architecture/0010-current-machine-scope.md). The
[02a handoff](docs/handoffs/02a-graphics-setup.md) records evidence and limits;
02b executable graphics cases and local validation are complete within checkpoint scope; see the
[02b handoff](docs/handoffs/02b-graphics-cases.md). The user removed Mac compatibility
from scope; [ADR 0009](docs/architecture/0009-windows-linux-platform-scope.md)
supersedes the earlier deferral and retires checkpoint 02c. ADR 0010 subsequently
limits client/editor qualification to this Windows PC's RTX 4070 Laptop and Intel
GPUs, with CPU/headless Server qualification in existing WSL2 Ubuntu. Do not plan, implement or
require Mac compatibility, CI, packaging or hardware qualification.
The [final 02d local handoff](docs/handoffs/02d-final-verification.md) records
Windows/Linux package work, full Windows lifecycle runs and isolated Ubuntu
CPU/package checks. The [scope handoff](docs/handoffs/current-machine-scope-amendment.md)
reuses that evidence for the two available GPU rows and local package gates.
The former six GPU rows and independent-machine checks are retired requirements,
not retroactive passes. Follow the
[graphics checkpoint plan](docs/roadmap/graphics-plan.md) and
[probe guide](docs/graphics-probe.md); local feasibility does not claim support
on other machines or a production MMO release.
Networking, persistence, simulation and streaming remain planned.

Keep all remaining gates achievable on this PC: local processes, loopback or
Windows/WSL connections, deterministic network faults, local storage and backups,
and available desktop/audio devices. Use ADR 0007's explicit LOCAL-MMO-1 workload.
Do not require additional hardware, native Linux graphics, cloud services,
external accounts, public certificates or hosted-runner access. Existing CI remains
supplemental; remote branch protection is unchanged and is not a local roadmap
gate. Investigate failures affecting in-scope behavior. Do not change host/WSL
resource settings or claim production capacity from local tests.

Human editor users and terminal-based LLM agents are equal development audiences.
Read [ADR 0008](docs/architecture/0008-human-agent-authoring.md) and the
[human/agent plan](docs/roadmap/human-agent-plan.md) before authoring/API design.
Use shared schemas/services, stable IDs/revisions, bounded semantic queries,
noninteractive commands and machine-readable results. CPU authoring must not
require an editor, display or GPU; capture has an explicit device capability.
An LLM SDK or MCP adapter is never a prerequisite. Editor and terminal writes
share one project owner or reject conflicting access; preserve unsaved changes,
common undo history and explicit retry outcomes. Add both semantic authoring
paths when features land, with earlier services exposed in 15 and editor in 16.
Current bootstrap flags are not game-authoring APIs; do not create premature stubs.

The user owns staging and commits. **Never run `git add`, stage through another
tool, commit, or push, including from tests or temporary repositories.** Leave
the change set reviewable and include suggested commit text in the final response
after updates. Review and maintain [.gitignore](.gitignore) every increment:
ignore generated output, dependency caches, local environment files and the local
`AI_Game_Engine_Agent_Roadmap.txt`. Do not ignore source, tests, repository skills,
or project documentation. The roadmap is a local planning input; builds and CI
must not depend on its presence in a checkout.

## Build, tests and formatting

Run commands from the repository root. [Building](docs/building.md) documents
tool installation, all presets and platform requirements; [CI](docs/ci.md)
documents required jobs and evidence. Windows presets require an x64 Visual
Studio developer environment with MSVC and the Windows SDK available to Ninja.
Alternatively, the repository wrapper initializes that environment and selects
the pinned tools in `.tools/venv`:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python tools/ci.py --preset windows-debug --jobs 2
```

```text
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
python tools/check_format.py
python tools/validate_contract.py
python tools/validate_contract.py --self-test
python tools/check_repository.py --self-test
```

Use matching configure/build/test preset `linux-debug` on Linux. Release presets
are `windows-release` and `linux-release`; sanitizer presets are `windows-asan`
and `linux-asan-ubsan`. The Linux sanitizer preset uses Clang. To run the automated
CPU workflow with recorded evidence:

```text
python tools/ci.py --preset windows-debug --jobs 2
```

Replace the preset with the native platform preset. To prove the runtime target
can build without authoring targets, use the host compiler environment and:

```text
cmake --preset runtime-only
cmake --build --preset runtime-only
```

This configuration disables editor and test targets; its default build includes
`shark_check_runtime_boundary`. It does not run Catch2 tests. Run the native
debug preset for the test suite.

Use C++20 without compiler extensions. Engine-owned targets treat warnings as
errors; fix the cause rather than globally suppressing diagnostics. Keep warning
options scoped to engine targets and do not impose them on dependency code.
MSVC uses `/W4 /WX /permissive- /Zc:__cplusplus /utf-8`; GCC/Clang use
`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`.
The checked-in `.clang-format` and locked clang-format version define C++ layout.
`python tools/check_format.py --fix` formats the selected source tree; review its
changes and keep unrelated formatting out of the increment.

Add behavior tests for engine-owned logic, boundaries and failure paths. Use the
single Catch2/CTest foundation and existing test labels; list current tests with
`ctest --preset windows-debug --show-only` before using a label filter. New test
families get explicit labels and timeouts in their owning assignment. Record
fixed seeds and controllable clocks; avoid arbitrary sleeps. Run focused checks
and the relevant regression suite, distinguishing PASS, FAIL and NOT RUN. A
missing runner, graphics device or audio device is NOT RUN. CPU starter tests do
not establish working graphics, audio or a supported platform. Never regenerate
image baselines automatically to hide failures.

Current labels are `cpu`, `unit`, `integration`, `boundary`, `server`, `client`, `contract`, `tooling`
and opt-in `graphics-probe` (CPU-safe CLI checks only, not GPU qualification).
`ctest --preset windows-debug -L boundary` runs client/Core/Server boundary checks;
`-L unit` runs starter engine tests and the normal failure-propagation sentinel.

## Protected target and dependency boundaries

EngineCore is the shared static bootstrap library. EngineClient is a separate
static presentation library, providing client help text, device-free raw input
state under the [input contract](docs/input-contracts.md), and owned Windows GLFW
window integration under the [window lifecycle contract](docs/window-lifecycle.md).
Its [owned presentation lifecycle](docs/presentation-lifecycle.md) provides bounded
clear submission, resize/minimize handling and resource retirement. The graphical
sample reuses the [offline frame loop](docs/frame-loop.md) and Core timing; neither
has server authority or a game-world implementation.
Its injectable window fixtures remain CPU-only; existing WSL builds report native
window capability unavailable. Raw input is never a Server gameplay protocol. Engine is an
interface composition target that links EngineClient and transitively EngineCore.
Server links only EngineCore. Server/Core exclude window/GPU/audio/authoring
libraries at configure/download, link and package time. `server-only` builds a
Release server with tests and dependency fetching disabled; `server-debug` and
`server-asan-ubsan` exercise actual server tests. `runtime-only` is a client preset.
EngineTools depends on EngineCore; Editor depends on EngineTools and EngineClient;
Runtime depends on Engine. Runtime must not link/load Editor, EngineTools, ImGui,
ImGuizmo, cgltf, source image/audio decoders or offline source shader compilers.
EngineTools may never be reached through an Engine transitive
dependency, generated header or plugin.

Follow [ADR 0002](docs/architecture/0002-dependencies.md) for allowed libraries and
integration owners. Add exact immutable inputs, licenses, options and patches to
[dependencies.lock.json](dependencies.lock.json) before first use. Do not add floating tags,
competing package resolution, hidden transitive downloads or future subsystem
stubs. Catch2 is test-only. GLFW/NVRHI/Vulkan must pass assignment 02 before
dependent renderer work on the two local Windows GPU targets; that amended
feasibility gate is now complete. Preserve the Linux CPU/Server boundary.

Keep EnTT/Jolt/NVRHI details inside their subsystem implementations. Shared public
contracts use engine-owned types; centrally configured GLM is the math exception.
UI, CLI and protocol authoring edits share validated application services and
transactions, never a second authoritative mutable scene. Those local authoring
services are not the player protocol. Server validates authenticated, bounded
intents and owns simulation, inventory, currency and progression. Client prediction
has no authority to grant outcomes; administrative access is separately authorized.

## Ownership, threading and errors

Follow [ADR 0001](docs/architecture/0001-targets-and-boundaries.md) and
[ADR 0005](docs/architecture/0005-simulation-and-rendering.md). Application roots
own services, scenes own entity/script/physics state, and managers own immutable
assets. Prefer RAII and unique ownership; shared asset reference counts do not
make mutable scene access thread-safe. Unwind partial startup in reverse order.

The client process main thread controls GLFW and local scene/authoring operations.
Server simulation has a single writer per authoritative region; ownership/fencing
and bounded messages mediate cross-region work. Workers never mutate a registry
directly. Workers return bounded messages or immutable results. Physics workers
never invoke Lua; audio callbacks perform no blocking I/O, general allocations
or scene mutations. Retire GPU resources only after submitted use completes.
Checked generational handles replace borrowed component pointers across mutation
boundaries. Follow ADR 0005's distinct clock profiles: offline 60 Hz, provisional
server 20 Hz, independently scheduled replication/presentation. Live server time
never pauses on client focus/minimize and never silently drops authoritative ticks.
Render callbacks are presentation-only for live gameplay. Use stable global cell
coordinates and bounded local origins; internal regions must not create visible
zone breaks or duplicate authority during seamless travel.

Expected failures return the owned `Result<T>`/`Error` introduced in 03a, with
stable code, message, operation and relevant entity/asset/path/request context.
Follow the [Core contracts](docs/core-contracts.md) for bounded UTF-8 diagnostics,
explicit sink ownership, configuration, IDs and paths. Keep nonzero process
failure codes; bootstrap logs are not game-authoring protocol results.
Assertions cover programmer invariants, not validation of files or commands.
Exceptions must not cross Lua C, worker or real-time callbacks; cleanup must not
throw. Fatal device failures receive diagnostics and bounded teardown.

## Formats and coverage

[ADR 0004](docs/architecture/0004-formats-and-identity.md) defines independent
source, cooked, authoring protocol, gameplay wire, database and Lua API versions.
Authored scenes are not live player persistence. Durable account/character/world
IDs, region ownership epochs, local handles and session replica IDs are distinct.
Economy/progression writes require validated, idempotent durable transactions;
file-save atomicity does not satisfy that contract. Source JSON is UTF-8 with explicit
format/schema fields. Reject unsupported versions, unknown fields outside bounded
extras, duplicate keys/IDs, nonfinite numbers and escaping project paths. Public
format changes need a tested migration or explicit version-rejection policy.
Validate complete candidates before activation; publish saves/imports/cooks
atomically. Never serialize raw structs, pointers or third-party handles.

Update [requirements and API coverage](docs/coverage/README.md) with behavior.
Each supported component, command and Lua entry maps to an executable test and
FeatureWorld case, or a justified external fixture. FeatureWorld starts in 10;
connect earlier components then and add new cases when their feature lands.
Keep planned fixtures separate from implemented coverage. The bootstrap does not
provide scene/component/Lua/import/export APIs or a scene renderer.
MMO requirements remain planned until executable multi-process, network-fault,
database, streaming and load evidence exists. M09 adds MMOWorld acceptance; Tetris
and the physics sandbox remain tooling regressions, not MMO release qualification.
Record active workload and named hardware before capacity claims. Current values
in ADR 0007 are provisional targets; tests cannot silently reduce them to pass.

## Locations and repository skills

| Location | Ownership |
| --- | --- |
| `engine/core/`, `engine/client/`, `engine/include/` | Shared Core, client presentation and retained bootstrap public header |
| `engine_tools/`, `editor/`, `runtime/`, `server/` | Authoring library and separate client/editor/headless executables |
| `tests/`, `cmake/`, `tools/` | Behavior tests, target configuration and development checks |
| `probes/graphics/` | Opt-in assignment 02 graphics, readback and lifecycle fixtures; not the production renderer |
| `docs/architecture/`, `docs/coverage/`, `docs/handoffs/` | Decisions, mappings/evidence and assignment handoffs |
| `docs/roadmap/` | Versioned execution order, scope amendments and foundation assessment |
| `.github/workflows/` | Required CI automation |
| `.agents/skills/` | Repository-scoped Codex skill sources |
| `build/`, `out/` | Ignored generated builds and test evidence |
| `cli/`, `assets/FeatureWorld/` | Future CLI and scene fixtures, created by their owning assignments |
| `assets/MMOWorld/` | Future M09 MMO acceptance world |

Use the relevant focused skill:

- [shark-build-test](.agents/skills/shark-build-test/SKILL.md): native builds,
  CTest, sanitizers and dependency boundaries.
- [shark-renderer-validation](.agents/skills/shark-renderer-validation/SKILL.md):
  GPU/platform and visual evidence when graphics exists.
- [shark-component-api](.agents/skills/shark-component-api/SKILL.md): components,
  schema/commands/Lua and FeatureWorld integration as those layers land.
- [shark-asset-import](.agents/skills/shark-asset-import/SKILL.md): bounded source
  import, cache identity and fixture provenance.
- [shark-release-packaging](.agents/skills/shark-release-packaging/SKILL.md):
  runtime separation, cooking, clean-target export and release evidence.

The exact discovery location is `.agents/skills/<name>/SKILL.md`. Codex scans
these directories from its working directory to the repository root, according
to the [official skill documentation](https://learn.chatgpt.com/docs/build-skills).
`python tools/check_repository.py --self-test` checks skill frontmatter, locations
and source eligibility under `.gitignore`; contract validation checks the
requirement mapping and document links. Neither structural check claims that a
particular client UI has loaded the skills.
In Codex CLI/IDE, `/skills` provides a client-side check after opening this
repository; the user controls committing these unignored skill sources.

## Required handoff and response

Update the assignment handoff with: assignment and base/result revision (or no
commit); behavior delivered; changed files/public contracts; exact commands and
results; platforms/hardware verified; checks not run and why; coverage/FeatureWorld
changes; blockers/limitations; and the exact next assignment. Do not advance until
the current assignment's required gate is met. End the user response with the
important changes, verification limits and suggested commit message, leaving
all staging, committing and pushing to the user.
