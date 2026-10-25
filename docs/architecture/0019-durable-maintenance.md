# ADR 0019: Explicit database migration and isolated restoration

Date: 2026-10-24. Status: accepted design for M03e; qualification is recorded
separately. Extends [ADR 0015](0015-durable-store.md),
[ADR 0017](0017-atomic-durable-transactions.md) and
[ADR 0018](0018-runtime-durable-recovery.md) within the current-machine scope.

## Offline owner and compatibility

Extend the existing Server-only Store with explicit static maintenance services.
The caller drains work and destroys the active CharacterAuthority/Store first.
Backup and migration acquire the existing exclusive SQLite ownership; inspection
and restore read a closed snapshot without repairing it. These services do not add
another live writer, public SQL endpoint, scheduler or mutable session bypass.
Human and terminal contributors use the same typed C++ results and CPU fixtures.
Operational administration and authoring adapters remain M08 and 15/16.

Ordinary `Store::open` still accepts exactly schema 3 and never migrates. Explicit
maintenance recognizes only the exact historical schemas 1, 2 and current 3.
Unknown/future versions reject before SQLite can write or recover them. Validate
complete DDL, identities, bounded records and economic history; a version number
or a snapshot's record counts do not establish compatibility.

Schema 1 upgrades to schema 2, then schema 2 upgrades to schema 3. Execute these
sequential transforms and their validation within one database transaction, so
there is no acknowledged intermediate migration. Preserve StoreId, WorldId,
character/entity identity, revisions, state and revocation tombstones. Newly
introduced account operation sequences begin at zero; migration creates no
currency, inventory or transaction outcomes. The active schema remains 3.

## Required backup and retry

Migration requires an existing, independently revalidated backup of the exact
pre-upgrade logical state, located in a separate canonical directory. Missing,
malformed, mismatched or stale backups reject before schema changes. The backup
is retained unchanged. A reported migration success follows the real COMMIT.

An interruption can leave the old or fully upgraded database. Explicit migration
maintenance may recover a recognized hot journal before inspecting the result.
After a lost reply, retry using the original backup: an already-current source
is accepted only if its complete logical contents equal the deterministic
upgrade of that backup. Later writes cause conflict instead of concealing a
different recovery point. No automatic downgrade or replay is provided.

## Backup and isolated restore

Use the pinned SQLite backup API under exclusive source ownership, with fixed
page/work bounds and no contention retries. It runs outside a source write
transaction. The destination is reserved exclusively and never overwrites an
existing path; every output is in a separate canonical directory. Reopen and
validate the completed copy, comparing complete logical state with the source
before acknowledging it. A failed or interrupted copy may leave an incomplete
or complete output, requiring explicit inspection and a fresh destination.
[SQLite backup contract](https://www.sqlite.org/c3ref/backup_finish.html)

Inspection validates supported closed snapshots without repairing or upgrading
them. Restore copies a validated snapshot into a new isolated candidate at the
same schema; legacy candidates require explicit migration before the current
server can open them. Neither operation switches an active server's path.
Snapshot summaries carry metadata, bytes and bounded counts; they are not
authorization, freshness proofs or durable migration tokens.

Restoring an earlier snapshot also restores earlier economic high-water marks,
revocations and revisions. It can therefore lose acknowledged effects and make
previously consumed operation identities admissible again. An old restore is for
isolated inspection/reconciliation, not automatic authoritative rollback. A
same-point restored restart requires the original authority to be quiesced and
no acknowledged changes after that backup. No simultaneous authoritative use
of original and restored copies is supported.

## Rollback and qualification limits

M03a builds accept schema 1, M03b builds accept schema 2, and M03c/M03d/M03e builds
accept schema 3. Schema-1/2-era builds cannot open schema-3 databases. There is no reverse
migration; a pre-upgrade backup can be used with its matching historical build
only within the same isolated rollback/reconciliation restrictions. Schema
versioning is independent of authored content and gameplay wire versions.

Acknowledged character/economic state has zero permitted authoritative rollback.
A backup is a point-in-time snapshot, without a scheduled checkpoint interval or
claimed elapsed-time recovery window. Software interruption and same-machine
restore do not establish physical power-loss, host/disk-loss or independent
disaster-recovery protection. No cloud service, new machine or resource-setting
change is required. M03f retains integrated qualification and full REQ-M03 closure.
