# Human and terminal-agent development plan and assessment

**Contributor amendment, 2026-10-27:** human contributors and AI agents select
verification using the [testing policy](../testing-policy.md). It supersedes
older repeated matrix and evidence-process demands; preserve behavior tests and
report actual selected results. The repository skills and AGENTS.md point to the
same policy. This changes contributor workflow, not product authoring parity.

Date: 2026-10-17. User direction: tune Shark for both a human developer and a
terminal-based LLM agent. Reviewed clean `main` at `d9f53ef` (M01 implementation).
This is a planning amendment, not implementation of the authoring CLI or editor.

[ADR 0008](../architecture/0008-human-agent-authoring.md) is the product contract.
It supplements the [MMORPG execution plan](mmorpg-plan.md) and
[requirements manifest](../coverage/requirements.json). Original assignment IDs,
MMO insertions and then-current assignment M01 remained unchanged. At the planning
review, M01's hosted gate was recorded as open. Its later
[hosted verification](../handoffs/M01-ci-verification.md) now closes that gate at
`d658d69`; the historical assessment below remains tied to the reviewed baseline.
No authoring implementation or parity evidence is inferred from the CI result.
The later [current-machine decision](../architecture/0010-current-machine-scope.md)
limits qualification to Windows editor/client/terminal workflows on the available
PC and CPU/server fixtures in existing WSL Ubuntu. It supersedes the wider
Windows/Linux platform matrix. Mac and native Linux graphics are outside active
scope; human/terminal parity is unchanged. No additional hardware, provider
account, external model service or hosted runner is needed to complete the plan.

11a adds [shared CPU asset metadata/catalog services](../asset-identity.md),
qualified in the [identity handoff](../handoffs/11a-asset-identity.md). Both
audiences use the same typed validation, exact file revisions, bounded query
pages and explicit missing/duplicate outcomes. Declared importer names do not
advertise execution support. 11b adds [shared import/cache services](../asset-import-cache.md):
registered importer/schema discovery, pure preparation, revision-fenced publication,
fresh/stale/missing status and explicit reload/prune/clear/recovery. Both audiences
reuse these CPU services. 11c adds [shared bounded jobs](../asset-import-jobs.md):
scoped identities, progress/status, pending deduplication, cancellation and retained
terminal outcomes through original revision-fenced publication. Adapters keep
15/16; no GUI, LLM provider or protocol dependency enters these CPU services.
11d [integrated qualification](../asset-qualification.md) proves one saved reference
through moves, worker-time owner edits, stale rejection, cancellation, retry and
reopen, plus actual async process interruption. This qualifies shared backend
behavior; human/editor and terminal protocol parity remains with its later owners.
12a adds shared pure [glTF source inspection](../gltf-source.md) over explicit byte
snapshots, with owned diagnostics/references and no editor/device requirement.
12b adds pure [static geometry conversion](../gltf-geometry.md) through that
same source owner, with owned shared geometry, hierarchy and bounds. The eleventh
FeatureWorld case exercises it without changing the authored scene.
12c adds pure [image and texture conversion](../gltf-textures.md) through the
same captured-source owner, with owned RGBA8/16 values, semantic mip chains and
per-use samplers. The twelfth native FeatureWorld case and public CPU example
exercise this shared behavior without an editor/device requirement. Its
[handoff](../handoffs/12c-gltf-textures.md) records 507 fresh outcomes, 48 public-
example compiles, 36 actual sanitizer objects and protected packages. 12d adds
[material/asset APIs](../gltf-assets.md) and typed resource imports through the same
owner, ready for later human/agent adapters. Its [handoff](../handoffs/12d-material-assets.md)
records selected verification. Terminal/editor adapters retain 15/16 and the
original owner/publication fences.
[12e](../handoffs/12e-gltf-qualification.md) closes full REQ-12 through connected
owner/process checks and independent fixture validation. The shared CPU APIs
remain ready for those adapters. Assignment 13 follows the
[scene rendering checkpoints](scene-rendering-plan.md): Camera/MeshRenderer use
the same CPU schemas, checked queries, commands, history and source persistence
for both audiences. Device-dependent rendering follows in later checkpoints.

