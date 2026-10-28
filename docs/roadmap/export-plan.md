# Assignment 14: early standalone export checkpoints

Date: 2026-10-28. Base: `5a8ed5c`, clean `main`. Owner: REQ-14.
The user approved three subchunks following completed assignment 13. Follow the
[testing policy](../testing-policy.md), [MMORPG plan](mmorpg-plan.md),
[human/agent plan](human-agent-plan.md) and current-machine scope. Full REQ-14
remains NOT_RUN until 14c's connected package acceptance.

**14a is PASS under current-machine scope**, recorded in the
[content handoff](../handoffs/14a-client-export-content.md). 14b's implementation,
Debug/Release package checks and one package presentation are delivered. Its
Client/worker sanitizer selection remains NOT_RUN after compiler crashes;
[14b's handoff](../handoffs/14b-standalone-scene-export.md) records the failures
and the startup codec's narrower ASan pass. Close or explicitly amend that
remaining gate before 14c; no isolated package acceptance is claimed yet.

| Checkpoint | Deliverable and gate |
| --- | --- |
| 14a: Export content | CPU Tools service captures the explicit startup scene/camera and required client mesh assets into an owned candidate using existing prepared formats. Validate freshness, privacy, reference closure, attribution, bounded work and nonmutation; revalidate before later publication. No package or export CLI yet. |
| 14b: Export command and launch | Noninteractive structured export command, package-relative startup and atomic publication. Extend existing package tooling for Runtime, prepared content, scene shaders, libraries and notices; reject unsupported host/target combinations. CPU preparation/staging needs no graphics initialization. |
| 14c: Isolated acceptance | Launch the exported scene on both available Windows GPUs after relocation outside the checkout and from another working directory. Inspect actual binary/module/content closure, declared prerequisites and failure handling. No Editor, source/build paths, compilers or SDK dependency may be required at launch. |

14a follows the [content preparation contract](../client-export.md) and
[ADR 0037](../architecture/0037-client-export-content.md). Preserve Core's prepared
formats and Client's existing loading/rendering behavior. This is the current
static unlit content profile; final cooking belongs to 31, authoring protocol to
15 and dedicated server packages/private content roots to M08.

Keep 14a/14b checks focused on changed owners and affected consumers. Connected
deployment qualification occurs in 14c; do not repeat unrelated rendering,
network/storage or full-profile matrices. Reuse the existing Runtime packager,
protected builds and isolated launcher instead of creating another package system.
