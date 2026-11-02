# Dynamic physics body controls

20a extends the existing [PhysicsWorld](physics-world.md) and
[fixed-step simulation](physics-simulation.md) with checked velocity, force and
impulse controls. These Core operations are called by the creating-thread owner;
they do not add a player command, Lua binding, authoring mutation or separate
scheduler. Physics owns runtime bodies and the server owns authoritative gameplay.
An Authored Scene can be used for an isolated trusted development preview; a Scene
scope alone is not proof of distributed authority.

## Admission and values

`velocity`, `set_velocity`, `apply_force` and `apply_impulse` require an open,
healthy world with its active original Scene writer lease and a live matching
body/entity generation. They recheck the committed body settings and pose. Only
dynamic bodies accept these controls. Static bodies use explicit static reset;
kinematics use the existing validated next-tick target. Foreign, stale, detached,
closed, faulted or wrong-thread operations reject. No native handles or borrowed
component pointers escape.

`PhysicsBodyVelocity` contains owned linear meters/second and angular
radians/second. Velocity vectors must be finite, fit normal native floats or
zero, and have magnitudes at most 256 m/s and 64 rad/s respectively. Impulse is
a center-of-mass linear impulse in N*s; force is a center-of-mass linear force
in N. Each has a magnitude limit of 1,000,000 and the same finite/float checks.
Vectors use the bounded physics Scene/world axes, not entity-local axes or global
cell positions. Center-of-mass force and impulse add no torque. The candidate
accumulated force also obeys that bound. Impulse and velocity
preflight the native float result against the speed limits; invalid input does not
mutate native state or replace earlier accepted force. Force and gravity applied
by native fixed integration remain subject to the existing Jolt speed ceilings.

## Ordering and lifecycle

Velocity assignment and impulse take effect immediately at the owner boundary;
impulse changes linear velocity according to body mass. Neither operation clears
an accepted pending force. The Scene pose changes
only through the existing fixed-step publication path. Multiple accepted forces
add for the next fixed tick, independent of collision substep count. The owner
applies their sum once after existing pre-native admission and contact-event
capacity checks. A pre-native admission rejection preserves pending force. A successful fixed
step consumes it; explicit dynamic teleport, end simulation or body/world
teardown discards it. A rejected force does not discard an earlier accepted force.
Teleport retains its existing velocity reset and interpolation discontinuity.

Pre-native validation failure leaves native state unchanged. A native failure or
failure after native work faults the owner and preserves the last fully committed
Scene; it does not promise native rollback or authorize blind replay. The
[contact](physics-contacts.md) backpressure and event lifetime policies remain in
force. Physics queries, Lua callbacks/bindings and connected human/terminal
inspection belong to the remaining [assignment 20 checkpoints](roadmap/physics-interaction-plan.md).
