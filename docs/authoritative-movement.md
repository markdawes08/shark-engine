# Authoritative character movement

M04a implements a server-owned CPU movement slice under
[ADR 0059](architecture/0059-authoritative-character-movement.md). Follow the
[checkpoint plan](roadmap/authoritative-gameplay-plan.md) and
[testing policy](testing-policy.md). This is a host service, with no Editor or
terminal authoring command and no client component setter.

## Admission and ownership

`server::CharacterMovement` lives in EngineStorage's server-only composition.
Creation captures one live leased Region Scene, native physics identity and
explicit canonical WorldPosition origin. All methods require the creating thread;
closed/replaced scenes or physics owners reject. Native handles and source
parsers stay private to their existing owners.

The trusted host calls `bind` for an existing durable-owned entity/body. Its actual
server-side mTLS connection must have a ready active session whose cached durable
world/entity mapping matches that Region Scene. Binding creates a fresh stream
UUID and an initial state with last-input zero. It consumes no session command.
Duplicate entity/account/connection bindings reject. One host remains responsible
for ordering access to the shared Scene/physics owners and reverse teardown.

`CharacterAuthority::authorize_entity` rechecks transport/session provenance,
readiness and cached durable ownership without database IO, sequence admission
or lease refresh. A returned snapshot is an observation, not a reusable grant.
During `submit`, shape, queue, stream, entity/body and exact-next movement-input
checks precede existing Heartbeat admission. That admission validates the complete
SessionEnvelope and spends the shared session sequence/rate budget. A rejection
before admission leaves that sequence available; admitted inputs are retained
without another allocation. Authenticated clients still cannot propose a pose,
speed, duration, teleport, collider or durable checkpoint.

## Initial movement profile

| Setting | Value |
| --- | --- |
| Authoritative time | 20 Hz, called once per existing ServerScheduler tick |
| Collision | Three native substeps per tick |
| Character body | Root dynamic non-trigger sphere, radius 0.5 m, mass 1 kg, unit scale, zero friction/restitution, nonempty collision mask |
| Horizontal input velocity | At most 6 m/s before native integration, using integer X/Z directions scaled by 1000 with squared length at most 1,000,000; contact response can change the resulting velocity |
| Vertical motion | Preserve native vertical velocity, clamped to +/-250 m/s before the next update; angular command zero |
| Capacity | At most 32 bound characters, eight FIFO inputs per character |
| Missing input | Zero horizontal velocity for that tick; gravity/contact response continues |

One queued direction is one future tick. Sending eight inputs at once cannot
advance eight movement ticks. Input sequences begin at one for a fresh stream,
advance exactly once and never wrap. The SessionEnvelope sequence remains shared
with session, replication and durable operations; it is not the movement-input
identity. No input is held indefinitely through a missing connection.

This primitive profile verifies actual static-wall blocking with the existing
native physics solver. It does not yet supply jumping, stairs, slopes, terrain,
CCD or a general humanoid controller. Those limitations do not reduce later
MMORPG world/navigation requirements or imply cross-CPU lockstep.

## Tick, correction and failure semantics

The host polls/drains transport frames, decodes input and submits bounded values.
It calls `step` once from the existing ServerScheduler callback with a bounded
span of live server transports. The span is borrowed only for that call. Each
actor is rechecked against its original connection/session/character/epoch and
exact entity/body generation. Missing, revoked, unready, replaced or stale actors
lose queued input; a still-valid old body receives horizontal stop. A replacement
body is never addressed through the retired handle. Rebinding requires the old
binding to have been fenced and creates a fresh stream.

The adapter preflights all actors before immediate velocity writes, then executes
one shared PhysicsWorld step. Every successful tick returns owned per-connection
corrections with session/stream, ownership epoch, completed native tick, last
applied input, committed local position and native velocity. Initial corrections
have last-input zero; neutral ticks preserve the previous last-input. The host
sends each delivery only to its indicated connection, using the ordinary bounded
transport FIFO. A send failure cannot roll back or replay the completed tick.

The result also returns the complete bounded native contact batch for existing
owner-thread dispatch. No native callback invokes Lua, and this adapter executes
no Lua callbacks. Dispatch preexisting contact observations before another tick;
do not silently discard contacts or introduce a second queue/scheduler. M04c
will define the connected gameplay/Lua order.

Errors after the first native velocity write permanently fault the adapter.
This includes post-step state/global conversion failure: `stats` retains the
completed native tick when one completed, and later movement calls reject. It is
not a rollback. Keep the error and reconcile/replace the host explicitly rather
than retrying an unknown tick. Ordinary preflight rejection preserves pending
input and does not fault the adapter. General C++ allocation exhaustion retains
the existing transport/owner limitations.

`position` translates a committed Scene-local pose through the explicit origin
with `translate_world_position`. It returns an owned trusted observation for the
existing durable checkpoint owner, performs no save and grants no client state
authority. Cell/origin transfer remains M05; global cells never enter native
physics floats. The origin and local pose must remain globally representable.

## Movement wire protocol 1

Outer authenticated client-to-server kind 9 is `movement_input`; server-to-client
kind 10 is `movement_state`. Diagnostic TLS and reversed directions reject.
Each payload starts with version u16 BE, kind u8, reserved-zero u8. All UUIDs use
their canonical 16 bytes and integers use big-endian order.

Input kind 1 is exactly 80 bytes: SessionEnvelope (48 bytes), stream UUID,
nonzero input u64 and signed two's-complement X/Z int16. Reply kind 1 is exactly
108 bytes: session UUID, ownership u64, stream UUID, completed tick u64,
last-input u64 and position/velocity XYZ IEEE754 binary64 values. Reply kind 2
is a four-byte generic rejection, without internal diagnostics or identifiers.
The host maps internal errors to that generic response rather than serializing
Error context. A state reply acknowledges committed movement only.

Unknown versions/kinds/flags, malformed UUIDs, truncation/trailing bytes,
out-of-circle directions and zero sequence/epoch values reject. State vectors
must be finite, have positive zero, positions within +/-8192 m and velocity
magnitude within 256 m/s. No native handle, source data or durable entity ID is
serialized. Additive outer kinds do not claim negotiation with older peers.

The [M04a handoff](handoffs/M04a-authoritative-movement.md) records focused CPU,
actual mTLS and native collision verification. M04b's
[client contract](client-movement.md) connects bounded prediction/reconciliation,
timed remote interpolation and publication through this existing character owner.
M04c now supplies [authoritative combat](authoritative-combat.md), including a
trusted checked movement release that discards pending input on death and requires
a fresh stream on respawn. [M04d](gameplay-lifecycle.md) connects lifetimes/effects;
[M04e](handoffs/M04e-gameplay-acceptance.md) records passing connected adversity
and continuity acceptance. Full REQ-M04 PASS under the declared functional scope.
