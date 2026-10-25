# Atomic authoring batches and preview

09b extends the same [AuthoredScene owner](authored-scenes.md) and
[five typed commands](authoring-commands.md). See
[ADR 0021](architecture/0021-atomic-authoring-batches.md) and the
[checkpoint plan](roadmap/authoring-plan.md). This is CPU C++ EngineTools behavior.

## Input, identity and limits

`AuthoringBatch` contains a caller-supplied `AuthoringBatchId` and an owned vector
of the existing `AuthoringCommand` values. The ID is a nonnil typed UUIDv4,
separate from authored entity IDs. It correlates this batch and its diagnostics;
it does not retain an outcome or deduplicate a retry. The same ID with a fresh
revision is a new invocation, even with a different payload. 09d owns retained
mutation identities, payload matching, expiry and lost replies.

`authoring_maximum_batch_commands` is 256. Empty batches are allowed. The bound
is checked before copying or evaluating any command. Existing Name UTF-8/256-byte,
Transform, hierarchy and configured Scene bounds apply to every intermediate
candidate state. Commands execute in order: a later command can use an earlier
creation, but a temporary invalid cycle, missing parent or capacity overflow
rejects immediately even if a later command could repair it. This is not an
unordered declarative final-state solver. Deletion removes the whole subtree.

## Preview and commit

Both `preview(batch, expected_revision) const` and
`execute(batch, expected_revision)` return `Result<AuthoringBatchReply>`.
Check both the outer Result and `reply.outcome`, as for single commands.
Wrong-thread/closed-owner access uses the outer error. Expected validation,
staleness and mutation-phase rejection uses the nested error. Every reply
contains the input ID and observed live Scene revision. Errors identify
`authoring.batch.preview` or `authoring.batch.execute`, put the batch ID in
`context.request_id`, and identify a failing command as `/commands/N` with its
entity context. Whole-batch admission errors have no command index.

Preview prepares a private candidate, runs all commands through the same Core
validators and discards it. It leaves state, source metadata, slot generations,
saved baseline, revision, cursors and the next live Scene incarnation untouched.
It does not reserve a revision, publish handles or authorize a later commit.
Commit rebuilds and revalidates from the supplied revision, then rechecks that
revision and the owner mutation boundary immediately before publication.

Successful changed batches publish together and increment the live revision
exactly once, retaining the Scene object and instance. Existing handles of
surviving entities stay valid. Deleted handles become stale; recreated IDs get
new slot generations and cannot inherit the deleted incarnation's source extras.
Source project/scene/world metadata and the saved baseline survive the commit.
The owner becomes dirty until an explicit successful save acknowledges it.
Reload/activation remains explicit replacement with its existing fresh instance.

`outcome.value()` is an `AuthoringCommandEffect`. `changed` means at least one
ordered operation actually changed its candidate. Empty batches and batches
containing only Core no-ops return false, an empty refresh set and the same live
revision. An edit followed by its inverse is still a changed batch, even when
captured component values equal the original values: intermediate identity and
revision transitions are real, and the saved baseline is revision-based.

`affected_entities` is the sorted unique union of each changed command's refresh
set. It includes deleted IDs, transformed descendants and affected old/new
parents. It is not a final live-entity list or a component-by-component diff.
Its bound is `configured max_entities + batch.commands.size()`, at most 4352.
Candidate per-operation revisions and handles never appear in the public reply.
Preview reports its unchanged live revision; successful commit reports the new
live revision. Query cursors survive preview/no-op/rejection and reject after a
changed commit. Old owned query pages remain owned snapshots.

## Publication and failure boundary

Core owns an internal staging/publication port, private to Core and Tools.
Preparation copies live records, cached transforms, identity maps and exact slot
generations into one nonauthoritative candidate. No native EnTT value is public.
The five existing mutation implementations validate candidate operations;
the deferred FIFO remains per-operation and is never used as a transaction.

All candidate copying, mutation work, ID-set construction and result allocation
finish before publication. Failure destroys the candidate and leaves the live
owner unchanged. Publication rechecks the base and swaps private state without
allocating, preserving the live object/instance and advancing the counter once.
Only nonallocating value moves and candidate cleanup follow success. No user
callback runs inside the batch. Internal candidate handles/cursors must never
escape through an adapter; the internal port is not a supported public API.
Memory is bounded by configured scene limits, one candidate, and the fixed batch
bound. Allocation failure is reported through the existing owned error boundary;
as elsewhere, constructing diagnostics still needs available memory.

## Worker completion and later owners

Workers may build bounded owned command values from immutable inputs and return
them with the originally observed SceneRevision using the existing completion
primitive. The creating owner thread calls `preview` or `execute` and revalidates
that revision. A stale completion rejects; it must be queried/reconciled before
an explicit fresh submission. A successful preview is not a commit ticket.
Workers never clone/access/publish a live registry. No scheduler, async job API,
importer, history or terminal/editor adapter is added here. Actual import/export
jobs remain 11-14; undo/redo is 09c and retained retries are 09d.

## Complete public example

This helper validates a batch and then commits at the same observed revision.
Another caller's intervening edit still makes commit reject. The preview's two
result levels are checked; the full commit reply is returned for the caller to
check. It requires only public headers and no display or private dependency.

```cpp
#include <shark/tools/authored_scene.hpp>

shark::Result<shark::tools::AuthoringBatchReply>
preview_then_commit(shark::tools::AuthoredScene& owner,
                    const shark::tools::AuthoringBatch& batch,
                    shark::SceneRevision observed) {
    auto preview = owner.preview(batch, observed);
    if (!preview) {
        return preview.error();
    }
    if (!preview.value().outcome) {
        return preview.value().outcome.error();
    }
    return owner.execute(batch, observed);
}
```

`shark.tools.authoring-batches` uses seed 1337 and a 60-second timeout;
`shark.core.scene-edit` uses seed 1337 and 30 seconds. They qualify rollback,
ordered validation, no-ops, revision/ownership boundaries, metadata/handle lifetime,
preview purity, worker-value reconciliation and real file-save integration.
On Linux, a test-executable-only allocator fails successive ordinary/nothrow owner-thread
allocation once and restores allocation for diagnostics. On Windows, private
one-shot bad_alloc hooks cover before/after candidate preparation, every staged
command, effect construction and immediately before publication. The exploratory Windows Debug global
allocator override timed out at its second injected allocation; that attempt is
retained as failed evidence. Vendor/runtime OOM recovery is not guaranteed.
Both supported fault paths prove exact live-state rollback until a fault-free
preview/commit. This does not promise recovery from every vendor/runtime OOM path. Existing command/persistence cases remain intact. FeatureWorld attaches in 10;
full REQ-09 remains NOT_RUN until 09e.

09c adds [shared undo/redo](authoring-history.md) to changed batches on this same
owner, preserving preview purity and one-group publication. Retry retention
remains09d; full REQ-09 remains NOT_RUN until09e.
