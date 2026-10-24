# Runtime durable recovery

Owner: M03d / REQ-M03. [ADR 0018](architecture/0018-runtime-durable-recovery.md)
extends the existing [characters](durable-characters.md) and
[transaction](durable-transactions.md) services without changing schema 3.
EngineStorage remains Server-only; public headers expose engine-owned values.

## Opening and admission

`CharacterAuthority::open_runtime(path, expected_store, expected_world, limits,
runtime_limits)` opens the real Store on one dedicated worker, validates identity
and hydrates SessionAuthority before returning. The authority, TLS transports
and scenes remain on their creating thread. The worker never receives pointers
to those objects. Existing `create(unique_ptr<Store>)` stays synchronous.

`submit_save` and `submit_transfer` copy bounded trusted server values. They
require the exact next command sequence of a current ready mTLS session and an
actual authoritative Scene entity with the character's durable WorldEntityId in
the expected world. A player checkpoint or copied identifier is not authority.
Capacity/shape/binding failures reject before command admission; an admitted
stale revision or business rejection consumes the live sequence as before.

Runtime capacity is one through sixteen, default sixteen, with one retained
operation per account. Queued, executing, settled and untaken results all count.
Full capacity rejects explicitly. Existing synchronous storage calls are blocking
maintenance and reject while results remain retained. Session operations can
continue while healthy; no second session registry is introduced.

Each accepted submission returns `(RuntimeId, sequence)`. The owner incarnation
is a fresh UUID and the ticket sequence never wraps. These ephemeral tickets are
distinct from durable economic operation IDs and cannot authorize a retry.

## Polling and taking results

`poll(now)` drains available completion tokens and observes deadlines. `take`
also polls, then reports the requested slot. Negative or regressing runtime
observations reject; equal timestamps are allowed. Deadlines lie in `(now,
now + 60 seconds]` and expire at equality. Poll/take remain available after a
fault, while new admission is closed.

| Delivery | Meaning |
| --- | --- |
| `pending` | No settled outcome; slot retained. |
| `ready` | Settled result passes the current session and exact EntityReference fences. Its Result may still contain an ordinary validation/revision error. |
| `timed_out` | Deadline reached; SQL may still complete. An absent outcome means the slot is still retained. A present outcome consumes the settled slot and is for reconciliation. |
| `fenced` | Settled result belongs to a replaced/revoked/expired session, closed connection or different scene/entity/region incarnation. It cannot be delivered as current gameplay state. |
| `reconciliation_required` | A settled I/O/uncertain/worker error requires explicit recovery. |

An eligible result is an owned CharacterRecord or TransferResult. `take` never
changes a Scene. A later heartbeat on the same session does not fence its result.
Logout, replacement Login, revocation, not-ready state, lease expiry, closed
transport, destroyed/reused entity, closed Scene and replacement region Scene do.
Every settled take returns its owned outcome, including fenced and timed-out
delivery, then consumes the slot. The trusted owner must keep delivery eligibility
separate from durable outcome; these values are not player response payloads.

Timeout is not cancellation and does not release capacity until a settled take.
Keep original save input/revision and the returned original transfer request.
Do not acknowledge a timeout as failure-to-commit or resubmit with a new economic
identity. `RuntimeStats` reports capacity, retained, unsettled, completed,
timed-out counts and whether reconciliation is required.

## Explicit recovery and shutdown

First drain and take every retained result. `recover()` rejects while any slot
remains, joins the old worker, opens the exact StoreId/world/schema again and
hydrates current durable ownership/revocations. It rotates RuntimeId and all
sessions; a fresh Login is required. Existing caller-owned TLS connections may
be reused for that Login. Failure to reopen leaves admission closed and can be
retried explicitly after the underlying problem is resolved.

Inspect durable revision/state before deciding whether a character save needs
another write. Retry economic work with its identical original request/ID; a
retained receipt resolves success or terminal rejection, while expired identity
remains permanently expired. Cache-only queries are not evidence that the
database is available. There is no automatic replay or automatic revision refresh.

Destruction closes admission and joins the worker. Already-dispatched SQL may
commit; queued undispatched commands do not execute. The worker owns Store
destruction, and no callback can later access a Scene. An OS filesystem call
cannot be forcibly interrupted by this contract. Startup, maintenance, recovery
and teardown have no live-tick latency guarantee.

## Public C++ example

This complete CPU-only example uses the same service for human and terminal
developers. Link EngineStorage; no SQLite/TLS implementation headers are needed.
The caller owns the actual authenticated session, authoritative Scene and trusted
request. It schedules polling and explicit recovery outside a live simulation tick.

```cpp
#include <shark/storage/characters.hpp>

shark::Result<shark::storage::RuntimeTicket>
begin_durable_transfer(shark::storage::CharacterAuthority& owner,
                       shark::net::TlsTransport& connection,
                       const shark::net::SessionEnvelope& session,
                       shark::Scene& scene, shark::EntityHandle entity,
                       const shark::storage::TransferRequest& request,
                       shark::MonotonicTime deadline, shark::MonotonicTime now) {
    return owner.submit_transfer(connection, session, scene, entity, request,
                                 deadline, now);
}

shark::Result<shark::storage::RuntimeCompletion>
inspect_durable_transfer(shark::storage::CharacterAuthority& owner,
                         shark::storage::RuntimeTicket ticket,
                         shark::net::TlsTransport& connection, shark::Scene& scene,
                         shark::MonotonicTime now) {
    return owner.take(ticket, connection, scene, now);
}

shark::Result<void>
reopen_after_draining(shark::storage::CharacterAuthority& owner) {
    return owner.recover();
}
```

Inspect both `delivery` and `outcome`: only a ready successful result is eligible
for the current live owner, and transfer receipt status must be committed before
reporting an economic effect. A replayed receipt does not apply its effect twice.

## Qualification and durability limits

`shark.storage.recovery` uses real mTLS, controlled clocks, worker barriers and
actual Scene references (seed 1337, timeout 120 seconds).
`shark.storage.recovery-process` uses real child processes and database files
(timeout 240 seconds): 45 crash rows and 24 native-VFS outage rows, each seeded,
interrupted and reopened/reconciled in separate processes. The outage fixture
requires an observed write/sync/disk-full injection and a failed unacknowledged
write. The supervisor rejects malformed, incomplete or contradictory receipts.
Earlier store, character, transaction and network families remain required.

Acknowledged character checkpoints and economic effects have zero permitted
rollback under the existing SQLite commit policy. Unacknowledged writes must
be inspected because either old or committed state may exist. No elapsed-time
checkpoint-loss budget or scheduling cadence is claimed. Process/VFS tests on
this Windows PC and its WSL environment do not establish physical power-loss
survival, host-loss recovery, production capacity or distributed region fencing.
Migration/backup/restore are M03e; integrated qualification is M03f. FeatureWorld
and MMOWorld integration remain 10/M09, and authoring/administration adapters
remain with their existing owners.
