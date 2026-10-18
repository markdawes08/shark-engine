# Human and terminal-agent development plan and assessment

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
| 07 | Schema metadata exposes stable IDs/types/defaults/ranges/units and mutability. Semantic inspection must not require ECS access or a viewport. |
| 08 | Documented readable source, project-root rules, atomic saves and conflict-aware text/file updates; external changes cannot be silently overwritten. |
| 09 | One authoritative service, expected revisions, pure validation/preview, atomic batches, shared undo/redo and precise retry identity/scope. Test stale snapshots, failed batches and lost-reply outcomes. |
| 10 | Deterministic CPU runs with nonzero assertion failure and bounded machine results/artifacts. Separate real GPU runs and report unavailable capabilities honestly. |
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

Keep 15 as one manifest owner and final gate, but implement it in these dependent
parts. This plan does not start assignment 15; assignment 02 closed under the
explicit current-machine scope amendment and assignment 03 follows its
[Core checkpoint plan](core-plan.md).

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
now closes its gate under ADR 0010; assignment 03 is current, split into
[03a contracts, 03b lifecycle and 03c scheduling](core-plan.md). 03a and 03b are
complete; [03b verification](../handoffs/03b-application-lifecycle.md) covers
application ownership and rollback. 03c scheduling and the full gate are next. The
[final local handoff](../handoffs/02d-final-verification.md) preserves full local
Windows lifecycle runs, bounded portable terminal launchers and isolated Ubuntu
CPU/package checks. Former external GPU and independent clean-machine gates are
outside current scope, not passed. Existing hosted CI is supplemental and does
not block local increments. These remain developer fixtures, not public
game-authoring commands. The
[graphics plan](graphics-plan.md) and [probe commands](../graphics-probe.md)
provide noninteractive contributor diagnostics; game-authoring APIs remain future work.
No additional numbered chunk or change to product/player authority is necessary
for this amendment.
