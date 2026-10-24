# M03: Durable characters and transactional state checkpoints

Date: 2026-10-22. Base: `01968d5` on initially clean `main`.
Owner: REQ-M03. M02 and assignment 08 are complete. The user approved dividing
M03 into the six checkpoints below. **M03a and M03b are PASS under the
current-machine scope; M03c is next and full REQ-M03 remains NOT_RUN.** The
[M03a handoff](../handoffs/M03a-durable-store.md) records the store foundation;
the [M03b handoff](../handoffs/M03b-owned-characters.md) and
[receipt](../coverage/evidence/M03b-owned-characters.json) record 539 CPU CTests,
ten additional standalone Tools checks and protected package/API gates.
Follow [ADR 0007](../architecture/0007-mmorpg-architecture.md),
[durable identity rules](../architecture/0004-formats-and-identity.md),
[existing session authority](../architecture/0012-owned-sessions.md) and
[human/agent development](../architecture/0008-human-agent-authoring.md).

Every checkpoint includes its own tests and handoff. All gates run on the
current Windows PC and its existing WSL environment. A local embedded store is
sufficient; no cloud account, external database host or service fleet is required.
Source project/scene files remain authored content and do not become live storage.

## M03a - Store foundation

Select and pin a real transactional database with a server-only ownership and
dependency boundary. Implement explicit create/open, immutable store/world
identity, independently versioned schema, validated compatibility, bounded file
and query inputs, owned errors and creating-thread lifetime. A real transaction
creates the initial schema and metadata together. Define the durability and
retry contract before introducing character mutations. The
[store contract](../durable-store.md) and
[ADR 0015](../architecture/0015-durable-store.md) select SQLite 3.54.1 privately
in EngineStorage, with an exclusive local connection and rollback journal.

Gate: real files reopen with stable identity; missing, malformed, foreign,
future-version and mismatched-world stores reject; create never overwrites a
file. Test owner contention, normal/abrupt release, thread guards and interrupted
initialization before/after commit. Verify native Windows/WSL CPU/sanitizer,
public-header isolation, exact offline dependency/source/license verification,
protected target builds and local packages. No public SQL, character records,
player mutation protocol, migration or backup API is introduced here.

## M03b - Owned durable characters

[ADR 0016](../architecture/0016-owned-durable-characters.md) and the
[character contract](../durable-characters.md) define the server-only coordinator,
schema-2 records and explicit version-1 rejection. Account-only mTLS admission
supports first-character creation; existing Login selects an owned character.
Load/save use ready sequenced sessions and trusted server state. Durable roster
and character revisions reject conflicting writes; tombstones survive restart.
No gameplay storage protocol or synchronous live-tick integration is implied.

Reuse existing account/character ID domains and authenticated SessionAuthority.
Persist enrollment/ownership and bounded character state, with durable world/entity
identity and explicit save/revision outcomes. Integrate through the server owner;
revocation, reconnect and replacement must not revive stale authorization.
Reuse bounded Core completion machinery when storage leaves the owning thread.

Gate: create/select/save/reconnect through actual authenticated processes and
real server/store restart. Stable identity and ownership survive; unauthorized
access, stale sessions and conflicting revisions reject. Neither an ECS handle
nor a source-scene identifier grants durable ownership.

## M03c - Atomic transactions

Implement a small inventory/currency transaction fixture with server validation,
multi-record atomicity, revisions, operation identity and an auditable retained
outcome. Commit deduplication results in the same transaction as state changes.
Define request lifetime, retention, reused-ID mismatch and expiry together.

Gate: concurrent stale writes, duplicate/replayed requests, retries and mismatched
operation payloads cannot duplicate effects, create negative balances or violate
modeled inventory/currency conservation. Expiring a cached result never makes an
old economic operation valid again. Full game rules remain M07.

## M03d - Runtime failure recovery

Exercise real-store crash and timeout behavior around every durable mutation
commit boundary, including a lost acknowledgement and database outage. Bound
pending work, reject affected operations safely and reconcile committed outcomes
before admission. Fence completions by operation and owner incarnation.

Gate: every injected interruption has a defined retry/recovery result; acknowledged
durable effects survive and resolve to one outcome. Late completions cannot mutate
a replacement session/entity/region. Record any checkpoint rollback window
separately from economy/progression changes that may not be acknowledged then lost.

## M03e - Database migration and restoration

Implement real-store version migration, interrupted migration recovery, backup and
isolated restore. Define compatibility with deployed server builds, pre-upgrade
backup requirements, supported sequential upgrades and downgrade/rollback limits.
Never mutate a newer database merely because an older executable opened it.

Gate: independently prepared legacy/current stores, failed/interrupted migrations,
backup validation and restored restart preserve the declared identity and durable
outcomes. Restore into a separate local directory. State checkpoint-loss and
rollback constraints explicitly; local restore does not prove host/disk-loss
survival or independent disaster recovery.

## M03f - Integrated qualification

Connect character restart, authentication, concurrent transaction, commit-crash,
outage, migration and restore fixtures. Freeze the tested inputs and run the
Windows/WSL CPU/sanitizer, dependency and package gates with machine-readable
receipts. Retain exact commands, hardware, active workload, failed attempts and
verification limits. Close full REQ-M03 only when every required gate passes.

FeatureWorld integration belongs to 10 and MMOWorld acceptance to M09. Human and
terminal developers use the same documented CPU services and noninteractive test
results. Authoring adapters remain 15/16 and operational administration M08;
storage fixtures are not player-accessible SQL or editor commands. After full
M03, the next assignment is **09: Shared authoring commands and undo/redo**.
