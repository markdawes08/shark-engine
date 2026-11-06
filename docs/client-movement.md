# Client movement prediction and interpolation

M04b connects presentation values to [authoritative movement](authoritative-movement.md)
and the existing [replica cache](network-replication.md), under
[ADR 0060](architecture/0060-client-movement-presentation.md). Use these CPU services
from the verified-server connection owner. No Editor, GPU or source document is
needed, and their returned poses grant no gameplay or authoring authority.

## Owned-character prediction

Create `net::MovementPredictor` from an accepted active, ready SessionSnapshot and
a fresh server bind state with last-input zero. Session/epoch must match. A fresh
login or movement binding uses a fresh predictor; packets cannot silently replace
its stream. Configuration bounds the pending window to 1..8 inputs and supplies
finite host-known Y gravity within the existing physics component limit.

The nominal model shares M04a's 20 Hz, three substeps, 6 m/s horizontal input and
250 m/s vertical ceiling. It integrates three small gravity/position increments.
It performs no collision query or native solver work. Server wall/contact or
gravity differences correct the prediction; cross-CPU lockstep is not assumed.
All predicted positions remain within the existing +/-8192 m local domain.

`prepare` takes an envelope from the host's one session sequencer and a checked
integer direction. It returns immutable exact input/bytes and publishes nothing.
Other session commands may occupy intervening session sequence numbers; movement
input IDs remain consecutive. Preparing again creates another candidate for the
same current revision, not a reservation or an instruction to retry an unknown
send. Keep the original prepared value when its outcome is uncertain.

`send` checks that owner/revision, encodes no new intent and admits the prepared
bytes through the actual client TLS FIFO. Prediction/history publication then
uses fixed-size values with no callback/allocation or validation window. A full
transport FIFO leaves the predictor unchanged and permits the same exact prepared
value to be retried after a known failure. A retained exact successful repeat
returns `duplicate` without sending bytes or moving again. History retains eight
pending receipts plus the latest successful admission; older acknowledged receipts
can expire. A stale/foreign preparation rejects before network admission.

Local FIFO success does not prove server admission. The host must keep the shared
session sequence synchronized with other controls/operations. A generic server
rejection has no input identity; call `apply(MovementRejected)` or `suspend` for
an ambiguous host outcome. This retains pending inputs, displays the last accepted
authoritative state and blocks new predictions. A later valid correction can
update the observation but cannot silently resume admission. Reconcile the session
and obtain a fresh binding explicitly. Never blindly resend an unknown input.

The eight-input bound limits speculative motion to eight modeled ticks. Full
history applies backpressure; it cannot silently drop older inputs. Position is
not extrapolated beyond the prepared/sent window. These are local coordinates,
distinct from world cells or origin/authority transfer.

## Corrections

Feed only decoded replies received from this connection's verified TLS server.
Validation requires the same session/epoch/stream, a newer completed tick,
nondecreasing acknowledged input no greater than locally sent input, and no more
acknowledged-input advancement than completed-tick advancement. An exact repeat
of the latest accepted correction is `duplicate`; older, conflicting, foreign or
impossible corrections reject without changing state.

After validation, retire the acknowledged prefix and replay only remaining inputs
from the committed server pose/velocity. A large server tick gap never creates an
unbounded catch-up loop. If approximate replay would leave the local domain, accept
the valid authoritative correction and suspend with the remaining exact inputs.
Prediction failure cannot veto committed server state.

The snapshot owns its authoritative base, predicted pose, pending count, revision
and reconciliation flag. Correction/replay invokes no Lua, durable operations or
presentation-event callbacks. A caller must not reinterpret replayed motion as
another attack, reward, animation event or Scene mutation.

## Remote interpolation

Use `ReplicaClient::apply(update, received_at)` on decoded verified-server updates.
It retains existing complete-candidate session, epoch, stream, revision, lifecycle
and capacity checks, staging both cache and timed history before one publication.
The host supplies a nonnegative, nondecreasing receive clock. Equal times coalesce
to one complete frame. Exact duplicate and missing-baseline observations advance
the receive-clock fence without changing or retiming presentation anchors.

At most eight complete frames of at most 32 entities are retained. A new full
baseline starts a new one-frame history; `discard_baseline` clears values/history
while preserving the existing stream/revision/retired-ID fences. An accepted new
untimed `apply` clears timed history; duplicate untimed application does not.

`sample(timestamp)` uses an explicit host-selected delayed receive time. It
does not invent a server tick, estimate clock synchronization or extrapolate
outside history. Samples clamp to retained endpoints. The topology always uses
currently live IDs, so a delayed sample cannot resurrect a despawn. A new entity
absent from either bracket uses its current committed pose immediately.

For shared IDs, decomposable TRS poses interpolate translation/scale and shortest
rotation. Signed-scale changes, legal affine shear or failed blend validation
fall back to the entity's complete current committed pose. No matrix-coefficient
blend can create a singular transform. `ReplicaPresentationSnapshot` is explicitly
display-only; sampling never changes the committed cache, revision or ACK state.
The host retains responsibility for display timing and supplying the latest
committed observation to gameplay/selection checks.

## Shared server publication

`CharacterAuthority::publish_replication` verifies a Region Scene in its durable
world, then calls the existing ReplicationServer using its private SessionAuthority.
`control_replication` routes the existing bounded ACK/resync admission through that
same owner. Selection remains a trusted explicit public-field allowlist; an
observer can see another character without gaining ownership of it. Controls and
movement still share the same session sequence. No database IO, new wire payload,
mutable session borrow or public component setter is introduced.

The [handoff](handoffs/M04b-client-movement.md) records actual input/correction TLS,
native collision correction, seeded decoded-message faults and a second actual
authenticated client receiving/interpolating published movement. Separate-process
replication also exercises timed ingestion and endpoint parity. CPU interpolation
does not qualify rendered smoothing or native UI, and decoded-message faults do
not simulate loss of encrypted TCP bytes. Full M04 combat/effect/adversity gates
remain open under the [checkpoint plan](roadmap/authoritative-gameplay-plan.md).
