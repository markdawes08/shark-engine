# Local durable store contract

M03a introduced a server-only local database owner in
[`store.hpp`](../server/storage/include/shark/storage/store.hpp).
[ADR 0015](architecture/0015-durable-store.md) records the dependency selection,
trust boundary and compatibility decision. This document defines behavior;
it does not itself assert that qualification has passed. The
[M03 requirements](coverage/requirements.json) include later character,
transaction, migration and recovery gates. M03b supplies
[owned durable characters](durable-characters.md), following
[ADR 0016](architecture/0016-owned-durable-characters.md). M03c advances the active
schema to 3 with [atomic economic transactions](durable-transactions.md), following
[ADR 0017](architecture/0017-atomic-durable-transactions.md). Earlier handoffs
retain their historical schema-1/2 evidence.

M03e adds explicit [offline maintenance](durable-maintenance.md) under
[ADR 0019](architecture/0019-durable-maintenance.md): sequential upgrades,
verified pre-upgrade backups and isolated restores. Ordinary open below remains
current-schema-only and never upgrades automatically.

## Public owner and identity

`Store::create(path, world)` returns `Result<std::unique_ptr<Store>>` after
initialization commits. `Store::open(path, expected_world)` returns the same
owner type after validating an existing supported database. Neither operation
accepts SQL, credentials, source-scene files or a generic mutable record bag.
`metadata()` returns an owned `StoreMetadata` value containing typed `StoreId`,
typed `WorldId` and `schema_version`. Schema version is independently fixed at 3;
source JSON, component and gameplay-wire version numbers do not set it.

The store generates a new persistent store ID through the existing OS-entropy
UUID implementation. Reopen preserves that ID and the configured world. A
valid stable ID identifies data, but grants no session, character or region
authority. [CharacterAuthority](durable-characters.md) connects durable account,
enrollment and character records to live session authorization. M03c adds bounded currency/item records, operation high-water marks and retained
transaction outcomes; these do not constitute a full game inventory.

The owner is neither copyable nor movable. Keep every operation and destruction
on its creating thread; SQLite's connection uses `NOMUTEX` under this ownership
contract. A move of the owning `unique_ptr` does not transfer thread ownership.
Wrong-thread public access rejects with `conflict`. There is one database
connection and no internal worker, queue or second authoritative mutable state.
Destruction on the creating thread is a caller obligation; the destructor does
not throw or offer a wrong-thread error result. Closing the owner releases its
database lock. The C++ API and fixtures require
no display, GPU, editor or LLM SDK.

## Files, initialization and compatibility

Supply an absolute path whose parent directory already exists on local storage.
The filename is at most 240 UTF-8 bytes; the resolved path is at most 4080.
Windows requires a local drive path, excluding UNC/device paths. The parent is
canonicalized and the remaining spelling passes the existing portable path
validation. Use a regular file in a trusted directory and coordinate external
file or directory changes.
Path validation rejects unsupported links/aliases and special files; it does
not promise resistance to a hostile topology race between checking and opening.
Do not rename, replace, hard-link or edit an active store, and do not issue
external SQL while the owner is active. The main file and any rollback journal
must each have exactly one filesystem link. An existing rollback journal must
be regular and at most 32 MiB. Any WAL or shared-memory sidecar rejects this
rollback-only profile. Creation also refuses an existing rollback journal at
the new name.

Creation exclusively reserves the requested new path through native filesystem
operations, then initializes the schema and metadata in one transaction.
Existing paths reject without replacement or deletion. A failure after
reservation may leave an empty, incomplete or committed file and its recovery
artifacts. Inspect the result explicitly; a failed call is not permission to
delete an existing store or blindly retry creation over it.

Opening never creates or migrates a database. Before SQLite can write anything,
the raw header must contain all of the following:

| Field | Required value |
| --- | --- |
| SQLite file signature | `SQLite format 3` followed by NUL |
| Application ID | `0x53484B44` |
| User schema version | `3` |
| Page size | `4096` bytes |
| File read/write versions | Both `1`, the rollback-journal format |
| Main-file size | Within `maximum_store_bytes`, 16 MiB |

