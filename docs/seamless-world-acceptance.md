# Seamless-world acceptance

M05g connects [world-cell authoring](world-cell-authoring.md),
[residency](world-cell-residency.md), [traversal](world-traversal.md) and
[process execution](region-process-execution.md) under
[ADR 0069](architecture/0069-connected-world-streaming.md). Follow the
[M05 plan](roadmap/seamless-world-plan.md),
[current-machine scope](architecture/0010-current-machine-scope.md) and
[testing policy](testing-policy.md). Results belong in the M05g handoff and
coverage manifest. Registration and this procedure do not establish a pass.

## Runtime ownership

RegionGateway retains actual mTLS connections, the sole session/character
authority, original durable requests and recipient interest/replica streams.
RegionProcess controls actual headless `Server --region-worker` children through
closed owned pipes. Children have no player listener, Store or Tools dependency
and never tick autonomously. All interacting players and NPCs move together to a
fresh executor; stationary player writers participate in the same atomic SQL CAS.

The executor's catalog contains at most eight immutable key/revision/proxy rows.
Optional absolute file sources bind exact byte counts and SHA-256 digests and use
the existing bounded Core reader. Portable image v2 carries catalog source/state,
residency limits and pursuit values; the decoder retains v1's original inline,
active-cell defaults. Cell payload v1/v2 decoding remains independent of the
portable image version. Assets still come from the explicit immutable prepared
closure; runtime file loading performs no source import or implicit discovery.

Prefetch uses the retained asynchronous residency owner. Requested native ticks
continue while a read is queued/running/ready. Explicit drain publishes at most
one ready job; activation publishes a complete traversal candidate and checked
native mesh. Cancellation and terminal take keep their original
owner semantics. RegionCellTicket also binds the executor instance, so delayed
old handles reject after transfer/recovery. File failure, cancellation or resident
pressure cannot detach the last-valid active terrain.

The fixed gameplay hierarchy admits only the catalog's validated native collision
proxies as streamed topology changes. Detach checks current contacts, dynamic
footprints, pending targets and retained life spawn positions. A collision proxy
has no authored RigidBody. Readers retain owned bytes and never access Scene.
Normal checkpoints describe committed residency and active content, not pending
reader work. Transfer freeze rejects until every retained job has settled and
been taken; a new child constructs its own reader, pins, bodies and navigation.

Pursuit binds a current NPC body and exact target life. Current native support
must agree with authored walkable geometry through bounded vertical projection.
The current profile uses dynamic spheres and the existing single-layer centerline
navigation, without a clearance bake. Steering remains a candidate: the owner
checks native committed displacement/support before advancing pursuit counters.
Readiness, blocking, stop, cancellation and unavailable-life states are explicit.
An accepted offer retains its canonical world steering anchor. Fresh paths use
that adopted point only while current native support remains within the existing
0.02 m horizontal and 0.04 m support agreement bounds and the current graph still
accepts it. Otherwise the owner discards the anchor and starts from current native
support. This preserves an already checked portal crossing when float integration
finishes fractionally short; it changes no navigation tolerance or native pose.
Paths are rebuilt from current tiles. The value-only anchor survives a handoff,
while old navigation IDs/cursors confer no authority in a destination.

## Frames and publication

A handoff may stage a cell-aligned frame with the same world/offset and exactly
the next frame generation. The pure candidate check precedes source freeze and
SQL. Local positions, native float values, mesh vertices, poses, previous poses,
targets and gameplay state must retain their represented global positions
exactly. Unrepresentable candidates reject before authority changes. Each actual
executor keeps one fixed frame throughout its lifetime; Jolt's internal solver
caches are reconstructed under the existing portable-state profile.

The gateway keeps its original client frame for public poses, interest selection,
replicas and embedded movement corrections. Player session, movement stream, life
and replica identities survive the server-frame change. Movement and replication
wire v1 remain unchanged; existing combat wire compatibility remains unchanged.
Canonical complete tick/admission/control state is journaled before public
publication. A failed post-effect capture/publication requires explicit recovery;
an incomplete publication cannot authorize a new canonical observation.

The same two-slot bounded journal and exact all-writer CAS remain in force.
Timeout or a lost control reply does not activate a second executor. Recovery
confirms actual owned child exit, resolves the original SQL ticket, then stages a
fresh incarnation. Original uncertain admitted inputs retain their receipts and
sequence numbers; only routes beyond the durable watermark replay internally.
Storage uncertainty still requires the existing durable-owner reconciliation.
Gateway/session cold restoration and physical-host failover are outside this
profile.

## Connected fixture and procedure

