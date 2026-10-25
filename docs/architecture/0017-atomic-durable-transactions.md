# ADR 0017: Bounded atomic durable transactions

Date: 2026-10-24. Status: accepted for M03c implementation; qualification is
recorded separately in its handoff. This extends [ADR 0015](0015-durable-store.md)
and [ADR 0016](0016-owned-durable-characters.md) through the existing server-only
Store and CharacterAuthority. [The transaction contract](../durable-transactions.md)
defines the public behavior. Full game rules remain M07.

## Fixture and authority

The fixture transfers nonnegative integer currency and units of one fixed item
from one character to another. At least one quantity is positive. It is a gift,
not a bilateral trade that could debit an unconsenting recipient. Both characters
need initialized economic records and active accounts. A trusted, one-time
initialization supplies the fixture's starting resources; there is no later
arbitrary balance replacement or player grant endpoint. Currency and item counts
each range from zero to one billion. Economic revisions are independent of
Name/WorldPosition checkpoints and stop at INT64_MAX without wrapping.

The ready authenticated session must own the source character and operation
account. The existing session authority admits every economic query and retry
with the next live command sequence and its shared budget. Durable operation
identity is independent of that ephemeral sequence. CharacterAuthority retains
sole ownership; M03d's [ADR 0018](0018-runtime-durable-recovery.md) adds a private
worker for this same owner's Store without a second connection or scheduler.
Public receipts are trusted server values. They are not a player response codec
and may contain private economic state. Future adapters must select an authorized
response rather than send these records verbatim.

## Identity, lifetime and bounded history

An operation is identified by `(StoreId, AccountId, sequence)`. Each account has
a durable high-water mark, initially zero. Fresh execution accepts exactly the
next sequence, at most INT64_MAX. The complete typed request is retained along
with its terminal outcome; there is no hash-only payload comparison. The eight
most recent admitted outcomes per account form a bounded audit and retry window.

After source-owner authorization, a retained identical request returns its
original outcome before present-day business checks. A different payload under
the same retained identity rejects. An older identity with no retained receipt
is permanently expired: the high-water mark never falls when receipts are
pruned. Restart restores that mark and cannot make the operation fresh again.
Account records and their marks are not deleted or re-enrolled.

Lifetime is defined by this logical sequence, not wall-clock seconds. The next
unconsumed identity remains admissible while its account and source remain
authorized. Admission consumes it permanently. Sequence 1's result is retained
through high-water 8 and expires when 9 commits. There is no timed cache eviction,
clock-reset assumption, sequence wrap or promise of an indefinitely retrievable
audit. After expiry, callers reconcile owned state; they must not issue a new
identity merely to retry an unknown old effect.

Malformed requests, wrong store/owner, missing source economic state and sequence
gaps do not enter the durable stream. A well-formed, next-sequence request with
an authorized source gets a terminal outcome even when business checks reject
it. Stale revisions, unavailable target, insufficient assets, capacity overflow
and exhausted economic revisions therefore cannot turn into later successes
under the same operation identity. An expected business rejection is a successful
`Result<TransferResult>` with a non-committed status, not a storage failure.

## Atomicity and compatibility

One SQLite transaction changes both economic records, advances the account's
high-water mark, inserts the complete receipt and prunes the oldest receipt.
Rejected business outcomes change only the stream and receipt. Candidate values
and return objects are prepared before BEGIN; cache publication follows successful
COMMIT through nonthrowing swaps. A failed or uncertain commit quiesces the owner
until reopen. A lost reply can be resolved by the same retained operation; this
does not promise exactly-once network delivery.

The database schema advances independently to **3**. Versions 1 and 2, future
versions and foreign headers reject before SQLite opens them. M03e owns explicit
migration. The exact schema, record bounds, ownership, contiguous receipt windows
and outcome invariants are checked before admission on reopen. The existing
16 MiB file, 32 MiB journal and private SQLite limits remain fixed.

M03c tests one-sided debit rollback, before/after commit acknowledgement faults,
concurrent stale intents, authenticated retries, restart and expiry. M03d retains
the complete crash/outage/timeout matrix and asynchronous completion fencing;
M03e retains backup, migration and restore. The synchronous fixture is not a
20 Hz simulation performance claim, production economy or disaster-recovery test.

M03e amendment: [ADR 0019](0019-durable-maintenance.md) and the
[maintenance contract](../durable-maintenance.md) implement explicit schema 1/2
upgrades with a verified pre-upgrade backup; ordinary open remains strict schema 3.
Current-schema backup/isolated restore preserve economic records, retained receipts
and high-water marks. Earlier snapshots omit later acknowledged effects and can
make consumed identities fresh again, so they never authorize automatic live
rollback. Migration introduces no economic grants. M03c evidence remains historical.
