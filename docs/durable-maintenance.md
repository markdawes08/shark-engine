# Durable database migration, backup and restoration

M03e extends the existing Server-only
[`Store`](../server/storage/include/shark/storage/store.hpp) under
[ADR 0019](architecture/0019-durable-maintenance.md). These are explicit offline
maintenance services, distinct from ordinary [store open](durable-store.md),
[authenticated ownership](durable-characters.md) and
[runtime failure recovery](durable-recovery.md). Qualification is recorded
separately from this contract.

## Public operations

All calls require the expected StoreId and WorldId. They return owned
`Result<T>` values with stable error code, operation and path context. They
expose no SQLite handle, SQL text, mutable authorization state or player protocol.

| Operation | Behavior |
| --- | --- |
| `inspect_snapshot(path, store, world)` | Validate a supported closed snapshot without migration or repair; return its summary. |
| `backup(source, destination, store, world)` | Acquire the source exclusively, copy using SQLite, then independently validate the new copy and complete logical equality. |
| `migrate(source, verified_backup, store, world)` | Require an exact pre-upgrade backup; atomically apply sequential schema upgrades or reconcile an already-committed retry. |
| `restore(snapshot, destination, store, world)` | Create and validate an isolated copy at the snapshot's schema, preserving all declared durable state. |

`SnapshotInfo` contains `StoreMetadata`, main-file bytes and account, certificate,
character, economy, operation-stream and receipt counts. Those counts are not
a substitute for validation or a proof of freshness. `MigrationResult` contains
before/after metadata and `already_current`; `before` describes the source found
by this call, including schema 3 after a committed retry. It is not a live session.

Drain and take all runtime work, reconcile uncertain outcomes, and destroy the
active CharacterAuthority/Store before maintenance. Each call owns its local
connections for its duration and releases them before returning. Contention
rejects; no asynchronous migration or automatic live-server switchover exists.
Startup, maintenance and filesystem I/O have no live 20 Hz latency guarantee.

## Files and validation

The existing absolute local path, portable UTF-8, regular-file, single-link,
rollback-journal and size limits remain. Each destination's parent directory
must already exist and must differ canonically from the source's parent.
Destinations are exclusively reserved: existing files, aliases or incompatible
sidecars reject, and existing content is never replaced or deleted. Trusted
callers still coordinate external file and directory changes; this is not a
hostile-filesystem sandbox or a network-filesystem profile.

The pinned SQLite backup API copies database pages rather than copying a live
main file without SQLite locking. Backup keeps exclusive source ownership;
restore holds a read transaction on its closed snapshot. The
destination is reopened and checked against the full source snapshot before
success, including exact schema/metadata, all records and bounded economic
receipts/high-water marks. The 16 MiB main-file limit and existing SQLite
query/record limits remain in force.

Read-only inspection and restore can accept an inert rollback journal, but they
never repair a hot one. Recovery requiring writes rejects without changing the
snapshot or its journal. Explicit backup/migration maintenance can recover a
recognized source under writable exclusive ownership; unknown future headers
still reject before SQLite opens them.

A failed/interrupted copy can leave an empty, incomplete or complete destination.
An error never authorizes deletion or overwrite. Inspect the output explicitly
and select a fresh destination when retrying. A complete result may survive a
lost acknowledgement; compare its validated contents with the source.

## Upgrade and deployed-build compatibility

| Database | Ordinary current open | Explicit migration |
| --- | --- | --- |
| Exact schema 1 | Reject without mutation | Validate backup, then 1 to 2 to 3 in one transaction |
| Exact schema 2 | Reject without mutation | Validate backup, then 2 to 3 in one transaction |
| Exact schema 3 | Validate and open | Accept only exact agreement with the original backup's deterministic upgrade |
| Unknown/future/foreign | Reject before SQLite recovery | Reject before SQLite recovery |

