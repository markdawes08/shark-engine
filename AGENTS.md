# Shark repository instructions

## Scope and source control

Read the assigned roadmap increment, [architecture decisions](docs/architecture/README.md),
the affected domain contracts below and latest relevant [handoff](docs/handoffs/)
before changing code. Extend existing systems and work on the requested increment.
Inspect `git status --short`, `git branch --show-current` and `git log -1 --oneline`;
if no commit exists, report that fact. Preserve unrelated work.

The ignored `AI_Game_Engine_Agent_Roadmap.txt` is optional local context. If absent,
use the versioned [requirements](docs/coverage/requirements.json), execution plan
and handoff; report genuinely missing instructions rather than inventing them.
Builds and CI must not depend on the local roadmap.

**The user owns staging, commits and pushing.** Never run `git add`, stage through
another tool, commit or push, including from tests or temporary repositories.
Leave reviewable changes and provide suggested commit text after updates.
Review [.gitignore](.gitignore) each increment: ignore generated outputs, caches,
local environments and the local roadmap, while keeping source, fixtures, skills
and documentation eligible for version control.

## Product and current scope

Shark targets a **seamless open-world MMORPG**, with human editor users and
terminal-based LLM agents as equal development audiences. Read the
[MMORPG plan](docs/roadmap/mmorpg-plan.md), [ADR 0007](docs/architecture/0007-mmorpg-architecture.md)
and [human/agent plan](docs/roadmap/human-agent-plan.md) before system or API design.
Original assignments 00-35 keep their IDs; M01-M10 follow the manifest's explicit
`assignment_order`. Do not infer execution order from numeric IDs alone.

The [current-machine scope](docs/architecture/0010-current-machine-scope.md)
limits required qualification to this Windows PC's RTX 4070 Laptop and Intel GPUs,
plus CPU/headless Server work in retained WSL2 Ubuntu. Use local processes,
loopback or Windows/WSL connections, deterministic network faults, local storage/
backups and available desktop/audio devices. Use ADR 0007's explicit LOCAL-MMO-1
workload. Do not change host/WSL resource or kernel settings, require additional
hardware/cloud/accounts, or claim production capacity from local tests. Native
Linux graphics is outside scope. Mac support, CI, packages and hardware requirements
are retired by [ADR 0009](docs/architecture/0009-windows-linux-platform-scope.md).
Hosted CI is supplemental; remote branch protection is unchanged.

**Desktop interaction is paused by user instruction (2026-10-29) until the user
explicitly resumes it.** Continue coding, builds and headless checks. Do not use
desktop automation, take focus, inject input or launch windowed tests, including
automated GPU fixtures. Record required desktop checks as deferred/NOT_RUN in
the [nonblocking UI backlog](docs/backlog.md), preserving existing results.
This user amendment permits later implementation to proceed before those checks;
it does not qualify untested behavior or close full UI acceptance. Do not infer
permission to resume from elapsed time or a new coding request.

