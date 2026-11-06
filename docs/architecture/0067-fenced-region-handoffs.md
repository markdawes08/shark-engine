# ADR 0067: consistent regional cuts and fenced local handoffs

Date: 2026-11-06. Status: accepted for M05e. Extends
[ADR 0066](0066-colocated-regions.md). Follow the
[regional contract](../region-world.md), [durable ownership contract](../regional-ownership.md),
[M05 checkpoints](../roadmap/seamless-world-plan.md) and
[testing policy](../testing-policy.md).

## Decision

Add an explicit persistent-ownership mode to the existing co-located RegionWorld.
The default preserves M05d's functional grant mode. Persistent binding requires
the actual CharacterAuthority's cached durable claim for the exact character,
entity and source region. A RegionWriter identifies Store, character, entity,
region, region instance and monotonically increasing ownership epoch. Session
ownership epochs, child grants and execution permits remain separate identities.

Keep one native/gameplay island while transferring a player's regional writer.
Prepare reserves one bounded handoff record without stopping ticks. Crossing a
geometric edge does not commit ownership: the source writer may continue within
its rectangle expanded by the declared overlap, intersected with the world union.
Normal movement outside that corridor consumes the input as a horizontal stop.

Cut checks the latest committed destination pose and all attached actual sessions,
then freezes the entire island before capturing final state. Stopping only the
transferring player's inputs would still allow gravity, contacts, projectiles,
target damage and respawn to change its cut. Movement/combat share one checked
ExecutionFence; NPC native writes and all regional ingress obey the same freeze.
Frozen or stale permits reject before another mutation or session command.

Capture owned movement FIFO/watermarks, combat/lifecycle/action history, global
ordering, complete original projectile flights, NPC native poses/velocities and
pending intents, current session command sequences, original durable requests and
routing, regional binding shadows, counters, frame and exact traversal generations.
SHA256 covers explicit canonical values, including settled worker values/errors
and original save payloads. Concurrent ownership-control work rejects the cut.

Retain the exact native, Lua, session, interest and worker owners. The observation
is not an installable native/VM image; reloading source or rebinding actors would
lose mutable rules, queues, contact history and protocol identities. The latest
cut identity, digest and watermarks are persisted with the ownership CAS; those
SQL receipts survive owner recovery even though the in-memory cut does not.

Submit commit through the existing bounded worker. SQL compares the original
writer inside the same transaction that publishes the next writer and exact last
transfer. Character checkpoints and economic writes independently validate their
captured writer at the transaction boundary. A cached admission is not permission
for a delayed stale job to mutate after the ownership change.

Only an exact known committed receipt activates the new local writer. Rotate the
execution permit, privately adopt it on the retained lower owners, update the
movement corridor and publish a fresh child grant without bind/reconnect/reset.
The client session, native bodies, gameplay/replica streams and original requests
continue. Abort is allowed before dispatch or after a known noncommitted rejection;
unknown commit results never authorize source resumption.

## Bounds and recovery

Allow one active handoff, eight retained records and the existing sixteen worker
slots. A cut's canonical encoding is at most 4 MiB; structural actor/queue/result
limits separately bound its owned values and duplicate lower-owner byte blobs.
This is not an exact heap/RSS claim. Terminal records release their internal cut
pointer. Explicit release acknowledges only committed/aborted receipts; forgotten
IDs become unknown, and trusted hosts must use fresh transfer IDs for new intents.

Expose a 500 ms local quiescence budget, cut/resumption times and an exceeded flag.
An overdue or uncertain commit remains frozen and requires reconciliation. A known
rejection stays frozen in its rejected phase until safe abort, with the budget
still counting. A late
exact commit can activate the sole current writer while retaining its exceeded
flag. Deadlines neither cancel dispatched SQL nor roll ownership back. Preserve
the original transfer ID/request through lost replies and explicit maintenance
recovery; reconcile against the cached exact last-transfer receipt.

M05f must implement separate-process routing, native reconstruction and portable
state/import semantics, including supported Lua state and cross-owner interaction.
M05g retains connected acceptance. No distributed lease, restartable simulation
snapshot, general Lua serialization or production availability is established by
this local handoff. Persistent schema compatibility belongs to the durable
ownership contract; source/prepared content and player wire formats stay separate.
