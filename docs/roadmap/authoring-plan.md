# 09: Shared authoring checkpoints

Date: 2026-10-25. Base: `0b2726c` on initially clean `main`.
Owner: REQ-09. M03 is complete. The user approved the following five checkpoints;
**09a-09e and full REQ-09 are complete under the current-machine scope**.
The [09a handoff](../handoffs/09a-shared-commands.md) and
[receipt](../coverage/evidence/09a-shared-commands.json) record 461 passing full
CPU CTests plus 12 standalone Tools checks and protected package qualification.
**09b - Atomic transactions and preview is complete under the current-machine scope.**
Its [handoff](../handoffs/09b-atomic-batches.md) and
[receipt](../coverage/evidence/09b-atomic-batches.json) record 605 full CPU CTests,
14 standalone Tools checks, public API/sanitizer audits and protected packages.
**09c - Shared undo and redo is complete under the current-machine scope.**
Its [handoff](../handoffs/09c-shared-history.md) and
[receipt](../coverage/evidence/09c-shared-history.json) record 610 full CPU CTests,
16 standalone Tools checks, 18 public-example compiles, sanitizer audits and
protected packages. Follow the [history contract](../authoring-history.md).
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
Server suites are refreshed. **Next: 10, FeatureWorld and automated scene runner.**

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

09b adds the internal Core candidate/publication port and two CTest families:
`shark.core.scene-edit` (30 seconds) and `shark.tools.authoring-batches`
(60 seconds), both seed1337. Its qualification refreshes seven full CPU profiles,
including both WSL Server profiles, all seven authoring families in standalone
Tools on each OS, five public examples, actual sanitizer objects and protected
builds/packages. Preserve the 09a inventories plus only these additive families.
The [transaction contract](../authoring-transactions.md) defines ordered limits,
identity, no-op and publication semantics; full REQ-09 remains NOT_RUN.

09c adds `shark.tools.authoring-history` (60 seconds, seed 1337), preserving all
previous inventories and workloads. Its seven full profiles total 610 CPU CTests;
eight authoring families run in standalone Tools on each OS and six public
examples compile on MSVC/GCC/Clang. Core changes refresh both Server profiles.
Undo/redo preserve surviving handles and restore removed/replaced IDs with fresh
generations; save retains history, successful activation clears it. Retained
mutation identity, payload matching and lost-reply handling remain 09d.

09d adds `shark.tools.authoring-retries` (60 seconds) and
`shark.tools.authoring-retries-process` (90 seconds), both seed 1337. Preserve all
previous inventories and the 512-allocation sweep ceilings. Its five full CPU
profiles total 486 CTest executions, with 10 standalone authoring families per OS
and seven public examples on MSVC/GCC/Clang. Core/Server implementation is unchanged;
09e refreshes both full Server profiles. Owner UUID scope plus consecutive
sequences and a nonrewinding watermark prevent expired identities from executing.
Save/activation retain the ledger; new owners require reconciliation of old IDs.

09e completes the five checkpoints through eight connected source/file/authoring
cases in `shark.tools.authoring-integration` (90 seconds, seed 1337). Its seven
profiles total 625 full CTest executions, including both Server profiles. Eleven
standalone Tools families per OS, eight public examples per compiler and actual
sanitizer/package audits pass. Preserve this lineage and every earlier case/limit.
The [qualification contract](../authoring-qualification.md) records the exact
connected behavior. Full REQ-09 is PASS; assignment 10 follows.
