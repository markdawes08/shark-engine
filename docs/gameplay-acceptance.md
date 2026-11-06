# Authoritative gameplay acceptance

M04e connects the [movement](authoritative-movement.md),
[prediction/interpolation](client-movement.md), [combat](authoritative-combat.md)
and [lifecycle](gameplay-lifecycle.md) owners in separate headless processes.
Follow the [M04 checkpoints](roadmap/authoritative-gameplay-plan.md),
[current-machine scope](architecture/0010-current-machine-scope.md) and
[testing policy](testing-policy.md). Results belong in the M04e handoff;
registration alone is not a pass.

## Connected fixture

`GameplayProbe` links only the server/Core composition. The existing process
runner owns one server and two authenticated CPU clients per scenario; the
Python supervisor runs scenarios serially and checks their combined results.
All player messages cross actual mutually authenticated loopback TLS. Accounts,
enrollment and characters use an isolated real Store. Harness files coordinate
startup and phase barriers; they do not substitute for gameplay messages.

The server uses the existing 20 Hz ServerScheduler, one retained Region Scene
and PhysicsWorld with three substeps. A client predicts into a static wall before
the trusted host explicitly teleports the duel into its combat layout. This
setup operation uses the physics owner and does not authorize client teleports.
Current-pose melee remains 25 damage, ten-tick minimum strike cooldown, 2.5 m
range and native line of sight, with zero historical rewind. A confirmed
interaction grants no economic effect. Four strikes cause one death; respawn
retains the original 60-tick deadline and rotates ephemeral life/body bindings.

Both clients consume actual combat/movement values and selected replica updates.
Prediction and presentation are observations. Replica ACKs share the session
sequencer with player intents. Replica updates and ACKs stay on reliable TLS;
the observation shaper affects combat states and their embedded movement values.
Replayed attacks must reject without consuming
another shared sequence or granting an effect. Death, respawn, client pause,
disconnect and fresh-session reconnect must leave the independent world and
healthy recipient advancing.

## Fault profile and observations

| Scenario | Reliable intent delay | Verified observation faults |
| --- | --- | --- |
| Baseline | None | None |
| `rtt100-loss1` | 50 ms | 50 ms plus seeded 0..30 ms jitter, 1% loss, duplicates and reorder |
| `rtt200-loss5` | 100 ms | 100 ms plus seeded 0..30 ms jitter, 5% loss, duplicates and reorder |

These nominal 100/200 ms request/reply models operate at the application layer.
Reliable sequence-bearing requests retain FIFO order; authenticated observation
bytes pass through the existing bounded FaultLink. TLS/TCP bytes are not lost or
reordered. The declared profile is not a measurement of Internet or transport RTT.
The 1% profile uses seed 1 and the 5% profile seed 1337; duplication is 50%, reorder
selection 30% and extra reorder delay 1,500 ms, matching the existing semantic
[network profile](network-qualification.md). Require observed losses, duplicates
and delivered inversions, not just configured probabilities.

Each run completes 320 native ticks. Ticks 1..319 use the selected observation
faults, followed by an explicit healthy final reconciliation over real TLS.
One canonical combat state per recipient/tick is preserved. Final authoritative
health, life and poses must agree across clients and server. Stale/duplicate
observations retain their original fences and cannot replay presentation effects.
Record prediction/replica history peaks, declared queue bounds, correction-distance
and semantic delivery-delay distributions for accepted observations. Delivery delay
measures verified receipt to client application; it excludes network time before
receipt and observations rejected as stale. The observation queue is capped at
64 packets/64 KiB and drains at most 32 copies per call; delayed reliable requests
are capped at 64 per connection. Transport retains its 32-frame/64 KiB defaults.
Measure wall prediction correction before the trusted combat-layout teleport;
that later setup correction cannot qualify the wall case.

## Acceptance ownership

The connected case supplements M04a-d's independent expected-value, malformed
wire, speed/impossible-move, stale/foreign authority, sequence, thread, native
fault, script lifetime and client-cache cases. Their original durable-operation
fixture establishes exact original-ID reconciliation after lost completion;
movement correction has no economic callback or reward path. Reuse unchanged
results under the testing policy rather than rebuilding an economic API here.

Headless polling pause exercises client independence without manual desktop
interaction. The unchanged [04c native evidence](handoffs/04c-frame-loop.md)
separately covers actual focus/iconify behavior with advancing server time; it is
reused rather than rerun during the manual UI pause. Graphical smoothing, the
complete LOCAL-MMO-1 population, region travel and endurance retain their later
owners. The two-client fixture does not
claim WAN or production capacity. Full REQ-M04 closes only after the connected
case and applicable affected-subsystem evidence pass.
