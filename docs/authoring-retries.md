# Retained authoring mutations

09d adds CPU retry handling to the same AuthoredScene command/history owner.
Use `mutation_status`, `mutate` and `mutation_outcome` from public
`shark/tools/authored_scene.hpp`; values live in `authoring_retries.hpp`.
[ADR 0023](architecture/0023-retained-authoring-retries.md) records the decision.
These are local C++ services for future human/terminal adapters, not a gameplay
protocol or durable transaction system. Full REQ-09 remains NOT_RUN until 09e.

## Identity and admission

`AuthoringMutationId` contains a random owner UUIDv4 scope and nonzero uint64
sequence. Read `mutation_status()` for that scope, current revision, high-water
mark, next identity, oldest retained identity, counts, charges and limits. Supply
exactly the next sequence; caller coordination belongs to the shared owner.
Future gaps reject `out_of_order`; zero rejects `not_admitted`. Counter exhaustion
returns no next identity and never wraps. An unused current-scope query returns
`not_recorded`: this owner has never admitted that sequence.

`AuthoringMutationRequest` combines the ID, required expected SceneRevision and
one of AuthoringCommand, AuthoringBatch, UndoAuthoringMutation,
RedoAuthoringMutation or ClearAuthoringHistoryMutation. Every admitted decision,
including noops, stale revisions, invalid values and execution failures, consumes
one sequence and retains its original result. Correct a rejected intent under a
new identity after reading current state. Preview remains a pure ordinary call.

The outer Result fails if owner observation or admission preparation fails. No
operation was dispatched in that case. A reply's `outcome` either owns an immutable
original outcome or an admission/lookup error. Finally inspect the original
outcome's `result` for the command/history success or terminal error. These levels
distinguish recording a rejected operation from rejecting admission itself.

## Matching and observations

An identical retained submission returns `replayed`; a query returns `retained`.
Both share the original immutable outcome, including its original observed
revision, effect and error context. The enclosing reply reports the **current**
observed revision. A replay does not check today's expected revision again or
apply any operation. Refresh current state before preparing the next intent.

Input matching includes expected revision, operation variant, ordered batch
commands and every command field: stable IDs, exact Name bytes, parent, reparent
mode and all ten Transform doubles. Double equality compares raw bits, including
signed zero and NaN payloads; normalized equivalents are different inputs. Undo
and redo include direction and ticket. Clear includes its operation type and
revision. Within the admission bounds, a changed retained payload returns
`payload_mismatch` without mutation. Oversized input returns `not_admitted`
before comparison. Foreign or expired identity is checked before payload bounds
and always requires reconciliation.
Batch ID is correlation only and excluded from matching; an original retained
failure keeps its original correlation ID in diagnostics. New request correlation
does not change mutation identity or overwrite the original error.

Successful clear supplies `history_after` and an effect whose `changed` means
history was nonempty. Scene revision and dirty baseline do not change. Other
outcomes leave `history_after` empty and use existing command/history effects.

## Retention, expiry and restart

Defaults are 32 entries and 8 MiB; configurable hard limits are 64 entries and
16 MiB. The optional fifth `create` argument selects AuthoringMutationLimits;
existing calls remain source compatible. Rebuild C++ consumers with the engine.
Count/byte pressure evicts oldest complete entries after an admitted decision.
Eviction never rewinds the high-water mark. Requests that cannot fit individually
return `not_admitted`, preserving existing entries and the next sequence.

Input admission bounds each Name to 4096 bytes, batches to 256 commands and the
owned request charge to 512 KiB. This permits bounded invalid component values to
receive retained validation errors while rejecting unbounded caller data before
copying. Retained bytes conservatively charge request/string/vector capacities,
entry storage and a fixed outcome reservation of twice the 4096 source entity
bound in IDs plus 64 KiB for bounded engine diagnostics/metadata. This is neither
allocator overhead nor process RSS. Caller-held immutable shared outcomes may
outlive eviction and owner destruction; callers own those references separately.

The complete owned request, immutable-outcome storage and reserved ledger vector
exist before dispatch. Admission allocation failure dispatches nothing, evicts
nothing and consumes no sequence. A failure inside a dispatched operation is a
retained terminal decision. Publication is followed only by nonthrowing moves,
whole-entry eviction and shared pointer copies. No successful edit can lose its
retention entry because of a subsequent allocation failure.

Save, successful activation/reload and history clear preserve the same ledger and
scope. Activation still replaces Scene incarnation and clears history as before.
A retained result can therefore describe an earlier incarnation. Never infer
current scene contents from an old success. Evicted IDs and foreign scopes return
`unknown_outcome` from both query and submission, even with a fresh revision.
Owner destruction expires all retained evidence; every new owner/process obtains
a fresh random scope. No time-based expiry, durable retry log or source-file
retry state exists. Inspect actual state, reconcile the uncertain original intent
and explicitly decide whether a new intent is needed. Never blindly repeat an
expired mutation under a new ID.

Owner-thread and closed-owner guards precede ledger access. Retained lookups and
replays do not mutate Scene/history and can run during visits, pending work or
out-of-sync history. New requests use the original services and their guards;
their terminal conflicts are retained. Ordinary execute/undo/redo/clear calls
share Scene/history but do not acquire retry retention. File saves themselves are
not AuthoringMutation operations; retain their explicit publication/durability
results under the [file contract](project-files.md). M03 economic durability is
separate from this owner-local source-authoring ledger.

## Complete CPU example

Keep the exact request if its reply may be lost. Handle unknown outcome before
issuing another intent. This example returns both an owned request and its
immutable original outcome for caller retention.

```cpp
#include <shark/tools/authored_scene.hpp>
#include <utility>

struct RetainedEdit {
    shark::tools::AuthoringMutationRequest request;
    std::shared_ptr<const shark::tools::AuthoringMutationOutcome> outcome;
};

shark::Result<RetainedEdit> rename_once(shark::tools::AuthoredScene& owner,
                                        shark::AuthoredEntityId entity) {
    using namespace shark;
    using namespace shark::tools;
    auto state = owner.mutation_status();
    if (!state) return state.error();
    if (!state.value().next)
        return Error{ErrorCode::conflict, "Mutation sequence exhausted"};
    AuthoringMutationRequest request{*state.value().next, state.value().revision,
                                    AuthoringCommand{SetNameCommand{entity, {"Shared edit"}}}};
    auto reply = owner.mutate(request);
    if (!reply) return reply.error();
    if (!reply.value().outcome) return reply.value().outcome.error();
    auto outcome = reply.value().outcome.value();
    if (!outcome->result) return outcome->result.error();
    auto retry = owner.mutate(request);
    if (!retry) return retry.error();
    if (!retry.value().outcome) return retry.value().outcome.error();
    return RetainedEdit{std::move(request), std::move(outcome)};
}
```

`shark.tools.authoring-retries` (60 seconds, seed 1337) exercises matching,
immutable outcomes, validation/history failures, limits, eviction, lifecycle and
allocation rollback. `shark.tools.authoring-retries-process` (90 seconds, seed
1337) drops a successful reply, saves source and starts two fresh processes. It
checks foreign-ID rejection after an intervening edit, explicit reconciliation,
same-owner replay and persisted final source, with independent parent validation
and negative receipt controls. These are process-restart tests, not a claim of
crash-durable retry retention. FeatureWorld attaches in 10; adapters remain 15/16.