**Current handoff: [17b, transform manipulation and drag transactions](docs/handoffs/17b-transform-manipulation.md).**
Follow [ADR 0043](docs/architecture/0043-transform-manipulation.md) and the
[manipulation contract](docs/transform-manipulation.md): cumulative local/world
math uses full parent matrices, explicit snapping and existing TRS rejection.
Drag updates use pure preview; one retained set_transform commits one history
group. Cancel never writes a stale baseline, and a rejected newest update cannot
commit an older candidate. Preserve selection/revision fences and original
uncertain-outcome reconciliation. Next is 17c, gizmo integration and candidate
presentation through these services, in the
[viewport checkpoints](docs/roadmap/viewport-interaction-plan.md).
17a adds display-only orbit/pan/dolly and revision-bound triangle picking from the
same RenderScene used for the viewport. Follow [ADR 0042](docs/architecture/0042-viewport-navigation-and-picking.md)
and the [navigation contract](docs/viewport-navigation.md): validate the optional
view camera before culling, retain authored-camera provenance and full affine
transforms, use actual image bounds and preserve focus/modal cancellation.
Navigation/selection must not change authored state or history.
Full REQ-17 remains NOT_RUN. 16e's headless project handoff remains recorded in its
[handoff](docs/handoffs/16e-editor-terminal-acceptance.md); actual connected UI
acceptance follows the [procedure](docs/editor-acceptance.md) after explicit
desktop resumption. Full REQ-16 remains NOT_RUN under the backlog amendment.
Follow the [editor checkpoints](docs/roadmap/editor-plan.md),
[shell contract](docs/editor-shell.md), [workspace contract](docs/editor-scene-workspace.md)
the [inspector contract](docs/editor-inspector.md) and
[asset/console contract](docs/editor-assets-console.md), following
[ADR 0041](docs/architecture/0041-editor-scene-workspace.md). The native shell adapts
the existing retained authoring owner in process; display values are not another
mutable Scene. Preserve revision-bound save/discard, original publication receipts,
one writer and explicit save/close/reopen handoff. Native close cancellation lets
the UI resolve dirty state before teardown. Input/text/modal focus gates shortcuts.
ImGui belongs only to native Editor; Client GUI contracts remain engine-owned.
ScenePanel keeps bounded query observations, revision-scoped selection and original
retained mutation outcomes. The typed CPU viewport read extracts the original
retained scene with fresh client asset closures and explicit catalog inventory;
it never imports or writes source implicitly. GUI composition retains scene GPU
owners through completion and exposes only engine-owned texture choices.
Inspector drafts bind to an entity and full revision, require explicit Apply,
and never silently rebase after a selection or history change. Hierarchy,
component edits and ticket-bound undo/redo use one retained mutation path;
preserve known success if a subsequent display refresh fails. Save preserves
history and does not implicitly apply a draft.
AssetPanel retains explicit catalog/cache observations and one exact metadata
token. Drawing/listing never publishes imports or acknowledges job outcomes;
drain publishes at most one ready result and take releases a terminal slot.
Preserve last-valid output and original cancellation/publication/take receipts.
Metadata edits are outside scene undo, and unapplied drafts require explicit
close resolution. Bounded console logging cannot alter operation outcomes.
Project status observation failure marks cached state unknown while preserving
the original reply; scoped cleanup stays available. Actual UI/terminal parity
retains 16e; gizmos and connected transform acceptance remain 17c-17d.
Full REQ-16 remains NOT_RUN; current assignment is 17.