## Assessment of existing work

| Reviewed area | Finding and action |
| --- | --- |
| Original roadmap and ADR 0001 | Already require shared services, CLI/stdio discovery in 15, UI/API parity in 16/17 and a fresh-agent test in 33. Keep them; make independent terminal use and human handoff explicit acceptance requirements. |
| Core/Client/Tools/Server split | Keep. Server depends only on Core; authoring belongs in Tools. No runtime restructuring is needed now. Clarify that future CPU authoring must not inherit optional preview/GPU dependencies. |
| EngineTools and bootstrap executables | Tools currently provides build/version information; Editor/Runtime provide help/version, Server an idle lifecycle. No scene, history, asset or authoring service exists to retrofit. Do not present Server flags as an agent API. |
| CMake presets and developer tools | Keep noninteractive build/test commands, bounded subprocess cleanup and JSON/JUnit evidence in `tools/ci.py` / `tools/headless_ci.py`. These let a terminal agent develop engine code today; they do not yet let it build a game through public authoring commands. Serialize runs sharing a preset/output directory; ordinary toolchain reports identify HEAD but handoffs must also identify uncommitted changes. |
| Bootstrap text output and errors | Keep the established human help and exit behavior. Add the owned structured error type in 03 and public JSON/protocol modes in 14/15, with compatibility tests then; no speculative machine flags or early protocol stubs now. |
| Identity, persistence and command plans | Keep stable IDs, validated atomic saves and shared commands. Add revision-aware file writes, one project writer, explicit human/agent handoff and retry-after-lost-reply semantics before UI and CLI diverge. |
| Renderer/test separation | Keep actual hardware gates and deterministic CPU fixtures. Agent access needs capability discovery and artifact metadata, not a claim that all GPU/audio work can run on a device-free terminal host. |
| Coverage and repository guidance | Extend now with a recurring human/agent requirement and owned acceptance criteria. Keep all authoring entries unimplemented and all future gates NOT_RUN. Repository-specific skills are contributor guidance, not a required product integration. |
| `.gitignore` and local roadmap | Existing output/cache/secret rules are sufficient. Keep public schemas, examples, plans and fixtures visible; store generated agent/session reports under `out/`. Amend the ignored roadmap to point here; clean builds still do not need it. |

No C++ changes, dependency substitutions, CMake changes or immediate CLI stubs
are justified by this review. The current structure can carry the new contract;
the required changes are in future service/API behavior and its acceptance tests.
No existing game-authoring compatibility needs migration because it does not yet
exist. Existing bootstrap consumers and historical CI receipts remain unchanged.

## Workflow to deliver

M03a adds the same documented [CPU storage service](../durable-store.md) for human
and terminal contributors, with owned errors and noninteractive process receipts.
Storage lives in a server-only module; source authoring and player/admin protocols
remain separate. No SQL shell, LLM SDK, GUI or new remote adapter is required.
The [M03 checkpoints](durable-state-plan.md) preserve later authoring owners.