Foreign, legacy version-1/2, future headers and incomplete initialization reject without SQLite
recovery or database mutation. No new database is silently substituted. A
recognized supported header permits SQLite to recover a hot journal, after
which the exact schema, metadata identity, expected world and integrity check
must pass. A wrong world or corrupt recognized database returns no owner;
recovery may already have changed its bytes. A read-only preflight cannot be
used to promise automatic repair of a hot journal.
[SQLite recovery sequence](https://www.sqlite.org/lockingv3.html#hot_journals)

The read/write open excludes CREATE and rejects an actual read-only connection.
SQLite permits a read/write request to fall back to read-only, so an open return
code alone is insufficient. [Open flags](https://www.sqlite.org/c3ref/open.html),
[actual connection mode](https://www.sqlite.org/c3ref/db_readonly.html)

M03c supports only its exact version-3 schema. Unsupported versions, additional
schema objects or incompatible metadata reject; opening does not run a migration
or rewrite them. Schema changes require an explicit compatibility decision and
real-store fixtures in their owning checkpoint.

The schema contains six fixed STRICT tables: metadata, accounts, certificate
enrollment, characters, economic records and retained transaction receipts, plus
only their expected uniqueness autoindexes. Accounts include a durable operation
high-water mark; the bounded economic contract defines receipt retention.
Canonical DDL lives in [store.cpp](../server/storage/src/store.cpp); inspection
compares complete stored DDL and expected indexes, not only column names.
The `shark_store` metadata table must contain exactly one row:

| Column | Required representation |
| --- | --- |
| `singleton` | INTEGER primary key, value `1`, constrained to `1` |
| `store_id` | Non-null TEXT of length 36, parsed as lowercase canonical UUIDv4 |
| `world_id` | Non-null TEXT of length 36, parsed as lowercase canonical UUIDv4 and matched to the caller's expected world |
| `schema_version` | Non-null INTEGER, constrained to and equal to `3` |

Both IDs use the existing validated non-nil identity type. The database
application/user versions, exact schema, `quick_check(1)` result and sole row are
checked while holding the exclusive transaction. Equivalent-looking rewritten
DDL is not automatically accepted as a compatible schema. Every account,
enrollment and character row is bounded and validated before returning the owner,
including immutable identity uniqueness, ownership references, boolean tombstones,
nonwrapping revisions, shared Name/WorldPosition rules and roster capacity. Economic records, high-water marks and complete contiguous
receipt windows additionally validate bounded values, ownership, outcome semantics
and consistent historical revisions before admission.
The complete validated registry is held as bounded owned values; no borrowed
database value or native handle crosses a public call.

## Connection policy and limits

The private integration pins SQLite 3.54.1 and compiles the reviewed amalgamation.
EngineStorage reaches Core and the private static dependency; Core, Runtime,
Client and Tools do not reach SQLite. The public include surface contains only
engine-owned contracts and standard C++ types. Runtime and Tools configurations
without Server do not populate the SQLite archive.

| Setting or bound | M03a policy |
| --- | --- |
| Journal mode | `DELETE` |
| Synchronization | `EXTRA` |
| Locking | `EXCLUSIVE`, acquired by `BEGIN EXCLUSIVE` and retained until close |
| Busy timeout | `0`; contention returns immediately |
| Database pages | 4096 pages of 4096 bytes; 16 MiB main-file maximum |
| Page cache | 1 MiB target; not a hard SQLite heap cap |
| Mapped database I/O | Disabled |
| Temporary storage | Memory, without a claim of a separate hard heap cap |
| Foreign-key enforcement | Enabled |
| Public query surface | Fixed typed metadata, registry, character and economic operations; no SQL |

Verify effective settings rather than trusting that a PRAGMA was accepted.
Setting a maximum page count cannot shrink an already larger database. Exclusive
locking can retain a journal even with DELETE; never delete that file as an
assumed orphan. Any journal size limit constrains residual size after a commit,
not peak transaction storage. [PRAGMA behavior](https://www.sqlite.org/pragma.html)

The main-file cap and fixed schema bound this foundation's data. They do not
establish a bound on total process memory, temporary/journal disk use or local
filesystem latency. The implementation bounds its private statements and
validation work; it exposes no arbitrary SQL or input-controlled schema.
Defensive configuration and untrusted-schema handling reduce unnecessary SQL
capabilities; they are not a sandbox for attacker-controlled database files.
[Connection configuration](https://www.sqlite.org/c3ref/c_dbconfig_defensive.html)

Private SQLite limits are 4096 bytes per value/row, 16384 bytes per SQL statement,
16 columns, expression depth 100, one SELECT term, 16 bound variables and 25000
prepared-program instructions. Attached databases, trigger recursion and SQLite
auxiliary worker threads are disabled. Prepared-program size is not a bound on executed VM steps
or I/O time. Schema validation operates on the bounded main file and stops on
the first incompatible object or row. These settings are implementation limits,
not public SQL capabilities.

SQLite ownership excludes cooperating connections until the store closes. It
does not implement distributed fencing, a persistent account epoch or a region
lease. The selected limits are a small local foundation profile, not measured
LOCAL-MMO-1 character/economy capacity. Later checkpoints must explicitly review
schema and limits as real workloads arrive.

## Results and recovery

Expected failures use the existing owned `Error`: `conflict` for an existing
create target, a different expected world, ownership contention or wrong-thread
access; `invalid_configuration` for malformed or
incompatible storage, and `io_failure` for storage I/O failures. Input/path
validation retains the existing argument, ID and path error distinctions.
Operation and bounded path context identify the failed operation; callers use
the stable code rather than parsing the human message.

A successful `create` means SQLite returned successful COMMIT with the fixed
durability configuration. It does not mean every conceivable hardware failure
has been tested. A COMMIT error is not acknowledged success and may have an
uncertain durable outcome; close and inspect/reopen before deciding how to
recover. Never report a rollback merely because the caller received an error.
An interrupted initializer either leaves supported committed metadata that
reopens, or fails validation without granting a usable owner.

M03c adds atomic economic mutations and retained retry results through the
[transaction contract](durable-transactions.md). Its persistent high-water marks
prevent expired operation identities from executing again. M03d's
[runtime recovery contract](durable-recovery.md) adds the crash/outage matrix,
bounded worker and session/entity completion fences. M03e adds explicit
[migration, backup and isolated restore](durable-maintenance.md); ordinary open
never invokes those maintenance operations. Ordinary process termination demonstrates
software recovery on this PC; it does not simulate physical host or disk loss.

## Public C++ example

Link a server-side caller to `EngineStorage`. Supply an existing parent directory,
a new file path and a valid `WorldId`. This example closes and reopens the same
store, returning its preserved metadata; a caller should retain the returned
error context on failure. No private SQLite header is needed.

```cpp
#include <shark/storage/store.hpp>

shark::Result<shark::storage::StoreMetadata>
create_and_reopen_store(const std::filesystem::path& new_path,
                       shark::WorldId world) {
    auto created = shark::storage::Store::create(new_path, world);
    if (!created) {
        return created.error();
    }
    auto initial = created.value()->metadata();
    if (!initial) {
        return initial.error();
    }
    created.value().reset();

    auto reopened = shark::storage::Store::open(new_path, world);
    if (!reopened) {
        return reopened.error();
    }
    auto restored = reopened.value()->metadata();
    if (!restored) {
        return restored.error();
    }
    if (restored.value() != initial.value()) {
        return shark::Error{shark::ErrorCode::invalid_configuration,
                            "Reopened store identity changed",
                            {.operation = "example.storage.reopen"}};
    }
    return restored.value();
}
```

The same API is available to human developers and terminal-driven executable
fixtures. Storage adds no player-facing SQL command or game-authoring CLI/UI.
Authoring adapters retain their existing owners, and later operational access
must remain separately authorized.
