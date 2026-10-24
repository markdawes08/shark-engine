# ADR 0018: Bounded runtime durable recovery

Date: 2026-10-24. Status: accepted for M03d implementation; qualification is
recorded separately. Extends [ADR 0015](0015-durable-store.md),
[ADR 0016](0016-owned-durable-characters.md) and
[ADR 0017](0017-atomic-durable-transactions.md). The
[runtime contract](../durable-recovery.md) defines the public API. Database
schema 3 and economic operation identity remain unchanged.

## One owner and a dedicated storage thread

CharacterAuthority remains the sole owner of authenticated admission and durable
character services. Its existing synchronous construction remains supported.
`open_runtime` opens, uses and destroys one Store on a dedicated private worker;
it never moves a live creating-thread Store to another thread. Startup and
recovery are explicit blocking maintenance. SessionAuthority, TLS and Scene
access stay on the creating thread. Only typed owned values cross the boundary.

The private executor accepts a closed set of existing Store operations. It is
not a second scheduler, arbitrary callback executor or public SQL service. Core's
existing bounded CompletionQueue carries trivial slot/sequence tokens; fixed
owned slots retain commands and results. Default and hard maximum pending work
are sixteen, configurable down to one. Queued, executing and completed-but-untaken
operations all count. At most one retained operation per account is admitted.
Blocking maintenance rejects while any runtime result remains retained.

Async save and transfer validate the actual ready sequenced mTLS session and its
authoritative Scene entity before enqueueing. The entity must have the expected
world and the character's immutable durable WorldEntityId. A RuntimeId identifies
the owner incarnation; a nonwrapping sequence identifies its ticket. Completion
delivery rechecks the live session, ownership epoch, connection and exact scoped
EntityReference. A replacement session, scene, entity generation or region scope
cannot receive the old result as ready. Later valid heartbeats do not invalidate
the same session's work. No completion mutates a Scene.

## Failure, timeout and recovery

Caller-supplied monotonic observations never regress. A deadline must be strictly
after now and within sixty seconds. Reaching the deadline faults admission but
does not cancel SQL, free capacity or imply rollback. An unsettled timeout stays
retained. Poll/take remain available after faults so every accepted operation
can settle and be inspected. Storage I/O, uncertain execution and caught worker
exceptions quiesce further work; queued commands then settle with owned failures.

The trusted owner distinguishes delivery eligibility from durable outcome. It
retains save inputs and the original transfer identity/payload. Recovery requires
all retained work to be settled and taken, joins the old worker, reopens the exact
StoreId/world/schema and hydrates the complete authorization registry. A new
RuntimeId and SessionAuthority invalidate all old tickets and sessions. A fresh
Login is required, using an authenticated connection; caller-owned TLS transports
are not forcibly closed. No operation is automatically replayed.

After reopen, inspect a saved revision/state or retry the identical retained
economic request. Never manufacture a new economic identity to resolve a lost
reply. Durable high-water marks still prevent expired operations becoming fresh.
Shutdown closes admission, joins the worker and cancels commands not yet
dispatched; an executing transaction may commit. Owned results are discarded only
after the join. This does not promise preemption of an OS filesystem call.

## Evidence and limits

Real child processes interrupt every durable mutation before and after commit
and after its successful reply. The matrix also covers partial debit, terminal
rejection and receipt pruning. A private test-only delegating SQLite VFS injects
actual write, sync and disk-full errors; fresh processes inspect and reconcile
the resulting real stores. Separate deterministic worker barriers exercise
timeouts, bounded queues, worker exceptions and actual session/Scene fences.

Acknowledged character checkpoints and economic changes have zero permitted
rollback under the existing SQLite durability policy. Unacknowledged writes can
be absent or committed and require reconciliation. No checkpoint cadence or
elapsed-time rollback budget is introduced before simulation owns scheduling.
These same-machine tests do not establish physical power-loss survival, hardware
fault tolerance, distributed M05 fencing or live 20 Hz latency. Migration/restore
remain M03e, integrated qualification M03f, gameplay M07 and administration M08.

Public C++ services and noninteractive machine-readable fixtures serve human and
terminal-agent developers equally. No new player protocol, CLI authoring stub,
editor requirement, LLM SDK or cloud dependency is introduced.
