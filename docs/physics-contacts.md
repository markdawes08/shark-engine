# Collision filters, triggers and contact events

18d extends [bodies](physics-bodies.md) and [fixed-step simulation](physics-simulation.md)
under [ADR 0048](architecture/0048-physics-contacts.md). All operations are CPU-only
and use the existing creating-thread PhysicsWorld and leased Core Scene. They
grant no player, client-render or durable economy authority.

## Shared settings and compatibility

RigidBody adds three optional source/terminal fields, with stable property IDs:

| Field / ID | Type, range and default |
| --- | --- |
| collision_layer / 21 | uint32 in [0,15], default 0 |
| collision_mask / 22 | uint32 in [0,65535], default 65535 |
| is_trigger / 23 | Boolean, default false |

A pair is enabled only when both masks include the other body's layer. Mask zero
disables all its pairs; triggers obey the same filters. Layer numbers are local
physics categories, unrelated to world/cell IDs, region authority or render layers.
Private native object-layer indices are never these public IDs. Rules are fixed
for each native body; end/edit/rebuild/begin changes settings without mutating a
filter during native work. Roots, positive uniform scale and replica rejection
remain unchanged.

Shared create/set/remove, capture/history reconstruction and exact retained input
comparison include every field. Source scene/component schema 1 accepts omitted
new fields with the defaults above; current saves explicitly write them. Unknown
fields/types and out-of-range integers reject. Older strict readers reject these
new fields rather than dropping collision policy. The public catalog remains five
components with 23 properties. Existing eight-field terminal body values remain
accepted; discovery and selected queries expose all eleven fields. No new RPC is
needed, and the authoring protocol is not a player protocol.

Prepared scene version 3 appends two uint32 values and a strict Boolean
byte after each body's eight float64 values. Version 1 has no bodies; version 2
decodes filters to defaults. Encoding body-free data still emits version 1; bodies
with default filters emit unchanged version 2; any nondefault new field selects
version 3 for all body records. Unsupported versions reject, and older readers
reject version 3 explicitly. Bounds, digests, complete candidate validation and
Core-only loading remain. No native contact/handle/event queue is serialized.

## Native behavior and sampling

Native sensors detect contacts without applying collision response. Static,
dynamic and kinematic sensors use the same body constraints. Kinematic/non-dynamic
contact detection is enabled. A static/static pair has no moving participant and
produces no contact observation. Discrete motion remains the profile; a fast body
can miss a thin trigger between collision evaluations. 18d added no queries or CCD; [20b queries](physics-queries.md) observe current
shapes, while CCD remains outside that scope.

Sleeping is disabled for this initial profile. This avoids native sleep removal
callbacks becoming false exits while a body remains inside a trigger. This is a
functional bounded CPU policy, not a population performance or MMO capacity claim.

The native callback collector records unique body pairs across collision substeps
in fixed preallocated storage. It reads only native body values (private ID, slot
and engine generation). It never allocates, throws, calls application/Lua code,
locks a body or accesses/mutates Scene. It assumes the existing single-threaded
native job profile; introducing concurrent native jobs must revisit this collector.

After Update returns, the owner verifies current engine/native incarnations and
filters against the final native contact cache. It sorts by engine body slot/
generation and compares with the prior committed pair set. One enter or exit is
published per changed pair, regardless of duplicate substep contacts. There is no
per-tick stay event. Contacts that begin and end wholly between these fixed
observations are not reported. Native collision evaluation precedes final velocity
integration: separation caused by a final movement can appear on the next tick.
These are native contact observations, not geometry queries of final Scene poses.

Scene pose candidates and complete event values are prepared before publication.
Only a successful native update and Scene commit publish transitions. A failure
after native work preserves the last complete Scene and earlier queued events,
faults the world and rejects further simulation; native progress is not rollback.
Contact listeners add no native temporary allocation path. Existing conservative
arena admission, discrete primitives and absence of step listeners still apply.

## Capacity and delivery

| PhysicsWorldConfig field | Bounds and default |
| --- | --- |
| max_contact_pairs | 1..16384 unique observed pairs/update, default 1024 |
| max_queued_events | 2*max_contact_pairs..65536, default 16384 |

Observation capacity counts unique transient pairs too, even if absent from the
final cache. Callback overflow is recorded without growing storage; the completed
native work then faults without publishing a partial tick or new events.

Each step reserves room for twice the pair capacity before any native movement,
covering all old exits plus all new enters. Insufficient space or exhausted event
sequence IDs returns conflict at physics.events.capacity, with no tick, target,
Scene or native change and no world fault. Take events before retrying that direct
step. Existing schedulers still terminally fault on a failed callback and retain
their unexecuted debt; size capacity/consume events for the chosen catch-up budget
or use the existing per-tick callback to take them. Do not blindly restart a server
schedule or drop debt. Default storage can reserve eight worst-case default ticks.

`take_events(maximum=256)` requires the creating thread, an open world and a bound
of 1..65536. It allocates the returned owned vector before consuming a FIFO prefix;
allocation failure preserves the queue. It also remains available on a faulted
world to collect earlier committed observations. No implicit dispatch or callback
acknowledgement is hidden in stats/reads. Stats add active_contact_pairs,
pending_events and discarded_events.

Each PhysicsContactEvent carries a nonwrapping sequence, completed tick, enter/
exit phase, simulation/body_removed cause and two participants. Each participant
owns its PhysicsBodyHandle, scoped EntityReference and trigger flag. Ordering
within each update/removal batch follows canonical engine pair order; sequence
orders the FIFO across ticks and removal operations. Both trigger flags remain visible for trigger/trigger pairs.
Values are historical observations: resolve current authority/entity/body
generations again before applying a later action. Do not use events as mutation,
durability or replay tokens. No native IDs, pointers or borrowed component values
leave the owner.

## Removal and teardown

`remove_body(scene, token)` is an explicit runtime boundary through the same leased
owner. It prevalidates the Scene reference/revision and complete exit event space,
then removes/destroys the native body, removes RigidBody from the Core Scene in one
revision and keeps the visual entity/children. It invalidates the body generation,
removes active pairs and queues body_removed exits at the current completed tick.
Earlier queued events retain their original participant values. Preparation/
backpressure rejection changes nothing; unexpected failure after native removal
faults the world rather than authorizing retry. This cannot mutate AuthoredScene's
const borrow, source history or saved files.

The original `remove_body(token)` remains detached-only and works after source
Scene destruction. It uses the same exit/lifetime path. The later
[Lua contact contract](physics-lua.md) adds bounded owner-applied callback
mutations and checked self-body recreation; it does not add entity spawning or
authorize arbitrary changes to the original authored Scene.

Close detaches the native listener before body/system teardown and invokes no
application callbacks. It clears contacts, accounts for unconsumed queued events
in discarded_events, and releases the weak Scene write lease. Previously taken
values survive entity/Scene/world destruction. Close is not delivery; callers
requiring the final observations must explicitly take them first.

Native inspector fields and connected acceptance were completed in 18e under the
current-machine scope. [Lua contact delivery](physics-lua.md) uses the owner after
native updates; streaming/rebasing retains M05.