`SeamlessWorldTests` links Tools only in the test host. Its shared fixture writes
and reopens a real project and three source scenes, exports native cell files
through the existing authoring/ProjectFiles APIs, and checks that source bytes,
dirty state and history stay unchanged. M05d's public terminal-process coverage
supplies the unchanged wire adapter evidence; the acceptance fixture adds no
second authoring service. Native World cells panel interaction remains NOT_RUN
under [BL-M05D-UI-01](backlog.md#deferred-items).

Run the same narrow terrain at global cell X=0 and X=1,000,000,000. Independently
inspect the encoded cell v2 fields, body digest and literal 1/1024 m grid geometry:
the first strip spans local X=1020..1024 and Z=3..7, with the next strips spanning
X=0..4 and the same Z range. Collision and walkable triangles come from explicit
authored mesh selections. Do not validate expected geometry solely by decoding
the encoder's own output. The logical region edge is X=1023.5, separately from
the content seam at X=1024.

Execute these connected observations with controlled gameplay clocks and bounded
wall watchdogs:

1. Start with only the first cell active. Admit actual player input through
   mutually authenticated TLS and the existing session sequencer. Show that a
   cold neighboring footprint consumes input as a horizontal stop while the
   world, healthy peer and reader continue to progress.
2. Exercise a missing native file, corrupt bytes, cancellation and explicit
   repaired retry. Preserve original terminal outcomes until take and keep the
   first native terrain generation valid. Retain managed scratch on failure.
3. Reserve the unused third cell's read and reject a second pending reservation
   before IO. Load that third cell into the second resident slot. A valid target
   load must fail resident pressure without replacement. Explicitly evict the
   inactive unused cell, retry, activate, safely deactivate and reactivate the
   target.
4. Cross the distinct region and content boundaries with native player movement,
   exact-life NPC pursuit and a native checked projectile. Transfer during live
   gameplay, preserving flight/rule state, life, FIFO/input progress and original
   durable ownership semantics. Keep independent expected health/effect counts.
5. Stage a real +1024 m destination frame and repeat a return handoff. Compare
   represented global/native positions before/after each cut, client-frame
   corrections and stable recipient replica/session identities. Compare the
   near/far workload's local results without forming huge absolute float values.
6. Deliver canonical combat observations over actual TLS, then apply bounded
   seeded FaultLink delay, loss, duplication, reorder and partition to the
   verified application observations. Exercise client stale/gap handling and
   explicit final healthy reconciliation. Require observed fault counters and
   delivery inversions where applicable, rather than just configured rates.
7. Add lost combat-admission and completed-step reply cases. Confirm hard child
   exit before replacement, exact original admission replay and last-durable-cut
   recovery without a second gameplay or durable effect. Reuse M05f's unchanged
   real source/destination crash/partition matrix for the remaining phases.

Player requests retain reliable sequence ordering. FaultLink shapes verified
messages after authenticated receipt; it does not inject faults into raw TLS/TCP
bytes or measure Internet RTT. The current semantic fixture uses seed 1337,
100 ms delay, 0..50 ms jitter, 10% loss, 30% duplication, 40% reorder selection
with 75 ms extra delay, and an explicit 150 ms partition. The movement predictor
first applies each reliable correction; only the verified combat observation
enters FaultLink. Keep reliable replica traffic and its actual ACKs
on their existing transport path. Observe one canonical state per recipient/tick
and preserve the distinction between client presentation and native authority.

## Budgets and evidence

The authored fixture fixes a two-cell resident limit within the runtime's
eight-row catalog bound. Its pending cap is exactly
`world_cell_maximum_payload_bytes + PreparedAssets::retained_bytes()`, allowing
one complete worst-case pending reservation. A second pending request rejects
before reader IO; retained terminal slots still follow the existing four-job
limit and explicit take protocol. Record resident/pending bytes, their logical
high-water marks and retained job counts. These are conservative owner charges,
not RSS.
Separately sample live source/destination child working-set on Windows and VmRSS
from `/proc` in WSL against the fixture's 1 GiB per-child functional budget.
Report the largest observed sample and sample count; this is not a process-lifetime
peak or a production memory-capacity claim.

Record handoff quiescence and the provisional 500 ms overrun flag. Source freeze,
native reconstruction and durable publication can exceed that target on the
available PC. Report any overrun honestly; it cannot bypass exit confirmation,
writer CAS, publication durability or routing fences. Timing results establish
only the measured local functional profile.

Select the full affected CPU subsystem inventory for M05 closure, the connected
cases and applicable WSL/sanitizer selections under the testing policy. Reuse
qualified unchanged M05a-f results where the final tested inputs remain valid.
Keep failures and exact limitations in the handoff, with PASS, FAIL and NOT_RUN
distinct. No engine-wide rerun, GPU or manual UI gate is implied by this CPU
composition alone. Manual UI remains paused; native World cells interaction stays
NOT_RUN under [BL-M05D-UI-01](backlog.md#deferred-items), independently of this CPU
gate. Later cooked distribution,
independent concurrent islands and LOCAL-MMO-1 load/endurance retain their owners.