M03b's [durable character service](../durable-characters.md) follows the same
device-free C++ and noninteractive fixture path for humans and terminal agents.
Trusted server rules supply state; development authoring does not grant player
ownership. Actual authenticated process tests use fixture-only ID discovery,
not an advertised CLI or player roster protocol. Adapters remain 15/16.
M03b is PASS under the current-machine scope; its
[handoff](../handoffs/M03b-owned-characters.md) records 539 CPU CTests, protected
packages and public API qualification. M03c adds the same documented
[CPU transaction service](../durable-transactions.md), bounded audit values and
noninteractive process receipts for both audiences. Its
[handoff](../handoffs/M03c-atomic-transactions.md) records 553 passing CPU CTests
and protected package/API qualification. M03d adds the same documented
[CPU recovery service](../durable-recovery.md), typed tickets/outcomes and
noninteractive crash/outage receipts for both audiences. Its
[handoff](../handoffs/M03d-runtime-recovery.md) records 567 passing CPU CTests
and protected gates. M03e adds shared [CPU maintenance services](../durable-maintenance.md),
independent legacy/current fixtures and machine-readable interruption results.
Its [handoff](../handoffs/M03e-migration-restoration.md) records 581 CPU CTests
and protected gates. M03f connects these CPU services and structured receipts
through actual authenticated processes. Its [handoff](../handoffs/M03f-integrated-qualification.md)
records 588 CPU CTests, 10 standalone Tools checks and protected gates.
Full REQ-M03 is PASS. 09a implements the first
[shared authoring commands](../authoring-commands.md) on the existing AuthoredScene
owner: typed single edits, mandatory revisions, bounded results and read-only
Scene access. Human and agent-equivalent callers share this CPU C++ service.
09a is PASS under the current-machine scope. Its
[handoff](../handoffs/09a-shared-commands.md) and
[receipt](../coverage/evidence/09a-shared-commands.json) record 461 CPU CTests,
12 standalone Tools checks and protected package/API gates. Full REQ-09 remains
NOT_RUN. 09b now qualifies [atomic batches and pure preview](../authoring-transactions.md)
on the same owner. Its [handoff](../handoffs/09b-atomic-batches.md) and
[receipt](../coverage/evidence/09b-atomic-batches.json) record 605 full CPU CTests,
14 standalone Tools checks and protected package/API gates. Preview reserves no
revision; owner-thread commit revalidates bounded worker values against their
original observation. 09c now qualifies [shared undo/redo](../authoring-history.md)
with revision/ticket conflicts, common groups and safe source/handle restoration.
Its [handoff](../handoffs/09c-shared-history.md) and
[receipt](../coverage/evidence/09c-shared-history.json) record 610 full CPU CTests,
16 standalone Tools checks, 18 public-example compiles and protected gates.
**09d is PASS under the current-machine scope.** Follow the
[retry contract](../authoring-retries.md), [handoff](../handoffs/09d-retained-retries.md)
and [receipt](../coverage/evidence/09d-retained-retries.json): 486 full CPU CTest
executions, 20 additional standalone Tools checks, 21 public-example compiles,
actual sanitizer objects and protected packages pass. Core/Server implementation
is unchanged; their full Server suites remain historical until 09e.
**09e and full REQ-09 are PASS under the current-machine scope.** The
[qualification contract](../authoring-qualification.md),
[handoff](../handoffs/09e-integrated-qualification.md) and
[receipt](../coverage/evidence/09e-integrated-qualification.json) record 625 full
CPU CTest executions across seven profiles, 22 standalone Tools checks, 24 public-
example compiles, actual sanitizer objects and protected packages. Both full
Server suites are refreshed. **10a/10b now qualify the CPU runner and saved
FeatureWorld**, including the real native case catalog, current component/command
mappings and shared authoring/persistence exercises. The
[10b handoff](../handoffs/10b-featureworld.md) records 311 passing profile results
and API/sanitizer/package evidence. **Next: 10c, explicit GPU execution.**

M02a provides the [bounded network foundation](../network-transport.md) through
engine-owned CPU C++ interfaces and noninteractive JSON test fixtures. It does not
reuse source-authoring RPC as a player protocol. Both human contributors and
terminal agents can drive the same diagnostic tests without an editor, device,
provider account or model SDK. M02b adds the shared [session authority](../network-sessions.md)
and noninteractive real-process authentication fixtures; its completed
[qualification](../handoffs/M02b-owned-sessions.md) records 425 passing CPU CTests
and protected package checks.
M02c adds shared [replication APIs](../network-replication.md), a compiled public
C++ example and real-process JSON evidence with 446 passing CTests.
M02d adds spatial interest and measured connection budgets. M02e adds qualified
retained-storage observations and integrated process adversity through the same
CPU APIs and structured evidence; authoring adapters retain their existing
assignment owners.

A terminal agent discovers capabilities and schemas, opens or creates a project,
queries a bounded state snapshot, validates and commits revision-checked edits,
waits for import/build jobs, runs tests, reads diagnostics/artifacts and saves or
exports. It can edit project text/Lua through public scoped operations. It does
not need to click pixels, parse human log wording or keep an editor window open.
A human opens the same project in the editor, sees those changes and continues
with inspect/edit/undo/play workflows; the agent can then resume from a fresh
revision. Simultaneous editing uses one owner or returns a clear busy/conflict
result. Silent competing writable copies are unacceptable.

