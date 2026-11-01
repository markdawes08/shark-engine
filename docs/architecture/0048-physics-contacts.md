# ADR 0048: Collision filters and bounded contact observations

Date: 2026-10-31. Status: accepted. Owner: 18d.

Append collision_layer, collision_mask and is_trigger to the existing RigidBody,
shared schemas, source, history, exact retained matching and terminal service.
Use 16 engine-owned layers with bilateral masks. Private native object layers
index immutable per-body rules; native sensors implement trigger response.
Keep one Scene/physics owner and the existing root/scale/authority restrictions.

Sample body-pair contact state from each successful native fixed update, not
application callbacks or render frames. A preallocated collector records unique
Added/Persisted pairs without allocating, throwing, locking bodies, accessing
Scene or calling user/Lua code. After native completion, recheck engine generations
and the native contact cache, compare canonical pair sets, and stage owned enter/
exit events before one Scene/event publication. No contact Removed callback reads
dead bodies. Contacts wholly between fixed observations are outside this discrete
profile; this is not CCD or a final-position intersection query.

Disable sleeping in this initial bounded profile so sleeping cannot masquerade
as a trigger exit. Native collision evaluation precedes final position integration;
a movement's separation can therefore be reported on the next fixed tick. Keep
tick and collision-substep policies independent. Contact listeners add no native
temporary allocation path; step listeners remain absent and arena admission stays.

Bound observation pairs and queued owned events independently. Reserve worst-case
transition space before native movement; backpressure is a nonfaulting rejection.
Post-native observation overflow faults the owner without partial Scene/events.
Explicit take returns owned values; close accounts for discarded queued values.
Active runtime removal detaches RigidBody through the existing leased Scene owner,
invalidates its native handle and queues exits while preserving earlier events.
Keep copied entity/body generations, never mutable pointers or native IDs.

Source/component schema 1 gains optional fields with legacy defaults; strict old
readers reject the new fields rather than silently discarding filters. Existing
eight-field terminal values still materialize those defaults. Prepared scene
version 3 stores nondefault filters/triggers, with tested version-1/2 decoding and
unchanged legacy encoding when applicable. No player-wire, terminal method or Lua
API is added. Details belong in the [contact contract](../physics-contacts.md).
