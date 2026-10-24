# ADR 0016: Owned durable characters

Date: 2026-10-24. Status: accepted design for M03b; qualification is recorded
in its handoff. Extends [ADR 0015](0015-durable-store.md) and
[owned sessions](0012-owned-sessions.md) under the current-machine scope.

## Server owner and admission

`CharacterAuthority` in EngineStorage exclusively owns one `Store` and one
Core `SessionAuthority`. A complete bounded durable registry hydrates account,
character and certificate enrollment before admission; persisted account and
certificate revocations apply before that candidate becomes usable. No mutable
store or session authority escapes the coordinator. Core never depends on
storage, and the Server bootstrap retains its idle lifecycle.

First-character creation cannot require an already selected character. Add
`SessionAuthority::authorize_account` using the actual live server-side mutual
TLS transport, explicit enrollment, revocation and the existing authentication
budgets. It creates no session and changes no ownership epoch. Character
selection continues through the existing Login request and session wire v1.
Account admission rejects connections older than the last successful Login,
without itself advancing that connection watermark.
No copied peer identity, source-scene ID, ECS handle or durable ID proves ownership.

Live character load/save requires a current ready session, matching character,
SessionId and ownership epoch, and exact command sequence admission through
the same session authority. Replacement, logout, expiry, live revocation and
disconnect invalidate access. Restart restores records but never restores a
session or connection. Fresh OS-random SessionIds distinguish incarnations
even when process-local epoch counters restart; epoch alone is not a durable
region fence. Distributed ownership retains M05.

Trusted enrollment and revocation commit before updating the session registry.
Validate capacity before commit; an unexpected publication failure quiesces the
coordinator until reopen and reconciliation. Do not rebuild SessionAuthority
on each durable change, which would reset live leases and rate/fencing history.
Synchronous operations remain on the creating thread in the current idle
service/fixtures. They cannot be placed unbounded in a live simulation tick;
off-thread work must later return through Core's existing bounded completion
boundary with owner/operation fencing.

## Durable values and compatibility

Database schema advances independently to **2**, adding account records,
certificate enrollment/revocation tombstones and immutable character ownership.
Version 1, future and foreign headers reject before SQLite can write or recover
them. There is no implicit upgrade or downgrade. M03e owns actual migration and
restore; M03a's historical version-1 evidence is retained as such.

The profile admits 64 accounts, 128 certificates, 128 characters globally and
eight characters per account. Tombstones count toward those limits. Character
records preserve distinct AccountId, CharacterId, WorldEntityId and the store's
WorldId. State contains validated display Name and canonical WorldPosition only.
No ECS slot, authored ID, session, socket, replica, inventory, currency or
progression state is serialized. Names are not unique identity.

Account roster revisions and character revisions start at 1, increase without
wrap and stop at INT64_MAX. Creation compares the expected roster revision,
generates both character/entity IDs on the server and commits the new record
and advanced roster together. Save compares the expected character revision
and commits a complete validated server-accepted state with revision + 1.
The service does not validate gameplay movement: callers may only supply trusted
server output, never forward a player checkpoint as authoritative state.

An acknowledgement follows successful SQLite COMMIT. No automatic retry or
exactly-once promise is added. After a lost create/save reply, reload the roster
or character and reconcile IDs/revisions; do not refresh a revision and blindly
repeat the action. A failed/uncertain commit or rollback leaves the owner
unusable until reopen/inspection. Operation IDs, economic deduplication and the
full commit/outage matrix retain M03c/M03d.

## Evidence boundary

Actual authenticated local client/server processes create, select, save and
reconnect, then repeat after a complete server/store restart. Test-only fixed
server scenarios use existing typed session messages and authoritative fixture
state. They add no player storage protocol, administrative endpoint or arbitrary
SQL path. Both human and terminal developers use the same CPU services and
noninteractive fixtures. Public examples require no private SQLite/TLS headers,
editor, GPU or LLM SDK. FeatureWorld and MMOWorld remain 10/M09.