These are future workflows, not runnable command examples. Actual command names,
request/response examples and schemas ship alongside the implementing features.
The engine works without an LLM service; agents consume ordinary developer tools.
For 33/M09, use the currently available terminal agent in a fresh context with
only public documentation and tool access, then verify the human continuation on
the same PC. The authored project and local execution evidence cross that handoff;
no second computer or specific provider integration is part of the requirement.

## Ownership and gates

| Owner | Added work and acceptance |
| --- | --- |
| 03 | Owned structured errors and correlation/context usable by humans and tools; distinguish validation, capability, conflict and execution outcomes as services land. Preserve readable diagnostics. |
| 07 | Implemented fixed Name/Transform metadata exposes stable versioned IDs/types/defaults/ranges/units and scene-owner mutability. Bounded revisioned CPU inspection needs no ECS access or viewport. 07c and full REQ-07 pass current-machine qualification. |
| 08 | Documented readable source, project-root rules, atomic saves and conflict-aware text/file updates; external changes cannot be silently overwritten. |
| 09 | 09a qualifies one AuthoredScene command owner, typed single edits, expected revisions and bounded queries/results. 09b qualifies pure preview/atomic batches and 09c shared undo/redo. 09d qualifies scoped retained retries and explicit expiry/restart reconciliation. 09e connects these services through real files, publication faults, reload and reconciliation; full REQ-09 is PASS. Next: 10 FeatureWorld. |
| 10 | 10a/10b qualify deterministic CPU runs, nonzero assertion failure, bounded machine results/artifacts and the saved FeatureWorld/native catalog through shared owners. Current components/commands resolve to real cases; isolated system fixtures remain required. Separate real GPU runs and report unavailable capabilities honestly in 10c; close the integrated audit in 10d. |
| 11-14 | Imports/jobs expose progress, cancellation and atomic publication; export has structured completion/artifact metadata. Build CPU tools without editor/device initialization; qualify optional capture separately. |
| 15 | Deliver the terminal contract and public discovery, queries, edits, text files, jobs, diagnostics, run/capture/export adapters. Include no-TTY, second-writer, protocol-stream, conflict/retry and resource-limit fixtures. |
| 16/17 | Editor calls the same services; UI/API actions yield equivalent semantic saved state and history. Prove human-to-agent and agent-to-human handoff, including unsaved state and a stale revision. |
| 18-31, M05-M07 | Add terminal discovery and semantic authoring alongside each supported physics, script, prefab, play, HUD/audio, material/light, world/cell, animation and MMORPG-content feature. No later feature may become editor-only. |
| 30/31 | Reload/cook/export preserve conflicts and emit reproducible result/artifact metadata. Offline tooling remains separate from live MMO deployment and administration. |
| 32 | Audit schema/service/CLI/editor/serialization/test parity for all supported authoring features. Map intentional presentation-only exceptions. Missing paths block feature completeness. |
| 33 | The available terminal agent in a fresh context builds samples using only public docs/tools, then a human opens and continues them in the editor on this PC. Preserve invocation/result evidence, public API gaps and both outcomes. |
| M08/M09 | Keep local authoring separate from authorized operations. A fresh agent context authors the MMORPG reference world and a human continues it; real local clients, WSL server processes, shaped network faults and a real local store supply the scoped MMO proof. |
| 34/35 | Bound protocol/query/job workloads and audit packaged developer-tool docs, schemas and sample scripts. The local development release includes separate human and terminal getting-started paths on this PC; no model/provider/plugin or external deployment prerequisite. |

## Reviewable parts of assignment 15

The user-approved [five terminal checkpoints](terminal-authoring-plan.md) now
refine the broad phases below. [15a](../handoffs/15a-terminal-protocol.md) completes
discovery, retained ownership and read-only access; [15b](../handoffs/15b-terminal-editing.md)
adds retained edits/history, creation, persistence and scoped script text;
[15c](../handoffs/15c-terminal-imports.md) adds metadata/catalog/cache and bounded
import jobs through the retained owner. 15d is next. Full REQ-15
remains open through 15e's connected acceptance.

