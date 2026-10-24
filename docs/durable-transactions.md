# Atomic durable transaction contract

M03c extends the server-only [Store](durable-store.md) and
[CharacterAuthority](durable-characters.md) with bounded economic records and
atomic operation outcomes. [ADR 0017](architecture/0017-atomic-durable-transactions.md)
records the design. Public types live in
[`store.hpp`](../server/storage/include/shark/storage/store.hpp); session admission
lives in [`characters.hpp`](../server/storage/include/shark/storage/characters.hpp).
Qualification results belong in the M03c handoff, not in this behavior contract.

## Small conservation model

`EconomyState` contains unsigned integer `currency` and `item_units`, each at most
`maximum_economy_amount` (1,000,000,000). There is one implicit modeled item, no
item catalog, equipment, pricing, loot or progression system. `EconomyRecord`
binds that state to an immutable account and character with an economic revision
from 1 through INT64_MAX. This revision is separate from the character checkpoint
revision. Saving Name/WorldPosition cannot replace economic state.

Trusted server setup calls `initialize_economy(account, character, initial)` once
for an existing owned character. Reinitialization rejects even after all assets
have been transferred away. Initialization is the explicitly recorded source of
the fixture's resources; subsequent transfers conserve total currency and item
units across the two participants. There is no delete/reset or arbitrary balance
write API. This setup method is not exposed to players or authoring tools.

`TransferRequest` specifies an operation identity, distinct source and target
characters, expected economic revisions for both, and an `EconomyState quantity`.
Each quantity is bounded, at least one is positive, and revisions are nonzero
and at most INT64_MAX. A committed transfer subtracts from the source, adds the
same amounts to the target and increments both economic revisions once. Neither
negative balances nor arithmetic overflow is permitted. A transfer cannot debit
the recipient: bilateral trading rules and consent remain M07.

## Operation lifetime and reconciliation

`OperationId` contains the StoreId, actor AccountId and sequence. It is scoped to
the durable store and account, not a connection, SessionId, socket, ECS handle or
character revision. Account-wide sequencing also prevents two characters of the
same account from independently consuming the same operation slot.

`operation_stream` returns an owned bounded `OperationStream`: store, actor,
`high_watermark`, and at most eight receipts ordered by sequence. The initial
mark is zero; a new operation must use exactly `high_watermark + 1`. At INT64_MAX
there is no next operation. Queries do not reserve a sequence. Competing requests
must handle the resulting conflict and explicitly reconcile their intent.

| Request condition after current source authorization | Durable behavior |
| --- | --- |
| Retained identity, identical complete payload | Return original receipt with `replayed=true`; no write |
| Retained identity, different payload | Conflict; no write or replacement receipt |
| Sequence at/below mark with no retained receipt | Expired conflict; never execute again |
| Sequence above next | Gap conflict; no admission or write |
| Exactly next, well-formed and authorized | Commit one terminal outcome and advance mark |

An identical retained retry is resolved before checking today's target state,
balances or revisions. A terminal failure remains that failure even if a later
operation fixes its cause. Request identity includes every request field,
including both expected revisions and both quantities. Do not edit a retry's
payload or refresh its revisions under the same identity.

Retention is a count, not an elapsed-time timeout. The most recent eight admitted
outcomes remain queryable. The ninth outcome evicts the first receipt atomically,
while its sequence remains below the durable mark forever. Reopening restores
the same window and mark. The next unconsumed sequence has no wall-clock expiry;
normal session/account authorization still applies. After an expired/unknown
result, inspect owned state and reconcile the application intent. A new operation
ID is a new action, not a safe blind retry of the expired one.

## Terminal outcomes and errors

`TransferResult` contains the complete `TransferReceipt` and a `replayed` flag.
The receipt holds the original request, stable `TransferStatus`, and owned
optional economic snapshots. Every admitted outcome has a source snapshot.
Committed snapshots describe both post-transfer records. Rejected outcomes
describe the checked state where available; an
unavailable target has no target snapshot. These historical values do not assert
the records are still current. Re-query current state using the owned service.

