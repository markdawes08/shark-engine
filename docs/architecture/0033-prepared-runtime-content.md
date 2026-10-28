# ADR 0033: Prepared runtime content and immutable assets

Date: 2026-10-27. Status: accepted for checkpoint 13b.
Follow the [runtime content contract](../prepared-content.md) and
[rendering checkpoints](../roadmap/scene-rendering-plan.md).

Keep the existing Tools glTF import/cache format unchanged. Its source reports,
diagnostics and import metadata do not belong in Runtime. Tools prepares a
separately versioned engine-owned mesh/scene representation; Core validates and
loads those explicit bytes without JSON, glTF/image parsers or Tools linkage.
These are CPU data/ownership services, with no renderer, device or file resolver.
They reuse Core identity, hashing, scene validation, transforms and errors rather
than introducing another scene model or dependency framework.

An immutable `PreparedAssets` snapshot owns validated meshes and supplies shared
const references. Cube and plane have reserved stable UUIDs and deterministic
geometry. Additional assets are decoded and checked completely before the snapshot
is returned. Duplicate/reserved IDs, mismatched identity and aggregate limit
violations reject the whole candidate. There is no mutable asset registry in this
checkpoint. Explicitly construct a new snapshot to replace content; existing
scenes/references retain their old values until released. Fixed snapshot budgets
do not pretend to bound all caller-retained historical snapshots or process RSS.

Prepared scene loading validates all components, hierarchy and mesh references,
including invisible renderers, before returning a fresh existing Core Scene owner.
Every load has a new incarnation; old entity handles cannot become valid again.
Global cell origin is retained separately from the scene's local transforms.
No region authority, session, replicated state, authoring history or source extras
is serialized. Failure cannot mutate a previously returned owner.

The temporary mesh profile preserves geometry attributes, base-color factor and
double-sided state for debug/unlit rendering. Texture images/samplers, PBR terms,
lighting and alpha-mode rendering are outside this prepared profile. The complete
original glTF cache remains available to later material/cooking owners; this does
not reduce assignment 12's import guarantees. Version changes require re-preparation,
not heuristic acceptance of another binary layout.

Both binary kinds have explicit little-endian fields, magic/version, bounded
counts and a SHA-256 body digest. Readers validate before publication and never
deserialize C++ object layouts. Mesh and scene formats version independently of
source JSON, gameplay wire and database schemas. This is development preparation,
not assignment 31's final cook or assignment 14's export/relocation workflow.
Application file selection and the connected Runtime launch remain in 13e.
