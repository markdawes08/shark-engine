# Durable regional ownership

M05e adds persistent regional writers to the existing Server Store and worker.
Follow [ADR 0067](architecture/0067-fenced-region-handoffs.md),
[regional gameplay](region-world.md), [transfer cuts](gameplay-transfer-cuts.md),
[durable maintenance](durable-maintenance.md) and the [testing policy](testing-policy.md).
This is a trusted server control surface, separate from player commands and
development authoring. Store still has one exclusive local database owner.

## Identities and SQL boundary

`RegionWriter` contains the StoreId, CharacterId, WorldEntityId, RegionId,
RegionInstanceId and a positive epoch bounded by `maximum_durable_revision`.
It is distinct from the login session's ownership epoch, the Scene's common
simulation island stamp, a RegionGrant generation and an ExecutionPermit.

`claim_region` requires the exact durable character/entity binding. A new claim
starts at epoch one. An exact existing region/instance claim returns that current
record; it cannot take over a different owner or reset an epoch. Provision economic
fixture state before claiming the character. Ordinary initialization cannot bypass
an established regional writer.

`commit_region` compares the entire source writer and commits its successor,
incremented epoch and original `RegionTransferRequest` in one SQLite transaction.
The request binds a transfer ID, source/destination region instances, final tick,
accepted/applied input watermarks, current shared session command watermark and
SHA256 of the retained canonical cut. Source and destination ownership must differ;
applied input cannot exceed accepted input. Full uint64 tick/input/command values
are stored as canonical decimal text; epochs retain bounded SQLite integers.

The latest exact transfer is retained per character. Retrying that exact request
returns its original writer and `replayed=true`; a changed request with that retained
ID rejects. Earlier cuts carry stale source epochs and cannot commit again. This
is a bounded last-receipt service, not an unbounded ID history. Trusted hosts use
fresh transfer IDs for new intents. No client supplies a writer or cut.

Character checkpoints and economic transfers carry the writer captured at
admission. The actual SQL owner is checked inside the same transaction as the
mutation, in addition to early validation. Missing writers reject once a character
is claimed; supplying a writer for an unclaimed character also rejects. Changing
an ingress cache or queuing work before a cut does not renew stale permission.
Fenced attempts change no checkpoint, balance, revision or economic receipt stream.
An exact economic retry also needs the current writer before returning a receipt.

The economic fixture's regional writer authorizes its source character. Its
existing bilateral transaction still validates the target and expected economy
revisions atomically; the target need not be owned by the same region. This grants
no general remote entity mutation. `TransferRequest` and OperationId remain
unchanged: the writer is execution context, outside canonical economic intent.

## Runtime control and original work

CharacterAuthority hydrates bounded ownership observations at startup/recovery
and publishes updates only when its creating thread drains actual worker results.
`region_ownership` performs no live database I/O and returns an observation.
It refuses to present cached authority as known after a reconciliation fault.

`submit_region_claim` and `submit_region_commit` enqueue closed typed commands on
the existing persistence worker. `take_region` retrieves only control tickets;
ordinary `take` cannot consume them, and control take cannot consume player work.
They use the same sixteen retained slots, nonwrapping tickets and maximum
sixty-second deadlines. One control and one gameplay operation per account may
coexist so ownership can commit while an original economic request is retained.
These controls spend no player session sequence and expose no arbitrary SQL or
worker callback. Runtime mutation still uses actual ready session admission.

`submit_save` and `submit_transfer` reject a stale cached writer before spending
the player's command sequence, then dispatch that exact writer to SQL. A commit
already queued ahead of such a request may fence it at SQL even if admission
occurred while the cache still showed the source writer. That failed request does
not consume its economic OperationId; the new owner can reconcile/retry the exact
original request with a current writer and a new session command sequence.

Pending observations retain original ticket, session/entity, deadline, writer,
economic request or checkpoint expected revision/state, timeout and observed
result. A cut never fabricates replacement jobs. The same worker may finish
pre-cut work while gameplay is frozen. A write that committed before ownership
CAS remains an original valid result even if the destination observes it later.
A write reaching SQL after CAS with the old writer rejects.

Taking an owned result requires the current writer and exact retained character
and entity. A stale or missing writer cannot consume the slot. Successful
reconciliation through a successor returns the original admission writer and
original save/economic payload; it does not rewrite history. Existing live
session, Scene reference and deadline delivery checks remain in force.

## Uncertainty and compatibility

Deadlines and process shutdown never roll back an already dispatched write.
An I/O or worker exception faults admission and retains the original request for
reconciliation. A regional coordinator remains frozen while commit is unknown;
timeout alone cannot return write rights to its source.

After all retained jobs settle and are explicitly taken, `recover` joins the old
worker and reopens the exact actual Store, hydrating its ownership rows. It rotates
runtime/session identities as before. A post-COMMIT error can therefore resolve to
the already advanced writer; the old ticket/session never becomes valid again.
RegionWorld may adopt only an exact persisted last-transfer match to its original
cut. Recovery from a lost simulation process still needs M05f's native/script
state and routing work; a SQL digest is not a restartable gameplay image.

Store schema 4 adds validated ownership rows and last-cut fields. Ordinary open
accepts only current schema. Explicit offline migration supports exact historical
schemas 1, 2 and 3 with a verified backup, performs sequential upgrades atomically
and starts with no invented regional ownership. Existing schema-3 economy balances,
operation high watermarks, retained/pruned receipts and revocations remain intact.
Complete inspection, migration retry, backup and restore equality include all
ownership rows. An older restored snapshot must not silently replace a live world
with later acknowledged effects. Source/prepared formats and player wire versions
are unchanged.