| Terminal status | Effect |
| --- | --- |
| `committed` | Both states and revisions change atomically |
| `target_unavailable` | Target absent, uninitialized or account inactive |
| `stale_revision` | At least one expected economic revision differs |
| `revision_exhausted` | A participant cannot advance its revision |
| `insufficient_assets` | Source cannot supply the requested quantities |
| `capacity_exceeded` | Target would exceed a balance bound |

Business rejection is a terminal, durable result, so it consumes the operation
sequence and occupies a receipt slot. `Result` remains successful with the
non-committed status. Bad request shape, wrong store/actor, unauthorized or
uninitialized source, sequence gaps, mismatched retries and expired identities
return an owned Error without advancing the durable stream. Existing
`invalid_argument` and `conflict` codes distinguish validation and conflict;
I/O/incompatible storage retain the Store error contract. Callers must check
both Result success and terminal status rather than treating any reply as an
economic commit.

For operation conflicts, stable `Error.context.operation` values
`storage.transfer.expired`, `storage.transfer.mismatch` and
`storage.transfer.sequence` distinguish expired outcomes, changed payloads and
sequence gaps/exhaustion without parsing human messages. Other ownership or
store conflicts use `storage.transfer`.

## Authenticated owner

CharacterAuthority owns the same Store and SessionAuthority as before. Its
`economy`, `operation_stream` and `transfer` methods require the actual server-side
live mTLS transport and a current ready SessionEnvelope with the exact next
command sequence. Queries return the selected character's economy or its own
account's stream. Transfer additionally requires operation.actor and source to
match the authenticated account and selected character. Copying a durable ID or
receipt never authorizes access.

Every admitted query and retry spends the existing Heartbeat command budget and
lease admission. Pure malformed requests and mismatched session ownership reject
before that admission; a subsequent storage conflict or terminal rejection
consumes the live command sequence. A retry needs a fresh live envelope but the
same durable TransferRequest. Replacement, disconnect, logout, expiry and
revocation remain effective even when a matching durable receipt exists.

Receipts are trusted server values and may include recipient-private balances.
They must not be forwarded wholesale to a player. The test protocol returns
generic session acceptance/rejection through existing wire version 1; economic
values appear only in trusted local fixture receipts. There is no new player
storage codec, editor command, CLI adapter or operational administration endpoint.

## Persistence and bounds

Schema 3 adds economic records, operation high-water marks and retained receipts.
There are at most 128 economic records and 512 retained receipts (8 per 64 accounts).
The exact DDL and all bounded candidate rows are validated when opening a store.
Unsupported versions 1/2 reject without migration or writes; actual migration and
restore remain M03e. Public types expose neither SQLite nor native TLS handles.

Both record updates, operation mark, result insertion and pruning share one real
SQLite transaction under the existing exclusive DELETE/EXTRA policy. Complete
candidate/cache/result allocation precedes BEGIN. Only successful COMMIT permits
acknowledgement and cache publication. A precommit error that rolls back does
not consume an identity. Commit uncertainty faults the Store and coordinator;
destroy/reopen before resolving a retained operation. Never refresh revisions
and resubmit as a new action merely because the original acknowledgement failed.

All calls and destruction remain on the creating thread. Synchronous bounded
fixture I/O has no live-tick latency guarantee. M03d owns the full crash/outage
matrix, pending-work bounds and fenced completion integration. Same-PC process
tests do not simulate physical power loss or an independent database host.

## Public C++ example

Link the trusted server caller to EngineStorage. Construct `request` from a
validated server intent and a reconciled operation stream. Both humans and
terminal agents can compile this CPU-only example without SQLite/TLS headers,
display, editor, LLM SDK or a cloud service.

```cpp
#include <shark/storage/characters.hpp>

shark::Result<shark::storage::TransferResult>
transfer_owned_resources(shark::storage::CharacterAuthority& owner,
                         shark::net::TlsTransport& connection,
                         const shark::net::SessionEnvelope& session,
                         const shark::storage::TransferRequest& request,
                         shark::MonotonicTime now) {
    return owner.transfer(connection, session, request, now);
}
```

The caller tests `result.value().receipt.status == TransferStatus::committed`
before reporting a committed effect and handles `replayed` without applying it
twice to live state. This synchronous fixture does not yet mutate a simulation.
FeatureWorld/MMOWorld remain assigned to 10/M09.