15a-15d supply the CPU `SharkAuthor` host, retained project ownership, shared
editing/persistence/imports, static deterministic runs and configured export.
`SharkAuthorClient` adds optional native capture through the same transport.
Follow [ADR 0039](docs/architecture/0039-terminal-authoring-protocol.md)
and the [protocol contract](docs/authoring-protocol.md): bounded UTF-8 framing,
truthful capabilities, one project writer and revision-bound single-use cursors.
Native handles stay private. Full REQ-15 is complete under current-machine scope;
the [terminal checkpoints](docs/roadmap/terminal-authoring-plan.md) record its gates.
**16, the human editor UI**, consumes the same services and ownership.
The [public terminal example](examples/terminal_authoring.py) creates, imports,
edits, saves/reopens, runs and optionally captures/exports a real project.
Route new writes
through the retained shared owner; request correlation is not mutation retry identity.
Preserve all three mutation outcome levels and explicit file publication/durability
receipts. Text access excludes typed catalog paths and retains exact file tokens;
discard-close requires the current revision. Asset metadata has separate bounded
exact-file tokens; catalog inventories are explicit. Imports use the existing
four-slot job service and publish only on explicit drain. Preserve terminal
outcomes until take, last-valid cache output, and cancel/join before owner release.
Run/export/capture consume this same retained owner and preserve dirty state/history.
`scene.run` is a static integrity case, not gameplay or Lua execution. Export uses
trusted startup helper paths, existing package assembly and final owner validation;
lost completion after a publish grant requires destination reconciliation. Capture
uses a separate capability-gated Client adapter, bounded raw image/depth artifacts
and explicit source versus reconstructed scene identities. CPU Tools stays independent.
Preserve connected public-process coverage and independent imported-geometry checks;
the example is not a second authoring service or a gameplay runner.
All [five rendering checkpoints](docs/roadmap/scene-rendering-plan.md) and full
REQ-13 are complete under current-machine scope. Full REQ-14 is complete under
the approved scope and [backlog amendment](docs/backlog.md). The
[export checkpoints](docs/roadmap/export-plan.md) record isolated scene packages
on both GPUs, default startup and actual F5 rejection/replacement. Extend the
existing shared owners; do not introduce a second mutation path or player protocol.
Follow [ADR 0038](docs/architecture/0038-standalone-scene-export.md) and the
[standalone export contract](docs/standalone-export.md): preserve the retained
project owner through final validation/publication, full asset notices, exact
Runtime CRT closure and Core-only startup format. Runtime package loading checks
owned file snapshots and retains its importer exclusion; CPU inspection is
portable, actual package export/launch is Windows x64. Package checks use an
initially empty external working directory per launch, record driver-created
outputs and audit observed modules. Preserve the distinction between isolated
validation-off deployment and required-validation rendering evidence.
Follow [ADR 0037](docs/architecture/0037-client-export-content.md) and the
[client export contract](docs/client-export.md): capture the startup scene and
explicit camera through CPU Tools, require fresh client-permitted mesh closures,
preserve unsaved edits/history and validate the owned candidate again before
publication. Retain bounded work, dependency attribution and the seventeenth
FeatureWorld case. Continue extending the existing packaging tools. Follow
[ADR 0032](docs/architecture/0032-render-components.md) and the
[component contract](docs/render-components.md): optional Camera/MeshRenderer share
Scene validation, authoring, history, retries and source persistence. Query their
fields explicitly. [Prepared content](docs/prepared-content.md) and
[ADR 0033](docs/architecture/0033-prepared-runtime-content.md) define Core-only
byte loading, immutable asset snapshots and reserved built-in cube/plane IDs.
Tools prepares cached glTF/captured scenes; Runtime must retain its importer
exclusion. [CPU extraction](docs/scene-rendering.md) and
[ADR 0034](docs/architecture/0034-scene-extraction.md) define revisioned owned
snapshots, full affine camera/model transforms, conservative bounds/culling and
explicit bounded local origins distinct from global cell anchors. Extraction
resolves post-load Core mesh mutations again, including hidden references.
Preserve both new 60-second families (seed 1337), existing cases/limits and the
sixteenth FeatureWorld case. [Offscreen scene rendering](docs/scene-viewport.md)
and [ADR 0035](docs/architecture/0035-offscreen-scene-viewport.md) add bounded float
packing and shared-depth draws through the existing GraphicsDevice. One target
persists across frames, resize adopts complete candidates, and failed GPU work
retains owners until drain/close. Preserve the new scene-unlit shader profile,
fixed readback limits and native resize/timeout/recording controls. CPU Tools stays
independent of Client. [Runtime scenes](docs/runtime-scenes.md) and
[ADR 0036](docs/architecture/0036-runtime-scene-launch.md) add explicit bounded
prepared-file loading, selected camera, dynamic sRGB presentation, F5 candidate
reload and actual presented capture. Preserve complete-candidate replacement,
fresh incarnations, bounded drain and seven native lifetime phases. Runtime has
no source importer or authoring UI. The existing Editor window/graphics/cube
flags remain diagnostics; `--editor` explicitly starts the new project shell.
Shared CPU services, terminal authoring and human authoring panels are implemented;
actual connected UI acceptance still retains assignment 16.

The user deferred remaining 05c work to the [nonblocking backlog](docs/backlog.md)
and authorized later development. Full REQ-05 remains NOT_RUN. Preserve historical
evidence and limits; revisit only under backlog triggers or at the user's request,
and do not repeat deferred items in routine updates/handoffs.

Completed checkpoint counts and evidence belong in their original handoffs and
receipts, not copied chronologies here. The [coverage guide](docs/coverage/README.md)
links completed owners and current mappings.

## Testing and evidence

