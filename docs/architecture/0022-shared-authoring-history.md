# ADR 0022: Bounded shared authoring history

Date: 2026-10-25. Status: accepted for checkpoint 09c.

## Context

Human and terminal-agent development must share one project scene and undo
history. 09a typed edits and 09b atomic batches already share AuthoredScene with
required revision fences, private Core candidates and transactional publication.
Undo must preserve source metadata and surviving handles, and must not overwrite
intervening work or revive stale entity handles. Gameplay Core mutations must
remain independent of local authoring history.

## Decision

Add bounded in-memory ordered history to AuthoredScene. A changed single command
or atomic batch is one group; noops, preview and failures create none. Retain
immutable before/after source snapshots with replacement-ID information, sharing
adjacent snapshot ownership. Revision-bound snapshots are derived values, never
a second authoritative mutable scene. Keep the existing single-delete mutation
path with a preallocated subtree-removal history projection so large sparse-query
fixtures retain their established workload and timeout.

Undo/redo require the expected live revision and next owner-local monotonic ticket.
Prepare a private candidate through existing validated Core mutation, recreate
removed or replaced identities with fresh generations, preserve surviving handles
and restore exact source metadata/order. Allocate before publication; force exactly
one revision when moving history, including source-equivalent groups. Publish
source metadata and stack cursor with nonallocating moves on the owner thread.
Use explicit conflict results for stale requests and out-of-band writes; explicit
revision-checked clear reconciles history without changing scene values.

Default to 32 entries and 32 MiB conservative snapshot charge with hard maxima
32 and 64 MiB. Drop redo and evict oldest only after successful changed publication;
reject a group that cannot fit alone. Save retains history and its existing exact
revision baseline; history movement stays conservatively dirty. Successful
activation clears history. History is not durable and tickets never authorize
retry replay. 09d owns lost-reply/idempotency behavior, 09e the integrated gate.

## Consequences

The [history contract](../authoring-history.md) defines limits, charging,
restoration, source compatibility and error/results. This extends the existing
CPU Tools service without new dependencies, formats, mutable public ports,
player protocol, editor/terminal stubs or durable transaction coupling. Core's
internal edit port adds an immediate guard and forced-revision publication for
Tools history; public safe mutation and default batch publication keep their
existing behavior. Refresh Core/Server regressions and protected packages along
with Tools tests. Real adapters remain 15/16 and FeatureWorld coverage joins in10.
