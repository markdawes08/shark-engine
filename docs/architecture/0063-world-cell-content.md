# ADR 0063: bounded static world-cell content

Date: 2026-11-06. Status: accepted for M05a. Extends
[ADR 0007](0007-mmorpg-architecture.md), [ADR 0004](0004-formats-and-identity.md)
and [prepared content](../prepared-content.md). Follow the
[M05 checkpoints](../roadmap/seamless-world-plan.md) and [cell contract](../world-cells.md).

## Decision

Keep WorldPosition's canonical 1024-meter integer cells and local doubles. Add
bounded relative conversion by subtracting integer cells before combining local
offsets. Float packing carries a nonzero caller-owned origin generation; unpacking
rejects a different generation. Global coordinates never become float vectors.
These pure operations do not move a Scene or choose an origin automatically.

Introduce independently versioned `SHKCELL1` development content with WorldId,
cell coordinates, positive content revision and one complete existing prepared
Scene. Its anchor must exactly match the cell's zero-offset corner. The initial
profile contains static hierarchies/meshes and optional static non-trigger body
settings; cameras, scripts and moving bodies reject. Revision labels are trusted
content metadata, not authority epochs or freshness attestations.

EngineTools captures an exact existing AuthoredScene revision and uses the Core
encoder. It neither rewrites the anchor nor changes source/history. Core loads a
complete candidate through LoadedPreparedScene, resolving all mesh references,
then returns immutable content ownership with a fresh private Scene incarnation.
Replacement is explicit and failed candidates cannot alter a retained cell.

## Compatibility and consequences

This is an additive public API and a new envelope. Existing WorldPosition,
source-scene, prepared-scene/mesh/startup and wire versions/bytes remain unchanged.
Existing readers need no migration; cell readers reject unknown cell versions.
SHA-256 detects payload corruption and supplies no authenticity guarantee.

One cell uses existing scene/entity/asset bounds. These are per-candidate logical
bounds, not aggregate process memory or a resident-world capacity claim. M05b owns
bounded resident inventory and asynchronous IO; M05c owns native static collision,
navigation and readiness. Loading content grants no gameplay, region, storage or
player authority and performs no native physics activation. Final cooked manifests
remain with 31/M08. No new dependency, scheduler or importer enters Core/Server.
