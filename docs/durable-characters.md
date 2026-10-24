# Owned durable character contract

M03b adds [`CharacterAuthority`](../server/storage/include/shark/storage/characters.hpp)
as the server-only owner of a durable Store and the existing authenticated
SessionAuthority. [ADR 0016](architecture/0016-owned-durable-characters.md) records
the design and schema compatibility decision. M03c extends this owner with [atomic economic transactions](durable-transactions.md)
under [ADR 0017](architecture/0017-atomic-durable-transactions.md). Runtime recovery,
migration and integrated gates remain later M03 work; character checkpoints alone
do not establish those gates.

## Ownership and startup

Move an opened or newly created Store into `CharacterAuthority::create`. Its
complete bounded registry is validated and loaded before the coordinator is
returned. Account and certificate revocation tombstones survive restart. A
registry larger than the supplied SessionLimits rejects startup instead of
partially admitting accounts. The coordinator exposes no mutable Store or
SessionAuthority reference; an error during startup destroys the inactive
candidate and releases ownership.

All methods and destruction belong to the creating thread. Moving a unique_ptr
does not transfer thread ownership. These synchronous database operations are
appropriate to the current idle fixture, not a latency-qualified simulation
tick. No second scheduler or background registry writer is introduced. The
existing bounded Core completion mechanism remains the required integration
when persistence leaves the owner thread.

Trusted owner calls provision accounts, enroll certificates and revoke either.
They are not player requests, development-authoring commands or an administrative
network API. Durable commit precedes authorization-cache publication. Capacity
is checked first; unexpected publication failure stops admission and requires
reopen. The active SessionAuthority is never rebuilt to apply a single change.
Per-session revocation remains intentionally ephemeral; persisted account or
certificate revocation prevents a fresh login after restart.

## Account and session admission

`roster(transport, account, now)` and `create_character` call the new bounded
account-only admission path on the actual server-side mTLS transport. The
certificate must be enrolled to that account and both must remain unrevoked.
This path shares global, certificate and account authentication attempt windows
with Login. It does not issue a session, select a character or change an epoch.
Once a newer connection logs in, account-only operations on an older connection
also reject. Account admission never advances that successful-login watermark.

`apply` forwards existing Login, SetReady, Heartbeat and Logout to the same
SessionAuthority. Login selects only an owned character. `load` and `save`
recheck the actual live transport, current ready session, SessionId, character,
epoch and exact next sequence. Their accepted admission uses Heartbeat and
therefore the existing command budget and lease policy before touching storage. Replacement, stale
transport/envelope, logout, expiry, revocation and disconnect cannot regain access
through a copied ID. No session, process connection token or local ownership
epoch is restored from the database. New sessions require fresh authentication
and OS-entropy SessionIds after restart.

Detailed owned errors are for trusted server callers. Player adapters must
return the same generic rejection for unknown, foreign, revoked and stale
authorization, without account, character, path or database diagnostics. The
process fixture uses `public_session_reply`; no new storage wire codec is
advertised. Gameplay wire and session wire versions remain 1.

## Records, revisions and retry

The [Store](durable-store.md) admits these fixed bounds:

| Value | Limit or invariant |
| --- | --- |
| Accounts | 64, including revoked records |
| Certificate enrollments | 128, including revoked records; one account per nonzero SHA-256 fingerprint |
| Characters | 128 globally, eight per account; immutable owner and identities |
| State | Shared validated Name (1–256 UTF-8 bytes) and canonical WorldPosition in the store world |
| Revisions | Integer 1 through INT64_MAX; exhaustion rejects without wrap |
| Identities | OS-generated CharacterId and distinct WorldEntityId, plus existing AccountId and WorldId domains |

An account starts with roster revision 1 and no characters. Successful create
compares `expected_roster_revision`, inserts the complete record at character
revision 1 and increments the roster revision in the same transaction. The
bounded owned roster provides the current revision and all owned records.
Names need not be unique and are never authorization.
Rosters and registry character/account lists sort by their canonical IDs;
certificate enrollment lists sort by fingerprint bytes. Character positions
reuse the validated store WorldId; cell and offset values persist per character.

Save requires `expected_revision` equal to the current character revision.
The complete candidate passes the existing Name/WorldPosition validators and
must belong to the store world. Only trusted server-accepted state reaches this
method: authentication alone does not authorize movement, teleportation or a
player-submitted state snapshot. Server gameplay validation retains M04.
Success returns the complete committed record and revision + 1. Invalid state,
wrong ownership or a stale revision cannot alter durable state or identities.
No currency, item, progression or source-scene fields are accepted.

Errors distinguish conflict, invalid input, incompatible storage and I/O through
the existing owned Result/Error code. Reconcile a conflict by loading the owned
record; no current foreign record is returned in an error. An admitted command
consumes its sequence even if a subsequent storage operation fails or its
expected revision conflicts. Pure invalid state rejects before admission. A lost
reply requires reading and reconciling the durable roster or character, never
blindly retrying with an advanced revision. These non-economic create/save operations do not gain retry deduplication.
M03c adds it specifically for the separate economic TransferRequest contract.

Commit acknowledgement means SQLite accepted COMMIT under the existing
DELETE/EXTRA policy. Commit or rollback uncertainty quiesces the store and
coordinator until destruction and reopen; an error never claims rollback or
durable success. Creating a fresh coordinator does not erase persisted
revocation. Process restart tests establish software recovery on this PC, not
physical power-loss survival or distributed region fencing.

## Public C++ example

Link a trusted server caller to EngineStorage. `accepted_state` below must be
produced by server rules, and the envelope must come from that connection's
owned session sequence. The function cannot grant access with a copied peer
snapshot or expose SQLite/TLS implementation types.

```cpp
#include <shark/storage/characters.hpp>

shark::Result<shark::storage::CharacterRecord>
checkpoint_accepted_character(shark::storage::CharacterAuthority& owner,
                              shark::net::TlsTransport& connection,
                              const shark::net::SessionEnvelope& session,
                              std::uint64_t expected_revision,
                              const shark::storage::CharacterState& accepted_state,
                              shark::MonotonicTime now) {
    return owner.save(connection, session, expected_revision, accepted_state, now);
}
```

Human and terminal developers exercise this same device-free API and bounded
process fixtures. No gameplay storage endpoint, editor widget, CLI adapter,
operational administration or LLM provider integration is introduced.
