# 09: Shared authoring checkpoints

Date: 2026-10-25. Base: `0b2726c` on initially clean `main`.
Owner: REQ-09. M03 is complete. The user approved the following five checkpoints;
**09a is complete and full REQ-09 remains NOT_RUN**.
The [09a handoff](../handoffs/09a-shared-commands.md) and
[receipt](../coverage/evidence/09a-shared-commands.json) record 461 passing full
CPU CTests plus 12 standalone Tools checks and protected package qualification.
**09b - Atomic transactions and preview is next.**

Follow [ADR 0008](../architecture/0008-human-agent-authoring.md),
the [human/agent plan](human-agent-plan.md) and
the [MMORPG execution order](mmorpg-plan.md). These are local development
services in EngineTools. Core retains shared validation and safe mutation;
player intents, live storage and operational administration remain separate.

## 09a - Shared command foundation

Extend the existing AuthoredScene owner with real typed single-entity create,
subtree delete, reparent, Name and Transform edits. Target stable authored IDs
under a required scene-incarnation/revision fence. Reuse Core validation and
immediate safe mutation, preserving handles, source metadata and the unsaved
baseline. Public inspection cannot expose a mutable Scene. Return owned bounded
outcomes, current revisions and deterministic affected IDs for refresh.
Reuse existing bounded queries and component schema discovery.

Gate: valid edits, no-ops and rejected stale/missing/invalid requests have exact
state and revision outcomes; failed commands preserve the complete scene. Query
pages and cursors obey existing bounds and revision rules. Alternating human-
and agent-equivalent callers use the same owner. Existing capture/save/reload,
metadata, lifecycle and external-file conflict tests remain covered through the
new editing path. Publish the C++ API and its source-compatibility migration.

No batch, preview, history, deduplication, job or terminal stub is exposed in
09a. Its command values and revision/result rules form the basis of later steps.
An ordinary repeated call is a new command checked against its supplied revision;
there is no retained request outcome or exactly-once claim.

## 09b - Atomic transactions and preview

Validate complete bounded batches against a private candidate before publishing
through the same owner. Pure preview cannot change state or reserve a revision;
commit rechecks it. Define batch identity, changed-ID results, no-op behavior,
metadata/handle lifetime and allocation/failure boundaries explicitly.

Gate: invalid or stale batches leave state, metadata, revisions and dirty status
unchanged; valid batches become visible together. A queued Core mutation list is
not a transaction. Define how asynchronous candidates reach this owner-thread
commit boundary; actual import/export jobs retain assignments 11-14 and use the
existing bounded work primitives, without introducing a generic job framework.

## 09c - Shared undo and redo

Add bounded ordered transaction history, grouping, redo invalidation and explicit
conflict outcomes. Restore entity identity, hierarchy, values and source metadata
without resurrecting an unrelated later incarnation or overwriting intervening
work. State whether save/reload resets history; durable undo is not required.

Gate: replay, undo and redo produce equivalent authored state. Alternating callers
share history; stale history requests reject. Gameplay continues to use Core safe
mutation without adding every simulation tick to authoring history.

## 09d - Safe retries and lost replies

Define mutation identity separately from request correlation. Add bounded retained
outcomes, payload matching, duplicate handling and explicit retention/expiry.
Within scope, identical retries return their original result without reapplying;
changed payloads reject. Restart or expired evidence requires reconciliation or
an unknown outcome, never blind repetition. Apply these rules to history changes
as well as ordinary edits and atomic batches.

Gate: lost replies, duplicate IDs, mismatched payloads, retention pressure,
restart/expiry and subsequent unrelated edits preserve the declared state.

## 09e - Integrated qualification

Connect edits, preview/batches, undo/redo, retries and save/reload/external changes
through the same owner. Freeze final inputs and qualify current-PC Windows/WSL
CPU/sanitizer behavior, standalone Tools, public examples and protected packages.
Record exact test inventories, failures, limits and evidence. Only this gate
closes full REQ-09; assignment 10, FeatureWorld and the scene runner, follows.

## Checkpoint qualification and later adapters

Every checkpoint includes its behavior/failure tests and relevant regressions.
09a changes Tools and its consumers, leaving Core/Client/Server implementation
unchanged. Qualify full Windows Debug/Release/ASan and WSL CPU Debug/ASan-UBSan
suites, all six authoring families in standalone Tools on each OS, and protected
Runtime/Server builds and local packages. Compile the four public persistence/
command examples without private dependency headers and verify real sanitizer
instrumentation for changed authoring sources. Preserve earlier test inventories;
report focused checks separately from full suites. Existing Server-only full
suite evidence remains historical; refresh it in 09e or if Core/Server changes.

All checks use this PC and existing WSL; no GPU acceptance is introduced here.
CLI/process ownership and editor integration remain 15 and 16. FeatureWorld
attaches the supported commands in 10; earlier CPU fixtures are explicit external
coverage until then. Full local authoring parity is not established by these
C++ services alone.
