# Assignment 20: physics interactions and Lua checkpoints

Base: `c33265a`, initially clean `main`. REQ-20 remains NOT_RUN until
connected assignment acceptance. Verification follows the
[testing policy](../testing-policy.md) and reuses valid prior evidence.

| Checkpoint | Scope |
| --- | --- |
| 20a | Checked body velocity, force and impulse controls through the existing PhysicsWorld and Scene lease; preserve teleport and kinematic target semantics and server authority. |
| 20b | Bounded filtered raycast and overlap queries with owned results and checked identity. |
| 20c | Checked Lua physics APIs and owner-thread contact delivery with bounded deferred body changes; never invoke scripts in native callbacks. |
| 20d | Debug visualization and shared human/terminal inspection of physics objects and queries. Implementation and focused CPU checks complete; native interactive overlay evidence is NOT_RUN as recorded in the [handoff](../handoffs/20d-physics-inspection.md). |
| 20e | Connected FeatureWorld and affected subsystem acceptance, including filtering, callback lifecycle, queued changes, pause, teardown and authority. |

Each increment must preserve the [physics simulation](../physics-simulation.md),
[contacts](../physics-contacts.md) and [script runtime](../script-runtime.md)
contracts. No player gains arbitrary physics or Lua authority. The server owns
gameplay; client presentation and local editor preview remain separately scoped.
M04/M05 retain network corrections and cross-region integration.
