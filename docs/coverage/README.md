# Requirement and API coverage

[requirements.json](requirements.json) is the versioned source for assignment
order, acceptance criteria, implementation mappings and PASS/FAIL/NOT_RUN status.
The ignored local roadmap is optional context, never a build or CI dependency.
The [testing policy](../testing-policy.md) controls verification selection and
evidence effort; historical full-profile counts are not recurring run lists.

## Current state

**18e and full REQ-18 PASS under current-machine scope.** The
[handoff](../handoffs/18e-editor-physics-preview.md#intel-physics-and-full-req-18-closure-2026-11-01)
records both-GPU physics acceptance and exact reused evidence.
Full REQ-16 and REQ-17 remain NOT_RUN for their separate native omissions.
Composition, imported-project handoffs and reference gestures have both-GPU
evidence. Selected hierarchy/inspector and asset/job/console branches also pass;
actual connected UI acceptance remains partial for the unperformed cases.
Desktop testing has resumed; the [backlog](../backlog.md) records remaining work.
The [shell contract](../editor-shell.md) and
[workspace contract](../editor-scene-workspace.md) cover shared project ownership,
dirty-state decisions, bounded hierarchy and revision-scoped selection, retained
entity edits, typed CPU viewport preparation and GUI scene composition. Their
feature mappings refer to the owning session/panel/adapter, CPU and native
fixtures; an unexecuted native fixture is not a GPU pass. Current assignment is
18. The [inspector contract](../editor-inspector.md) adds entity/revision-bound
drafts, atomic component Apply and ticket-bound history through the same owner.
The [asset/console contract](../editor-assets-console.md) covers explicit inventory,
metadata tokens, job publication/acknowledgement and bounded diagnostics. Actual
bidirectional UI/terminal acceptance follows the
[connected procedure](../editor-acceptance.md) and
[editor checkpoints](../roadmap/editor-plan.md).
16e adds no component, terminal RPC method or FeatureWorld case; its test-only
process adapter is not a second public authoring host.
17a extends Core/Tools extraction with an optional validated display camera
before culling and maps the [navigation contract](../viewport-navigation.md)
to CPU geometry and headless routing fixtures. No source/protocol version,
component, command or FeatureWorld case is added.
17b maps the [manipulation contract](../transform-manipulation.md) to focused
math and retained-owner drag fixtures, including terminal-equivalent resulting
edits. Existing `set_transform` remains the semantic command; previews publish
nothing and one changed commit adds one undo group. 17c's
[gizmo contract](../viewport-gizmos.md) maps actual headless ImGui controls and
pointer gestures, Core subtree extraction and retained Tools preview validation
to focused fixtures. No new semantic command or FeatureWorld case is introduced.
The [input contract](../input-contracts.md) maps owned first mouse-edge positions,
ambiguous-cycle rejection and focus/capture/pending-frame boundaries to the
existing `shark.client.input` and `shark.client.frame` fixtures. Fast gizmo,
picking and navigation consumers use `shark.editor.gui-adapter`; literal
pre-capture motion and complete press/release cases retain one shared mutation
and undo owner. Mixed-button regressions also require raw gizmo cancellation
without a source/history mutation. These CPU mappings do not qualify actual
native gestures; the handoff records the separately executed native selection.
17d extends the existing process handoff fixture with independent transform and
saved-source expectations. Both GPUs now have actual reference gestures and
terminal/native saved-source returns. Both GPUs also passed compact affine/
reflected/sheared picking, Camera/reset, committed culling, rejected shear and
angle wrap; the recreated-entity freshness case ran once in primary Windows
Debug. Remaining held-preview/cancel/freshness/intermediate-scale and right/middle
navigation are tracked under the [viewport procedure](../viewport-acceptance.md);
full REQ-17 is not closed.

18a's [physics owner](../physics-world.md) has bounded real empty updates and
native-registration/lifetime coverage. 18b's [body contract](../physics-bodies.md)
adds native shapes/lifetimes, one component and a shared command through source,
history, exact retries, prepared compatibility and real terminal pipes. The
eighteenth FeatureWorld case exercises independent source/native expectations.
18c's [simulation contract](../physics-simulation.md) extends this same case with
prepared runtime gravity and unchanged authored source/history. CPU fixtures cover
admission/leases, all three pose modes, existing schedules, independent fixed-rate
oracles and interpolated extraction/picking rejection. No new component, command,
protocol format or terminal RPC method is added in 18c. 18d's
[contact contract](../physics-contacts.md) extends shared settings and prepared
version 3, safe active removal and bounded post-tick enter/exit delivery. The same
FeatureWorld case verifies prepared filtering/triggers with unchanged authoring.
Native editor/connected acceptance remains 18e in the
[physics checkpoints](../roadmap/physics-plan.md).
Full REQ-18 is PASS: native physics control/lifetime/body-setting handoffs and
required readbacks on both GPUs complement retained Core, server-profile,
source/history/retry, compatibility and sanitizer evidence. Common native
validation controls use primary Windows Debug selection under the testing policy;
they are not reported as fresh per-field/per-GPU repeats. Earlier 16/17 UI
omissions remain independently open.

**15e and full REQ-15 are complete under current-machine scope.** The
[public example](../../examples/terminal_authoring.py) creates a fresh project,
imports and modifies licensed glTF content, authors a scene, saves/reopens and
runs static integrity assertions. Configured capture/export consume the current
retained owner. The [15e handoff](../handoffs/15e-terminal-acceptance.md) records
selected Windows/WSL/CPU sanitizer checks, actual validated captures on both GPUs
and isolated package results separately. The tools-only host also exports without
GUI/device libraries. CPU checks do not qualify device execution or deployment.
Existing focused rejection and
lifetime cases retain their evidence under the
[terminal checkpoints](../roadmap/terminal-authoring-plan.md). Full
[REQ-14](../handoffs/14c-isolated-export-acceptance.md),
[REQ-13](../handoffs/13e-runtime-scenes.md) and [REQ-12](../handoffs/12e-gltf-qualification.md)
remain complete under current-machine scope and approved amendments.

FeatureWorld has eighteen native cases, four saved entity IDs and 58 exercised
CPU surfaces, plus 44 external CPU commands (the exporter and 43 terminal methods,
including configured export) and three GPU commands, including optional terminal capture.
Real source/geometry/texture/material/
payload cases supplement owner/cache/job paths. The manifest and independent
executable inventories, not this prose, define the actual mappings. Lua stays
empty until bindings exist.

The current qualification profile is the Windows PC and retained WSL environment
under [ADR 0010](../architecture/0010-current-machine-scope.md). Historical unsupported
hardware rows remain NOT_RUN, not retroactive passes. The user's
[nonblocking backlog](../backlog.md) preserves the sequencing exception for remaining
05c work and full REQ-05's NOT_RUN status.

## Maintain the manifest

Schema 3 retains assignments 00-35 and inserts M01-M10 in the explicit
`assignment_order`. Preserve prerequisite ordering and every owner. MMO and
human/agent requirements have executable owners through the versioned
[MMORPG](../roadmap/mmorpg-plan.md) and [human/agent](../roadmap/human-agent-plan.md)
plans. A future assignment must remain NOT_RUN.

Each requirement records a stable ID, owner, acceptance criteria and
`planned_fixture`. Planned fixtures describe future evidence; their names do not
prove implementation or qualify a requirement. Verification needs a meaningful
reason and actual evidence scoped to the active qualification profile.

Each `implemented_surface` feature/component/command/Lua entry identifies:

- A unique stable ID/name and existing owning requirement IDs.
- Actual registered test IDs and test-source paths.
- Executable FeatureWorld cases, or justified external fixture paths/reasons.
- Supported behavior, including deliberate limits and unavailable capabilities.

Do not register placeholders or duplicate capabilities to increase counts. A
mapping is not a test pass. When a public surface grows, update its real behavior,
independent expected state, case discovery and manifest together. Preserve earlier
workloads, seeds, timeouts and failure cases.

The additive `featureworld` catalog records the saved project/scene, native case
source and case IDs. `featureworld_cases` references those IDs; an explicit
`external_fixture_reason` justifies isolated network, storage, platform or GPU
work. `shark.tools.featureworld-process` independently compares native discovery,
resolves mappings and executes saved cases. Scene assertions cannot replace those
isolated boundaries or establish GPU rendering correctness.

## Validation and evidence

Run relevant documentation checks from the repository root:

```text
python tools/validate_contract.py
```

Use `--self-test` when changing validator behavior. It rejects unsupported schemas,
duplicate/malformed IDs, missing owners/criteria, invalid ordering, unsupported
future PASS claims, bad mapping references and broken Markdown links. The
current-machine graphics PASS also retains both exact pinned 02d receipts and its
scope reference. This is structural validation, not engine/platform/GPU or
natural-language completeness qualification.

Record selected runs with commands, profiles, results and logs under the testing
policy. Distinguish full affected-subsystem inventories, selections, repeats and
unchanged historical evidence; profile totals are not unique test counts. Preserve
failed attempts and their resolution. Update required machine-readable records
when consumed by tools, but do not add another receipt/freeze framework for ordinary
edits. Keep existing receipt bytes and pins unchanged.

## Completed-owner records

These links retain detailed behavior, limits, commands and historical evidence
without copying checkpoint chronologies into every guidance document. The manifest
remains authoritative for individual requirement status.

| Owner | Contract / final handoff |
| --- | --- |
| 00 architecture | [Decisions](../architecture/README.md), [handoff](../handoffs/00-architecture-contract.md) |
| 01 build foundation | [CI](../ci.md), [hosted gate](../handoffs/01-ci-verification.md) |
| M01 headless split | [Target boundaries](../architecture/0001-targets-and-boundaries.md), [hosted gate](../handoffs/M01-ci-verification.md) |
| 02 graphics feasibility | [Probe](../graphics-probe.md), [scope handoff](../handoffs/current-machine-scope-amendment.md) |
| 03 Core | [Core](../core-contracts.md), [time/work handoff](../handoffs/03c-time-and-work.md) |
| 04 input/frame loop | [Frame loop](../frame-loop.md), [handoff](../handoffs/04c-frame-loop.md) |
| 05 device/presentation | [Presentation](../presentation-lifecycle.md), [05b handoff](../handoffs/05b-presentation-lifecycle.md), [remaining backlog](../backlog.md) |
| 06 shaders/resources | [GPU resources](../gpu-resources.md), [handoff](../handoffs/06c-window-distribution.md) |
| 07 scenes | [Inspection](../scene-inspection.md), [handoff](../handoffs/07c-schema-inspection.md) |
| M02 networking | [Qualification](../network-qualification.md), [handoff](../handoffs/M02e-integrated-qualification.md) |
| 08 source persistence | [Source/file contract](../project-files.md), [handoff](../handoffs/08d-integrated-qualification.md) |
| M03 durable state | [Qualification](../durable-qualification.md), [handoff](../handoffs/M03f-integrated-qualification.md) |
| 09 authoring | [Qualification](../authoring-qualification.md), [handoff](../handoffs/09e-integrated-qualification.md) |
| 10 FeatureWorld | [Qualification](../featureworld-qualification.md), [handoff](../handoffs/10d-integrated-qualification.md) |
| 11 assets | [Qualification](../asset-qualification.md), [handoff](../handoffs/11d-asset-qualification.md) |
| 12 static glTF | [Qualification](../gltf-qualification.md), [handoff](../handoffs/12e-gltf-qualification.md) |
| 13a components (partial REQ-13) | [Component contract](../render-components.md), [handoff](../handoffs/13a-render-components.md) |
| 13b prepared content (partial REQ-13) | [Prepared content](../prepared-content.md), [handoff](../handoffs/13b-prepared-content.md) |
| 13c scene extraction (partial REQ-13) | [Extraction](../scene-rendering.md), [handoff](../handoffs/13c-scene-extraction.md) |
| 13d offscreen rendering (partial REQ-13) | [Viewport](../scene-viewport.md), [handoff](../handoffs/13d-offscreen-scene-viewport.md) |
| 13 scene rendering | [Runtime scenes](../runtime-scenes.md), [final handoff](../handoffs/13e-runtime-scenes.md) |
| 14a export content (partial REQ-14) | [Client export](../client-export.md), [handoff](../handoffs/14a-client-export-content.md) |
| 14b/14c standalone export (full REQ-14) | [Standalone export](../standalone-export.md), [isolated acceptance handoff](../handoffs/14c-isolated-export-acceptance.md) |
| 15a terminal foundation (partial REQ-15) | [Protocol](../authoring-protocol.md), [handoff](../handoffs/15a-terminal-protocol.md) |
| 15b terminal editing (partial REQ-15) | [Protocol](../authoring-protocol.md), [handoff](../handoffs/15b-terminal-editing.md) |
| 15c terminal imports/jobs (partial REQ-15) | [Protocol](../authoring-protocol.md), [handoff](../handoffs/15c-terminal-imports.md) |
| 15d terminal run/export/capture (partial REQ-15) | [Protocol](../authoring-protocol.md), [handoff](../handoffs/15d-terminal-execution.md) |
| 15e connected terminal workflow (full REQ-15) | [Public example](../../examples/terminal_authoring.py), [handoff](../handoffs/15e-terminal-acceptance.md) |
| 16a editor shell (partial REQ-16, remaining desktop acceptance deferred) | [Shell contract](../editor-shell.md), [handoff](../handoffs/16a-editor-shell.md) |
| 16b scene workspace (partial REQ-16, desktop acceptance deferred) | [Workspace contract](../editor-scene-workspace.md), [handoff](../handoffs/16b-editor-scene-workspace.md) |
