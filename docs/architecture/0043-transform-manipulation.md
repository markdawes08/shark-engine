# ADR 0043: Pure transform manipulation and one-commit drags

Date: 2026-10-29. Status: accepted. Owner: assignment 17b.

Keep manipulation math in a pure EngineTools value helper. Local and world
translate/rotate/scale use the existing double-precision affine and TRS rules;
world rotation and scale preserve the entity pivot and reject local shear rather
than approximating it. Cumulative input and snapping refer to the original pose.
Negative/nonuniform scale remains supported, with unchanged singularity bounds.
The [contract](../transform-manipulation.md) fixes axes, units and snap semantics.

The editor captures one entity/selection/revision and parent matrix at drag begin.
Every update uses the existing pure `scene.preview`; no per-frame writes or
history grouping mechanism are introduced. Commit sends one retained
`set_transform` through the same ProjectSession owner and shared history used by
the terminal and inspector. Cancellation discards values, never restores a stale
scene snapshot. A rejected latest update cannot commit an earlier valid candidate.
Selection/revision conflicts require a new gesture. Ambiguous delivery retains
the existing mutation reconciliation obligation; request correlation is not retry
identity.

This adds a public C++ value API and private editor adapter, with no dependency,
source/component/protocol version, new RPC, runtime importer or gameplay authority.
Terminal equivalence uses existing transform inspection, preview, retained
`set_transform`, history and persistence. No mutable Scene or private candidate
handle is exposed. Rebuild C++ consumers with the engine.

17c supplies gizmo UI and candidate rendering through these services; 17d supplies
connected acceptance. Headless math and owner tests cannot qualify native input,
rendering, focus or layout. Full REQ-17 remains NOT_RUN until its required gates,
including the explicitly deferred desktop checks, are satisfied.