**Read [docs/testing-policy.md](docs/testing-policy.md) before selecting verification.**
The user-approved policy supersedes older frequency, full-matrix and evidence-process
instructions in skills, roadmaps, ADRs and handoffs. Preserve behavioral requirements,
tests, fixtures, independent expected results, workloads, seeds, timeouts and
historical results. Preservation does not mean rerunning every test/profile.

Select checks by changed behavior and affected owners. Assignment closure covers
the affected subsystem's full Debug inventory, not every test in a configured
build. Bare CTest and `tools/ci.py` are for explicit whole-project or CI work.
Documentation-only work uses relevant repository/link/manifest checks and diff
review, without engine builds/tests. Reuse valid final-input results. Stop after
selected checks pass unless a change, failure or concrete concern justifies more.
Applying this policy needs no further confirmation.

Use the existing Catch2/CTest foundation and runners. New behavior families need
appropriate labels, explicit timeouts, fixed seeds and controllable clocks; avoid
arbitrary sleeps. Distinguish PASS, FAIL and NOT_RUN. Missing prerequisites and
empty selections are not passes. CPU/synthetic fixtures do not qualify GPUs or
audio. Preserve failures and investigate causes; never widen tolerances, shrink
workloads, regenerate image baselines or retry blindly to obtain green.

Use concise commands/results/logs and an existing handoff. Do not create new
per-checkpoint source-freeze ledgers, hashed-helper chains or exhaustive object
audits by default. Existing artifact integrity checks and consumed report formats
remain required; historic receipts remain unchanged. Successful negative-control
copies belong in managed scratch; retain unexpected failures, not duplicate inputs.

## Build and dependency boundaries

