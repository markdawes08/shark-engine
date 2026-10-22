# 08: Project and scene persistence checkpoints

Date: 2026-10-21. Base: `8463a84` on initially clean `main`.
Owner: REQ-08. M02 is complete; assignment 08 is split into the four checkpoints
below. **08a, 08b and 08c are PASS; 08d is next. Full REQ-08 remains NOT_RUN.** The
[08a handoff](../handoffs/08a-source-documents.md) records 484 passing CPU CTests,
standalone Tools tests and protected package checks. The
[08b handoff](../handoffs/08b-scene-activation.md) and
[receipt](../coverage/evidence/08b-scene-activation.json) record 489 passing
full-suite CPU CTests, two additional Tools CTests on each OS and protected
package/public API checks against 291 frozen inputs. The
[format contract](../source-documents.md) and
[capture/activation contract](../authored-scenes.md) define the implemented surface.
Follow [ADR 0004](../architecture/0004-formats-and-identity.md),
[human/agent authoring](../architecture/0008-human-agent-authoring.md) and the
[MMORPG execution order](mmorpg-plan.md).

## 08a - Formats, validation and migration

Implement device-free EngineTools value documents and bounded, versioned JSON
decoding/encoding. Reuse Core identity, project-path and component validators.
Define project references, authored hierarchy, optional stable world/cell anchor,
bounded opaque extras and explicit component versions. Add real legacy version-0
fixtures and in-memory 0-to-1 migrations. Source readers and the pinned JSON
dependency remain private to EngineTools; neither enters Core/Runtime/Server.
Make EngineTools independently buildable without Editor, Client or graphics.

Gate: independent readable fixtures round-trip IDs/local data, migrated output
uses version 1, and malformed/oversized/duplicate/unknown/future input rejects.
Validate complete value candidates, missing references, parent cycles/depth and
bounded extras. Prove shared component validation, deterministic output and
unchanged input on failure. Qualify Windows/WSL CPU/sanitizer behavior, isolated
tools construction and protected dependency/package boundaries. No active Scene
or file is replaced in this checkpoint.

## 08b - Scene integration and transactional activation

The [capture/activation contract](../authored-scenes.md) defines the EngineTools
surface; the [08b handoff](../handoffs/08b-scene-activation.md) records its gate.

Bridge documents to the existing Scene owner. Capture complete revision-consistent
authored state, resolve project/scene references and build an inactive candidate
with preserved IDs and hierarchy. Validate composed transforms and all supported
references before publishing the candidate. Keep process-local handles and live
authority/session state out of source data.

Gate: representative hierarchy and world/cell identity survive capture/load;
failed load leaves the active scene and its unsaved state intact. Successful
replacement has explicit handle invalidation and ownership behavior. Loading a
source document never grants server authority or creates a second live registry.

## 08c - Safe project files and conflicts

The [project file contract](../project-files.md) defines the typed C++ service,
exact-content revisions, native path restrictions, writer coordination and
separate publication/durability results. The [08c handoff](../handoffs/08c-project-files.md)
and [receipt](../coverage/evidence/08c-project-files.json) record 499 passing full-suite
CPU CTests, four additional standalone Tools tests on each OS, protected packages
and public API audits against 298 unchanged frozen inputs.

Implement project-root file services over validated documents. Resolve real
filesystem containment and path collisions, stage sibling writes, flush and
replace atomically with explicit durability/error semantics. Check expected
on-disk revisions/digests so human and agent external edits cannot be overwritten
silently. File results identify the committed outcome and useful retry/reload
information without borrowing later command history or CLI protocols.

Gate: traversal and symlink/reparse escapes reject; stale writes, failed reads,
failed writes/replace and interrupted saves preserve the declared prior state.
External edit and subsequent reload scenarios preserve both authors' work. State
the concurrency assumptions and boundaries of each operating-system guarantee.

## 08d - Integrated qualification

Integrate round-trip, migration, activation, external-edit and interrupted-save
fixtures. Freeze implemented inputs and qualify Windows/WSL CPU/sanitizer,
protected Runtime/Server, public API examples and local packages. Retain exact
commands, failures, limits and coverage in the final handoff and receipt; close
REQ-08 only when all required gates pass. Tests accompany each earlier checkpoint.

Authored documents define world content. Live character/economy durability stays
in **M03**, which follows full 08. Shared commands/history retain 09, FeatureWorld
10, terminal adapters 15 and editor UI 16. This plan introduces none of them.
