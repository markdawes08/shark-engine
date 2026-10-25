# ADR 0020: Shared authored-scene commands

Status: accepted for 09a; later transaction/history/retry checkpoints remain
planned. Date: 2026-10-25. Base: `0b2726c`.

## Decision

Extend EngineTools' existing AuthoredScene as the authoritative local editing
service. It owns one Core Scene, source metadata and the saved/activated baseline.
Five typed command values create an entity, delete a subtree, set Name, set
Transform or reparent. Targets and parents use AuthoredEntityId; every command
requires the observed SceneRevision, including its process-local incarnation.
Core keeps validation, hierarchy rules and safe immediate mutation. No second
authoritative document model or generic command/job framework is introduced.

Commands mutate the existing Scene in place. Reusing `activate` for normal edits
would invalidate all handles and acknowledge a clean baseline, so it retains its
explicit reload/replacement meaning. Successful edits preserve unaffected handles,
source extras and the baseline; actual Core changes advance the revision once.
No-ops do not advance it. The required revision is checked before target lookup
or mutation, including for a no-op. Stable IDs never bypass that fence.

`execute` returns an owned reply with a current observed revision and a nested
Result for the command outcome. Inability to access the owner, or an allocation/
unexpected failure preventing the reply, uses the outer Result. Expected command
rejection returns a structured error and unchanged revision. The affected-ID
refresh set is prepared before mutation; successful mutation is followed only
by nonallocating observations and moves. A completed edit cannot become an
apparent rollback because allocating its receipt failed afterwards.

## Ownership and compatibility

The public mutable `AuthoredScene::scene()` overload is removed. The const
overload remains available for checked reads. Queries reuse Core's bounded
immutable pages, cursor revision checks and fixed component schemas. Private
invariant fixtures retain explicit owner-checked access to inject pending work,
visits and closed-state failures; no public authoring adapter uses that access.

This is an intentional C++ source compatibility break: callers replace mutable
Scene setters with `execute(command, expected_revision)` and check both Result
levels. All repository consumers and the published example migrate together.
Read-only callers continue to compile. Source JSON and component versions remain
1; gameplay/database/authoring-wire versions do not change. No binary ABI or
unimplemented terminal protocol compatibility is promised.

Source creation, validated replacement and explicit file saves remain the
existing persistence lifecycle operations. History integration must account for
their clean baseline, replacement incarnation and external-file conflicts in
09b/09c; they are not ordinary command shortcuts. Runtime gameplay keeps Core
operations without authoring history. Local authoring does not grant player or
server administration authority.

## Increment boundaries

The [five-checkpoint plan](../roadmap/authoring-plan.md) assigns atomic batches
and pure preview to 09b, bounded shared history to 09c, and retained retry outcomes
to 09d. 09a has synchronous single-edit semantics, with no retained request ID or
lost-reply lookup. Repeated calls undergo fresh revision checks and must not be
advertised as deduplicated mutations. Terminal adapters and process ownership
remain 15, editor integration 16, and FeatureWorld command cases 10.

The [command contract](../authoring-commands.md) records the implemented API,
outcomes and limits. 09a requires behavior/failure coverage, migrated persistence
regressions, public-header examples, CPU/sanitizer checks and protected dependency
boundaries. Full REQ-09 remains NOT_RUN until 09e integrated qualification.