Follow [building](docs/building.md), [CI](docs/ci.md),
[ADR 0001](docs/architecture/0001-targets-and-boundaries.md) and
[ADR 0002](docs/architecture/0002-dependencies.md). Windows Ninja presets require
the x64 MSVC environment selected by `tools/windows_dev.ps1`:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug --show-only
```

Build affected targets through the same wrapper and list the inventory before
selecting tests. Use matching configure/build/test
presets. `tools-debug` builds independent CPU Tools; `runtime-only` and
`server-only` disable tests and qualify protected build boundaries, not behavior.
Reuse canonical builds and exact offline dependency caches; never run concurrent
builds in the same directory.

Use C++20 without compiler extensions. Engine warnings are errors: MSVC uses
`/W4 /WX /permissive- /Zc:__cplusplus /utf-8`; GCC/Clang use
`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`. Fix causes and
scope flags to engine targets, without imposing them on vendor sources. Use the
pinned clang-format and [.clang-format](.clang-format); avoid unrelated formatting.
Only `gltf_source.cpp` uses private MSVC `/EHc-` after `/EHsc` for its bounded parser
allocation boundary.

| Target | Boundary |
| --- | --- |
| EngineCore | Shared scene/math/timing/networking and CPU contracts; no presentation/authoring dependencies |
| EngineClient / Engine | Client presentation; Engine is the client composition interface |
| EngineTools | CPU authoring/imports using Core, independently buildable without Client/Editor |
| Editor | Tools and Client; adapters use shared authoring services |
| Runtime | Engine only; no Editor, Tools, ImGui/ImGuizmo, source parsers/decoders or offline shader compiler |
| EngineStorage / Server | Private SQLite belongs only to server Storage; Server uses Core/Storage without GPU/display/audio/authoring SDKs |

Never reach Tools through an Engine transitive dependency, generated header or
plugin. Keep EnTT/Jolt/NVRHI/TLS/socket details private. Public contracts use
engine-owned types; centrally configured GLM via `shark/math.hpp` and SharkMath
is the math exception. Core's exact Mbed TLS/TF-PSA inputs are private; Server adds
SQLite. Tools' JSON/cgltf/image decoders stay out of protected Runtime/Server
configure, link and package closures. Server's bootstrap CLI does not open a Store.

Add exact immutable dependency inputs, licenses, options and patches to
[dependencies.lock.json](dependencies.lock.json) before use. No floating tags,
competing resolution, hidden transitive downloads or future-subsystem stubs.
Catch2 is test-only. Native Windows presentation uses the pinned Vulkan/NVRHI
stack; Core/Server and WSL CPU clients keep native window/graphics options OFF.

## Ownership and public APIs

Follow [ADR 0005](docs/architecture/0005-simulation-and-rendering.md),
[application lifecycle](docs/application-lifecycle.md) and
[time/work](docs/time-and-work.md) before ownership or scheduling changes.
Application roots own services; scenes own entity/script/physics state; managers
own immutable assets. Prefer RAII and unique ownership. Shared asset reference
counts do not make mutation thread-safe. Unwind partial startup in reverse order.
Reuse existing clocks, distinct offline/server policies and bounded value
completion primitive; do not add a second scheduler or generic job framework.

The client main thread owns GLFW and local scene/authoring operations. Server
simulation has one writer per authoritative region, with fenced ownership and
bounded messages across regions. Workers return owned bounded values or immutable
results, never mutate registries. Physics workers never invoke Lua. Audio callbacks
do not block, allocate generally or mutate scenes. Retire GPU resources only after
submitted use completes. Use checked generational handles across mutations rather
than borrowed component pointers.

Offline time is 60 Hz; provisional authoritative server time is 20 Hz with
independent replication/presentation. Server time never pauses on client focus or
minimize and never silently drops authoritative ticks. Render callbacks cannot
grant gameplay outcomes. Keep global cell identity distinct from bounded local
origins; internal regions must not create zone breaks or duplicate authority.

Expected failures return owned `Result<T>`/`Error` with stable codes and useful
operation/entity/asset/path/request context. Follow [Core contracts](docs/core-contracts.md)
for bounded UTF-8 diagnostics, configuration, IDs and paths. Keep nonzero process
failure codes. Assertions cover programmer invariants, not external validation.
Exceptions cannot cross Lua C, worker or real-time callbacks; cleanup must not
throw. Device failures need diagnostics and bounded teardown.

Read applicable contracts before extending a domain:

| Domain | Contracts and required invariants |
| --- | --- |
| Scenes | [Scene](docs/scene-contracts.md), [transforms](docs/transforms.md), [inspection](docs/scene-inspection.md): private EnTT owner, mandatory Name/Transform, deferred queue, scoped identities, complete hierarchy validation and bounded revisioned queries |
| Authoring | [Commands](docs/authoring-commands.md), [batches](docs/authoring-transactions.md), [history](docs/authoring-history.md), [retries](docs/authoring-retries.md): one AuthoredScene owner, read-only scene borrow, expected revisions, atomic candidates, bounded history and explicit retry reconciliation |
| Source persistence | [Documents](docs/source-documents.md), [activation](docs/authored-scenes.md), [files](docs/project-files.md): incarnation-bound metadata, dirty protection, complete candidates, root/path content revisions and truthful publication/durability outcomes |
| Networking | [Transport](docs/network-transport.md), [sessions](docs/network-sessions.md), [replication](docs/network-replication.md), [interest](docs/network-interest.md): actual ready mTLS admission, session/stream fences, selected public poses, privacy and connection budgets |
| Live durability | [Store](docs/durable-store.md), [characters](docs/durable-characters.md), [transactions](docs/durable-transactions.md), [recovery](docs/durable-recovery.md), [maintenance](docs/durable-maintenance.md): trusted server state, sequenced sessions, atomic economic outcomes, timeout reconciliation and isolated restoration |
| Assets | [Identity](docs/asset-identity.md), [cache](docs/asset-import-cache.md), [jobs](docs/asset-import-jobs.md), [glTF](docs/gltf-assets.md): stable IDs, exact dependency/settings keys, bounded captured inputs, owner publication fences and last-valid output |
| Presentation | [Input](docs/input-contracts.md), [window](docs/window-lifecycle.md), [frame loop](docs/frame-loop.md), [device](docs/graphics-device.md), [presentation](docs/presentation-lifecycle.md), [shaders](docs/shader-pipeline.md), [resources](docs/gpu-resources.md): shared frame/input pump, explicit capabilities, offline shaders and bounded GPU lifetimes |

Do not expose private candidate handles or mutable invariant-fixture ports to
adapters. Check outer and nested command Results; retained mutations have three
Result levels. Extend component copying, metadata capture/restoration, exact retry
input comparison and retained charges together when fields grow. Save keeps
history; activation clears it. Recreated entities get fresh generations. Out-of-band
writes and foreign/expired retry IDs require explicit reconciliation.

CharacterAuthority keeps session/Scene admission on its owner thread while its
private worker owns Store. A timeout is not rollback: drain/take outcomes, reopen
explicitly and reconcile original operation IDs. Drain/destroy live owners before
maintenance; migration needs an exact pre-upgrade backup, and restore creates an
isolated candidate rather than authorizing stale rollback. Do not restore sessions/
connection IDs or expose mutable storage/session bypasses.

Human editor and terminal writes share schemas/services and one project owner,
or reject conflicting access. Preserve unsaved changes, common undo and explicit
retry outcomes. CPU authoring needs no editor, display, GPU, LLM SDK or MCP client;
device-dependent capture is capability-gated. Local authoring protocols are not
player protocols. Players submit bounded authenticated intents; the server owns
simulation, inventory, currency and progression. Administration is separately
authorized. Do not add premature interface stubs.

## Formats, coverage and retained storage

[ADR 0004](docs/architecture/0004-formats-and-identity.md) separates source, cooked,
authoring protocol, gameplay wire, database and Lua versions. Durable IDs,
ownership epochs, local handles and replica IDs are distinct. Authored saves do
not implement live economy durability. JSON is UTF-8 with format/schema fields;
reject unsupported versions, unknown fields outside bounded extras, duplicate
keys/IDs, nonfinite numbers and escaping paths. Public format changes need tested
migration or explicit rejection. Validate complete candidates before activation;
publish saves/imports/cooks atomically. Never serialize raw structs or handles.

Update [requirements/API coverage](docs/coverage/README.md) with behavior. Each
supported component, command and Lua entry maps to actual tests and FeatureWorld
cases or a justified external fixture. Extend real cases, independent expected
state and mappings together. Keep saved source/IDs stable; runner persistence
uses an explicit scratch parent through ProjectFiles. Lua remains empty until its
owner lands. Isolated network/storage/platform/GPU fixtures remain separate.
MMOWorld in M09, not Tetris or a static scene, qualifies the MMO reference module.

Follow [storage retention](docs/building.md#storage-retention). Reuse one active
build per platform/compiler/profile and canonical dependency archives. Clean
successful temporary deployments; archive older evidence with verified bytes and
original paths. Inspect references before removing inputs: old CMake caches and
helpers can depend on earlier `out/` paths. Never delete whole `out/`, build or WSL
trees by age, or discard unique failure evidence. Same-disk copies are not
independent backups.

## Repository skills and handoff

Use only the focused skill relevant to the change:

- [shark-build-test](.agents/skills/shark-build-test/SKILL.md): builds, CTest,
  sanitizers and dependency boundaries.
- [shark-renderer-validation](.agents/skills/shark-renderer-validation/SKILL.md):
  graphics behavior, readbacks and current-PC GPU evidence.
- [shark-component-api](.agents/skills/shark-component-api/SKILL.md): components,
  schemas, shared commands/Lua and FeatureWorld coverage.
- [shark-asset-import](.agents/skills/shark-asset-import/SKILL.md): bounded import,
  cache/jobs, stable identity and licensed fixtures.
- [shark-release-packaging](.agents/skills/shark-release-packaging/SKILL.md):
  protected/runtime closure, export/cooking and isolated deployment.

Skills live in `.agents/skills/<name>/SKILL.md`. Repository/contract validators
check discovery inputs, ignore boundaries and links; they do not prove a client
UI loaded a skill. The user controls committing these sources.

Update the relevant handoff with base revision, delivered behavior, changed public
contracts, selected commands/results, verified platforms, justified omissions,
coverage changes, limitations and exact next work. Do not advance past an unmet
required gate except under the user's explicit backlog amendment. End the response
with important changes, verification limits and suggested commit text.
