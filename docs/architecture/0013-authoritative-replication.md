# ADR 0013: Bounded authoritative replication

Date: 2026-10-18. Status: implemented and qualified for M02c; see the
[handoff](../handoffs/M02c-authoritative-replication.md).
Scope: the current Windows PC and existing WSL, following
[ADR 0010](0010-current-machine-scope.md).

## Decision

Publish an explicit, bounded selection from the existing authoritative Region
Scene through `ReplicationServer`. Use the authenticated transport and
[owned session service](0012-owned-sessions.md) for every publication and control.
The server owns selection and state. A client can acknowledge or request a fresh
baseline; it cannot submit a state update, choose arbitrary entities, change
components or acquire authoring privileges.

Replication version 1 contains one public field: the validated scene-local
affine world pose, encoded as twelve explicit binary64 coefficients. Names,
local component values, hierarchy, durable identities and private Scene revisions
are excluded. This fixed gameplay schema is independent of the authoring schema
catalog, source formats and later database schemas. Adding a component property
does not make it replicated. Follow [ADR 0004](0004-formats-and-identity.md) for
separate versions and identities.

Each stream permits at most 32 live replicas, one acknowledged state and one
pending update. This cap matches the current LOCAL-MMO-1 visible-entity target;
it does not establish player capacity, a replication frequency or a bandwidth
budget. A baseline has at most 32 spawns. A delta has at most 32 pose-bearing
records and 32 despawns, with strictly increasing nonzero replica IDs. The
largest payload is 3742 bytes, below the existing 4096-byte outer payload limit.
No fragmentation, unbounded history or new dependency is introduced.

## Ownership and lifetime

A stream binds an authenticated session ID, ownership epoch and one live Scene
incarnation. Its random stream UUID and monotonically allocated replica IDs are
separate from durable entity IDs and local handles. Replica IDs and revisions
never wrap or reuse an old identity for another entity. Publication validates
the entire selection and reads committed values through the Scene owner.

`SessionAuthority::current` verifies the actual server-side mutual-TLS transport
and expires ended leases without refreshing them. Publication requires a ready
live session. Reconnect uses a fresh authenticated session and stream, replacing
the old account stream only after complete candidate validation. Old session
IDs, epochs, connections and controls cannot act on the replacement. Owners
explicitly release ended streams by exact session ID.

`ReplicaClient` is an owned presentation cache. It validates a complete candidate
before publication and retains stream, revision and identity fences after local
cache loss. A missing baseline requires a newer full baseline; clearing values
does not reset replay protection. A new login constructs a new cache. The
authoritative Scene remains the only gameplay writer.

## Acknowledgement and bounded recovery

An application ACK confirms that the recipient applied one replication revision.
It does not commit an inventory change, grant a reward or establish durable
success. TLS FIFO admission and TCP delivery are also separate from these
application semantics. Durable effects remain M03.

The publisher retains one immutable pending update until an admitted ACK.
Repeated publication returns that same value. ACK expiry rejects until an
explicit resync or teardown; it does not allocate a growing retry history.
Resync discards the pending value and schedules a newer full baseline in the
same stream. The last-issued revision fence remains intact.

ACK and resync controls validate ownership, stream and revision before using the
existing session Heartbeat admission path. They share the session's exact-next
application sequence, rate budget and lease. Invalid controls cannot advance
that sequence, refresh the lease or acknowledge state. Separate bounded
per-stream control and resync windows limit retries, including rejected attempts
once the caller is associated with its live stream.

## Consequences and qualification

Human-written clients, terminal-agent fixtures and later adapters use the same
CPU C++ services and structured `Result` values. No editor, GPU, terminal protocol,
LLM SDK or second mutable scene is required. This adds no public player listener,
default credential or remote authoring command to the bootstrap executables.

The [replication contract](../network-replication.md) defines exact bytes,
limits, errors and call sequencing. Qualification is PASS across the current
Windows Debug/Release/ASan and WSL CPU/Server sanitizer profiles, real local
processes and protected packages: 446 CTests pass without failures or skips.
M02d spatial interest, privacy and connection budgets is next. Full REQ-M02
remains NOT_RUN.

M02d owns spatial interest, recipient authorization policy, entry/exit hysteresis,
priorities, byte/work budgets and measured TCP head-of-line costs. An explicit
selection here must already be authorized by its trusted caller; a pose allowlist
does not authorize disclosure of an arbitrary entity. M05 owns global-origin
coordination and fenced region transfer. M02e retains integrated adversity
qualification. Neither this decision nor a local fixture establishes production
MMORPG scale or support on additional machines.
