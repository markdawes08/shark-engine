# ADR 0059: authoritative character movement

Status: accepted for M04a. Follow [ADR 0007](0007-mmorpg-architecture.md) and the
[movement contract](../authoritative-movement.md).

## Decision

Add a creating-thread `server::CharacterMovement` host adapter to the existing
server-only EngineStorage composition. It owns bounded input values and checked
identities, borrowing the existing CharacterAuthority, Scene, PhysicsWorld and
actual transports only during calls. It introduces no session registry, database
worker, scheduler, source importer or native controller dependency.

CharacterAuthority gains a nonmutating, nonblocking `authorize_entity` read of
its actual mTLS session and cached durable character/entity mapping. It spends
no command sequence and refreshes no lease. Input admission then uses the existing
exact-next Heartbeat admission, sharing session command/rate budgets with other
operations. The retained snapshot itself cannot authorize later native work.

Use 20 Hz ticks, three collision substeps and root dynamic spheres of radius
0.5 m, mass 1 kg, unit scale and zero friction/restitution. Client input contains
only integer X/Z directions inside a unit circle. Server speed is 6 m/s; each
accepted input lasts one tick. Missing input stops horizontal movement. Preserve
ordinary vertical motion with a 250 m/s ceiling that leaves native packing
headroom below the existing 256 m/s total-vector bound.

Before native work recheck live transports, readiness, account/character/session
epochs and exact entity/body generations. Fence stale actors and discard their
pending inputs. Existing physics controls own velocity and committed transforms;
returned contact observations go to the host's established ordered dispatch.

## Outcomes and compatibility

Movement stream IDs are fresh per binding. Session command sequence, movement
input sequence and authoritative completed tick remain separate, nonwrapping
domains. Up to 32 characters each retain eight inputs; at most one input per
character is consumed per tick. Local FIFO admission is not a completed movement
acknowledgement. Only a successful native tick produces a correction containing
the committed pose, velocity and last applied input.

Errors after velocity writes permanently fault this adapter, including a native
completion whose global-position conversion or observation fails. Do not replay
an uncertain tick. Keep the existing physics fault/teardown semantics and obtain
fresh explicit owners when reconciling.

Outer wire version 1 gains authenticated client input kind 9 and server state
kind 10, following the additive session/replication convention. Their independent
movement protocol 1 uses exact bounded payloads and generic public rejection.
Older peers reject unknown kinds; no mixed-version negotiation is claimed.
No source, prepared-content, database or authoring protocol version changes.

M04b adds prediction and remote presentation; M04c chooses combat rules; M04d
connects durable effects and reconnect lifecycle. M05 retains terrain, streaming
and seamless authority transfer. These are separate acceptance obligations.
