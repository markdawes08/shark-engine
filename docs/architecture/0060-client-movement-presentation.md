# ADR 0060: client movement presentation

Status: accepted for M04b. Follow [ADR 0059](0059-authoritative-character-movement.md),
the [client contract](../client-movement.md) and [M04 checkpoints](../roadmap/authoritative-gameplay-plan.md).

## Decision

Add a CPU-only `net::MovementPredictor` alongside the existing replica cache.
It owns presentation values and at most eight unacknowledged directions. It has
no Scene, native collision, Lua or durable-operation access. Share M04a's nominal
tick/substep/speed limits in Core; the host supplies an explicit gravity model.
Prediction is approximate and collision differences require server corrections.

Prepare immutable exact input bytes before sending. One creating-thread `send`
preflights ownership/revision/history, admits those bytes to the existing TLS
FIFO, then publishes fixed-size history without another failure window. Retained
exact repeats return the original admission status without resending. The host
still owns the single shared session sequencer; input IDs and server ticks remain
separate. Generic rejection or an ambiguous host outcome suspends prediction,
retains exact history and requires explicit binding/session reconciliation.

Corrections validate stream/session/epoch, increasing completed ticks and bounded
acknowledgement progression before replaying only the retained input window.
Exact latest duplicates do nothing. Approximate replay cannot veto a valid server
correction: if it exceeds local bounds, accept the server state, retain remaining
inputs and suspend prediction. Reconciliation emits no gameplay/presentation event.

Extend the existing `ReplicaClient` with timed application and at most eight
complete history frames. The verified-server host supplies monotonic receive
times; replication protocol 1 has no server tick and does not gain one here.
Stage history and the committed cache together. Duplicates never retime anchors;
equal receive times coalesce, and new baselines/discard clear history.

Sampling returns owned display values with current live topology. Clamp outside
the retained interval without extrapolation. Use TRS interpolation with shortest
rotation only when decomposition and signed scales remain valid; legal shear,
reflection changes or invalid blends retain a complete committed-pose fallback.
Presentation samples cannot authorize picking or Scene mutation.

## Integration and compatibility

CharacterAuthority gains narrow publication/control forwarding methods using its
existing private SessionAuthority. Publication requires its durable Region world
before calling the existing public-field publisher; controls retain its bounded
quotas and shared exact-next session admission. No mutable session borrow, second
registry, worker or scheduler is exposed.

All APIs operate on CPU values. No source, database, prepared-content, authoring,
movement or replication wire version changes. Untimed replica application retains
its existing behavior and clears timed history on a new accepted update. Native
Editor/UI, graphical movement presentation and full connected combat/adversity
acceptance remain their explicit gates; M04c is next.
