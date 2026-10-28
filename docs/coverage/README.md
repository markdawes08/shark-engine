# Requirement and API coverage

[requirements.json](requirements.json) is the versioned source for assignment
order, acceptance criteria, implementation mappings and PASS/FAIL/NOT_RUN status.
The ignored local roadmap is optional context, never a build or CI dependency.
The [testing policy](../testing-policy.md) controls verification selection and
evidence effort; historical full-profile counts are not recurring run lists.

## Current state

**Full REQ-12 is complete under current-machine scope.** The latest
[12e handoff](../handoffs/12e-gltf-qualification.md) and
[results](evidence/12e-gltf-qualification.json) record integrated static glTF
qualification. Next is assignment 13, basic scene rendering and cameras.
Terminal/editor adapters remain 15/16; no playable authoring editor or scene
renderer is claimed by the CPU import work.

FeatureWorld has thirteen native cases, four saved entity IDs and 47 CPU surfaces
plus one separately qualified GPU command. Real source/geometry/texture/material/
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
