# ADR 0053: Checked dynamic physics controls

Date: 2026-11-01. Status: accepted for checkpoint 20a.

Extend the existing owner-thread PhysicsWorld and Scene lease with checked
velocity, center-of-mass force and impulse operations. Restrict them to live
simulated dynamic bodies; retain static and kinematic pose ownership policies.
Validate finite bounded native values before mutation. Use immediate velocity and
impulse boundaries and one bounded per-body accumulated force for the next fixed
tick, cleared on successful step, teleport, end and teardown. Preserve event
backpressure, explicit partial-native faulting and unchanged Scene/source history.

This is a Core capability for trusted simulation owners, not an authenticated
player protocol or a grant of gameplay authority. Existing native speed ceilings
remain in force. Queries, script bindings, contact delivery and debug presentation
remain later [checkpoints](../roadmap/physics-interaction-plan.md).
