# Assignment 18: fixed-step physics checkpoints

Date: 2026-10-29. Base: `6fe897e`, initially clean `main`. Owner: REQ-18.
The agreed five-checkpoint split follows
[17d's headless preparation](../handoffs/17d-connected-viewport-acceptance.md).
Actual UI/GPU acceptance remains deferred under the user's
[desktop amendment](../backlog.md#desktop-ui-deferral); later coding is authorized.

| Checkpoint | Deliverable and acceptance |
| --- | --- |
| 18a: Jolt foundation | Pin the exact CPU dependency and notices; implement a private bounded empty physics-world owner with real native fixed-duration updates, owner-thread access and complete teardown. Define transform authority, parent/scale and timing policies before body APIs. Verify Windows/WSL, one sanitizer, protected closures and affected packaging/dependency tooling. |
| 18b: Bodies and colliders | Add static/dynamic/kinematic bodies and box/sphere/capsule shapes, checked entity lifetimes and bounded creation/removal. Enforce authority/root/scale rules. Connect actual component validation, source persistence, shared commands/history/retries, terminal discovery and FeatureWorld mappings alongside supported behavior. |
| 18c: Fixed-step synchronization | Attach simulation to existing schedulers; implement gravity, mode-specific scene/physics synchronization and separate render-pose interpolation. Keep server tick frequency and collision substeps explicit. Verify fixed-input results across render rates with documented tolerances. |
| 18d: Layers and triggers | Implement collision filtering, trigger transitions, bounded owner-delivered events and safe removal/teardown while events are queued. Workers never invoke Lua or mutate scenes. Queries and Lua bindings retain assignment 20. |
| 18e: Editor and connected acceptance | Expose implemented settings through the shared inspector/services, consolidate real FeatureWorld cases and human/terminal saved-project workflows, and qualify connected simulation/lifetimes. Actual UI/rendering follows desktop resumption; do not substitute headless checks for it. |

18a's [contract](../physics-world.md), [ADR 0045](../architecture/0045-physics-foundation.md)
and [handoff](../handoffs/18a-physics-foundation.md) distinguish the real native
foundation from unimplemented bodies/components. Full REQ-18 stays NOT_RUN until
its connected behavior and required acceptance are qualified. Every checkpoint
uses the [testing policy](../testing-policy.md); preserve workloads/fixtures and
reuse valid results rather than rerunning unrelated engine matrices.

EngineCore owns CPU physics, with Jolt types hidden. Server authoritatively owns
gameplay outcomes; editor authoring and render interpolation grant no player
authority. World/cell identity remains separate from bounded physics coordinates.
Initial rigid bodies are scene roots with positive uniform scale baked into shape
dimensions. Nonuniform/negative scale, parenting and local shear reject for these
bodies; ordinary render-only hierarchies retain their existing support. Static
world collision/streaming and origin transfer remain M05, and client prediction
does not assume bit-identical server physics.