Keep 15 as one manifest owner and final gate, but implement it in these dependent
parts. Assignments 02/03/04 are complete
under the current-machine scope; the [window/input plan](window-input-plan.md)
records the completed 04c gate. 05a device ownership and 05b presentation
lifecycle are complete under the current-machine scope.
Existing machine-readable stress/failure/package tools are usable by a human or
terminal agent; they do not add authoring APIs. The user has authorized continued
development under the [nonblocking backlog decision](../backlog.md).

1. **Discovery and read-only access:** one-shot CLI + versioned stdio framing,
   clean stdout, capability/schema discovery, bounded project/scene queries,
   structured errors and examples. Test no TTY, malformed/versioned/large input,
   pagination consistency and unavailable device capabilities.
2. **Safe editing and jobs:** shared transactions, preview, revision conflicts,
   text writes, retry identity, project-owner exclusion/attachment policy,
   undo/redo and import/export jobs. Test two writers, external file changes,
   dropped replies, cancellation, EOF and crashes at publication boundaries.
3. **Complete terminal workflow:** a separate process creates/edits/saves/reopens,
   imports, runs deterministic assertions, retrieves artifacts and exports.
   Run CPU-only tools without GUI/device libraries; qualify actual GPU capture
   separately. Publish exact examples and capability/coverage manifests.

Assignment 16 then proves the editor consumes the same behavior, rather than
building a parallel service to match screenshots. Later feature increments
extend both interfaces and their tests, without waiting for a second API phase.

## Evidence and current limits

Current available terminal development entry points are documented in
[building](../building.md) and [CI](../ci.md). Completed local and hosted M01
qualification covers bootstrap/lifecycle/boundaries only. It does not prove project authoring, agent
discovery, human/agent parity, graphics or live MMO operations. The separate
[amendment handoff](../handoffs/human-agent-planning-amendment.md) records the
checks for this planning change. The full authoring gate remains NOT_RUN.

