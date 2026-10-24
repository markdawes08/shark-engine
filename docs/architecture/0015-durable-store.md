# ADR 0015: Server-owned local durable store

Date: 2026-10-22. Status: accepted design for M03a; qualification is recorded
separately from this decision. Scope: the current Windows PC and existing WSL
environment under [ADR 0010](0010-current-machine-scope.md).

## Decision and dependency boundary

Use the exact SQLite 3.54.1 amalgamation as a private static dependency of the
server-only `EngineStorage` library. SQLite fits the first local transactional
store without an external service, credentials or another machine. This is a
local single-writer foundation for [M03](../roadmap/mmorpg-plan.md), not a capacity
claim for a production MMORPG or a distributed database decision. The store
implements the separate live-state database boundary required by
[ADR 0004](0004-formats-and-identity.md) and
[ADR 0007](0007-mmorpg-architecture.md). Authored project/scene files remain
EngineTools content; they neither become this database nor acquire server authority.

The reviewed input is
[`sqlite-amalgamation-3540100.zip`](https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip),
SHA-256 `0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9`,
with source ID
`2026-10-16 14:45:20 555e07ef33f69a0957d24d8e555b702464a131d6f1607f4c3c1e4551e8e6ca76`.
The upstream [3.54.1 release record](https://sqlite.org/releaselog/3_54_1.html)
dates it to 2026-10-16 and includes a fix for a possible 32-bit heap-buffer
overflow introduced in 3.54.0. The selected profile is x64. This review does
not assert that upstream has no remaining defects; fixed Shark statements and
the actual pinned build require executable qualification.

[The dependency lock](../../dependencies.lock.json) records the exact archive,
source/header hashes, compiler options and notice extraction. Only `sqlite3.c`
is compiled; the command shell and optional extension tools are not shipped.
SQLite's [public-domain policy](https://sqlite.org/copyright.html) is represented
by the checked first comment from its pinned `sqlite3.h`, installed as the Server
package's SQLite notice. Build-time licensing does not depend on a mutable web
page. Upstream C code receives sanitizer instrumentation without engine-owned
warning-as-error policy.

Server reaches EngineStorage and Core. EngineStorage owns the private SQLite
dependency; Core, EngineClient, Runtime, EngineTools and Editor cannot acquire
that dependency transitively. Runtime-only and Tools-only configurations do not
fetch or build SQLite. Public headers expose engine-owned IDs, values and
`Result<T>`, with no SQL text, SQLite handle or generic key/value interface.

## Ownership, schema and opening

A `Store` owns one connection on its creating thread. Its stable typed `StoreId`
and existing `WorldId` are persisted together at schema version 1. A store ID is
not a character, source scene, session, authority epoch or proof of permission.
Opening an existing store requires the caller's expected world and the exact
supported schema. There is no implicit migration, fresh identity on reopen or
boot-time authority mutation. M03b must connect durable account/character
ownership to the existing authenticated [session boundary](0012-owned-sessions.md)
without weakening its authorization checks.

`create` first reserves a new regular file using an exclusive native operation;
it never overwrites an existing path. Schema and metadata initialization form
one database transaction. A failed attempt can leave its newly reserved file,
including an uninitialized file, for explicit inspection. It does not delete
an existing file or silently retry against it. SQLite's open flag named
`EXCLUSIVE` does not provide this file reservation; that flag is a no-op for
`sqlite3_open_v2`. [SQLite open API](https://www.sqlite.org/c3ref/open.html)

`open` checks the raw file header before opening SQLite for read/write access:
SQLite magic, Shark application ID `0x53484B44`, database user version 1,
4096-byte pages, and rollback read/write format bytes equal to 1. Incompatible,
foreign, future and incomplete headers reject before SQLite can write them.
The accepted version-1 path may perform hot-journal recovery before complete
schema, metadata and integrity validation. This distinction matters because
even a database read can trigger recovery and modify database/journal files.
[SQLite rollback recovery](https://www.sqlite.org/lockingv3.html#hot_journals)

The connection must actually be writable, rather than accepting SQLite's
read-only fallback. Its schema, unique metadata row and expected world must
match before returning an owner. Version rejection is the M03a compatibility
policy; real migrations and interrupted-migration recovery retain their later
M03 checkpoint. Invalid recognized data is never replaced with a new database.

## Commit and resource policy

Use rollback journaling, `synchronous=EXTRA`, and lifetime exclusive ownership.
Acquire that ownership with an actual `BEGIN EXCLUSIVE` transaction, not only
the locking-mode setting. SQLite distinguishes starting a write transaction
from merely marking a transaction deferred.
[Transaction semantics](https://www.sqlite.org/lang_transaction.html)

The store verifies its selected PRAGMA values. EXTRA extends FULL with the
directory synchronization used when a DELETE-mode journal is unlinked.
Exclusive mode may retain a journal after commit; the filename's presence is
not proof of unfinished work. The cache setting is a target and the page limit
bounds the main database, not all SQLite heap or journal storage.
[SQLite PRAGMA contracts](https://www.sqlite.org/pragma.html)

The [store contract](../durable-store.md) fixes page size, main-file limit,
cache target, fixed-query budgets and failure behavior. There is no arbitrary
query entry point, automatic schema import, loaded extension or public database
administration endpoint. Busy handling returns immediately; it does not make
filesystem I/O nonblocking or establish a wall-clock latency bound. This owner
must not be inserted into the live simulation tick as an unbounded synchronous
operation. Later M03 integration reuses the existing bounded completion boundary.

Success is returned only after SQLite acknowledges COMMIT under the selected
configuration. A COMMIT error can leave an uncertain outcome and is never a
durable-success acknowledgment. Reopen/inspection must resolve that outcome
before a caller retries initialization. M03a qualifies initialization/reopen;
economy operation IDs, atomic results, deduplication expiry and retry resolution
belong to later M03 checkpoints. Process-interruption evidence does not prove
survival of physical power loss, disk failure or a filesystem that violates
SQLite's synchronization assumptions.

## Trust and consequences

The path belongs to an ordinary trusted local directory. Native checks reject
unsupported path aliases and links; callers must coordinate external SQL,
file replacement and directory-topology changes. SQLite locks coordinate
cooperating connections to that database. They do not fence distributed region
authority, protect against a hostile administrator or turn corruption parsing
into a security sandbox. No network filesystem or concurrent external database
maintenance is supported by this profile.

Both human contributors and terminal agents can exercise the same CPU C++ API
and noninteractive fixtures. This adds no editor-only workflow, model SDK,
player storage command or premature authoring adapter. M03a contains only
creation, validated reopen and owned metadata. Character/session restart,
economy transactions, outages, migrations, backups and integrated qualification
remain explicit later work; full REQ-M03 cannot pass from this foundation alone.
