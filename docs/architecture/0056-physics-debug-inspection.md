# ADR 0056: Shared physics inspection and debug overlay

Date: 2026-11-01. Status: accepted for checkpoint 20d.

Extend the existing retained `scene.physics` owner with bounded, read-only body and spatial-query observations. Fence every new observation by preview session, completed tick and next operation sequence: removal can change a body without advancing a tick. Return owned entity identities, committed poses, filters and complete query hits without native handles. Enforce existing query limits and checked ignore identity; a query is an observation and cannot grant later write authority. Continue to use the original PhysicsWorld, Scene lease and owner thread.

The Editor uses the same inspection service to show committed box, sphere and capsule wireframes and the last requested query in the existing image rectangle. Project through the displayed RenderScene camera and origin with homogeneous frustum clipping. Draw an opt-in display-only overlay over the scene image; it is not depth tested, is not a picking surface, and may differ from interpolated render poses. Keep bounded geometry and no native Jolt renderer or separate simulation. Invalidate query observations when the session, tick or operation sequence changes.

These are additive local-authoring protocol-version-1 observations and Editor controls. Source, cooked, database and player-wire formats do not change. Existing scene-entity queries remain compatible. Full REQ-20 and connected assignment acceptance remain with [20e](../roadmap/physics-interaction-plan.md).