Migration retains store/world IDs, account and character ownership, stable entity
IDs, names/positions, revisions and revocation tombstones. Schema 1 has only
metadata. Schema 2 introduces the bounded registry; its migration adds zero
operation high-water marks and empty economic/receipt tables. No economic grant,
re-enrollment, new durable identity or restored session is implied.

Both sequential transforms and their full validation occur before one COMMIT.
The required backup is an existing independently validated snapshot of the exact
pre-upgrade logical state. A different backup of the same StoreId is insufficient.
Missing, corrupt, wrong-world, wrong-store or mismatched backups reject without
schema changes. Current semantic validation includes all bounded rows and
economic invariants, not just DDL or a schema-version field.

After interruption, migration can recover a recognized source journal and inspect
old versus fully committed state. Retry with the original backup. If the source
already equals that backup's deterministic upgrade, return `already_current`.
If subsequent state changed, reject and require explicit reconciliation. A lost
reply never authorizes a new backup of the upgraded source to conceal history.

M03a accepts schema 1; M03b accepts schema 2; M03c, M03d and M03e accept schema 3.
Current ordinary open does not accept legacy schemas and M03a/M03b builds do not
accept schema 3. There is no downgrade transform. Historical-build rollback
requires a matching pre-upgrade backup and the isolation restrictions below.

## Restored restart and rollback

Restore preserves the snapshot's schema and all logical values, including
retained terminal receipts, expired-operation high-water marks and revocations.
It never activates the candidate or overwrites the current database. A legacy
candidate must be explicitly migrated before current-server open.

A same-point restart can use the restored candidate after the original owner
has stopped and no acknowledged writes occurred after the backup. Open through
the existing CharacterAuthority and require fresh authenticated Login; sessions,
connection IDs and live entity handles are never restored from disk. Retrying
an economic request uses its identical original identity and payload.

An older snapshot cannot preserve later acknowledged writes. Restoring it can
also lower operation high-water marks, undo revocations and make old requests
appear fresh. Such copies are for isolated inspection/reconciliation. Never run
two authoritative copies with the same store identity or automatically resume
gameplay from a stale backup. Reconciliation and later operational promotion
are explicit responsibilities, not a freshness guarantee supplied by counts.

Acknowledged character and economic effects have zero permitted authoritative
rollback. No scheduled backup cadence, elapsed-time checkpoint-loss allowance,
physical power-loss guarantee or host/disk-loss recovery is established here.
All qualification uses this Windows PC and its existing WSL environment.

## Complete public C++ example

This trusted offline helper expects existing distinct directories and a new
backup filename. Runtime admission must already be stopped. A failed result
requires inspection of any output before a retry; it does not trigger overwrite.

```cpp
#include <shark/storage/store.hpp>

#include <filesystem>

shark::Result<shark::storage::MigrationResult>
upgrade_database(const std::filesystem::path& source,
                 const std::filesystem::path& backup,
                 shark::storage::StoreId store, shark::WorldId world) {
    auto copied = shark::storage::Store::backup(source, backup, store, world);
    if (!copied) {
        return copied.error();
    }
    return shark::storage::Store::migrate(source, backup, store, world);
}

shark::Result<shark::storage::SnapshotInfo>
inspect_restore_candidate(const std::filesystem::path& backup,
                          const std::filesystem::path& candidate,
                          shark::storage::StoreId store, shark::WorldId world) {
    auto restored = shark::storage::Store::restore(backup, candidate, store, world);
    if (!restored) {
        return restored.error();
    }
    return shark::storage::Store::inspect_snapshot(candidate, store, world);
}
```

After a lost migration reply, call `Store::migrate` directly with the original
backup; do not repeat the example's create-only backup step over an existing file.
These public services require neither SQLite headers nor an editor, GPU, LLM SDK
or external account. The noninteractive fixtures return machine-readable results.
M03f retains integrated qualification; FeatureWorld/MMOWorld and terminal/editor
adapters keep their assigned owners.