M01's hosted qualification is complete. Assignment 02's recorded local evidence
now closes its gate under ADR 0010; assignment 03 is complete across
[03a contracts, 03b lifecycle and 03c scheduling](core-plan.md).
[03c verification](../handoffs/03c-time-and-work.md) closes the local Core gate.
**04c and full assignment 04 are complete under the current-machine scope.**
**05b, swapchain, submission and presentation lifecycle, is complete under the
current-machine scope.** The
[presentation contract](../presentation-lifecycle.md) and
[05b handoff](../handoffs/05b-presentation-lifecycle.md) describe the owned lifecycle,
bounded native JSON fixture and separate qualifying GPU workload.
The [device guide](../graphics-device.md), [05 plan](graphics-device-plan.md) and
[05a handoff](../handoffs/05a-graphics-device.md) add noninteractive description,
inventory and bounded JSON device-lifetime fixtures usable by humans or agents.
Inventory/smoke explicitly require a device; pure capability checks, CPU tests
and future CPU authoring remain display-free.
**06a, CPU/offline shader tooling, is complete**, described by the
[06 checkpoint plan](shader-resource-plan.md),
[shader contract](../shader-pipeline.md) and
[06a handoff](../handoffs/06a-offline-shaders.md). Its noninteractive inputs and
machine-readable results add no scene-authoring API. The user's backlog decision
authorizes 06b and subsequent increments under their own acceptance criteria.
**Full REQ-06 is now complete**, including shared `--cube` launches, bounded
precompiled bundle preparation, machine-readable completion and compiler-free
Runtime distribution. The [06c handoff](../handoffs/06c-window-distribution.md)
records the local evidence. These diagnostic paths add no scene-authoring API;
assignment 07 follows the [scene checkpoints](scene-plan.md). 07a provides the
CPU identity/lifetime foundation. 07b is complete under the current-machine scope:
mandatory Name/Transform, validated hierarchy and mutation use that owner. Its
[handoff](../handoffs/07b-transform-hierarchy.md) and
[receipt](../coverage/evidence/07b-transform-hierarchy.json) record 362 passing CPU
CTests and protected dependency/package qualification. The
[transform contract](../transforms.md) specifies owned state
copies, checked handles, no-op revisions and failure rollback for the shared C++
foundation. 07c implements [schema discovery and bounded immutable revisioned
inspection](../scene-inspection.md), with its gate recorded in the
[07c handoff](../handoffs/07c-schema-inspection.md).
07c and full REQ-07 are PASS under the current-machine scope, with 376 passing
CPU CTests and protected dependency/package checks in the
[07c receipt](../coverage/evidence/07c-schema-inspection.json).
These are typed CPU C++ APIs; transactions, terminal commands and editor adapters
retain their owners in 09, 15 and 16. M02 is complete. Its
[transport foundation](../handoffs/M02a-transport-wire.md) passes 404 CPU CTests and
protected dependency/package qualification. M02b authentication and owned sessions
is also complete. M02c [authoritative replication](../network-replication.md) is
locally qualified through the same CPU C++ services and noninteractive fixtures;
M02d spatial interest, privacy and connection budgets is complete with
460 passing CPU CTests and protected package checks in its
[handoff](../handoffs/M02d-spatial-interest.md). M02e and full REQ-M02 are PASS
under the current-machine scope: the
[handoff](../handoffs/M02e-integrated-qualification.md) records 474 passing CPU
CTests and protected package qualification. Assignment 08 follows the
[persistence checkpoints](persistence-plan.md). 08a's
[source-document codec](../source-documents.md) provides shared CPU values and
structured errors, independently buildable without Editor/Client. Its
[handoff](../handoffs/08a-source-documents.md) records the completed CPU/sanitizer,
independent Tools and protected package gate. 08b's
[authored Scene owner](../authored-scenes.md) adds complete capture and revision-
checked replacement with unsaved-state protection; its
[handoff](../handoffs/08b-scene-activation.md) records the completed local CPU,
sanitizer and protected package gate.
08c's [project file service](../project-files.md) adds exact-content conflict checks,
native sibling publication and revision-safe save/reload through the same owner.
Its [handoff](../handoffs/08c-project-files.md) records completed current-machine
CPU/sanitizer, interruption, package and public API qualification.
08d and full REQ-08 are PASS under the current-machine scope: the
[handoff](../handoffs/08d-integrated-qualification.md) records 504 CPU CTests,
ten additional standalone Tools checks, protected packages and all three
public API examples. Integrated external-edit/reload fixtures preserve
owned snapshots from both authors before explicit reconciliation. M03 separately
owns live durability; M03 and all six checkpoints are complete. 09a extends that
same in-process authored owner with shared typed edits and bounded inspection.
The storage lock remains distinct from process attachment/ownership in 15.
Terminal/editor adapters retain owners 15/16; readable
source does not bypass that owner or create shared command history.
The [input guide](../input-contracts.md),
[window lifecycle guide](../window-lifecycle.md),
[frame-loop guide](../frame-loop.md), [04c handoff](../handoffs/04c-frame-loop.md) and
[window/input plan](window-input-plan.md) define injectable input and owned native
windows with offline frame scheduling. Explicit Editor/Runtime `--graphics` clear
presentation and `--window` diagnostics share that frame/input pump. Both are
accessible to a human or terminal agent, with bounded run options and diagnostic
JSON lines for F6/F7 sample pause/step controls. Graphical launches require an
explicit device capability; help and argument validation remain device-free.
Deterministic CPU tests inject input and time without an editor/display. These controls are neither
game-authoring commands nor a live-world protocol; scene rendering and user-facing
authoring UI/CLI adapters remain planned. EngineCore's scene/component C++ APIs
do not expose those interfaces. Full `REQ-04` is PASS after final local qualification. The
[final local handoff](../handoffs/02d-final-verification.md) preserves full local
Windows lifecycle runs, bounded portable terminal launchers and isolated Ubuntu
CPU/package checks. Former external GPU and independent clean-machine gates are
outside current scope, not passed. Existing hosted CI is supplemental and does
not block local increments. These remain developer fixtures, not public
game-authoring commands. The
[graphics plan](graphics-plan.md) and [probe commands](../graphics-probe.md)
provide noninteractive contributor diagnostics. The 09a command service is a CPU
C++ API; user-facing protocol/editor adapters remain future work.
No additional numbered chunk or change to product/player authority is necessary
for this amendment.
