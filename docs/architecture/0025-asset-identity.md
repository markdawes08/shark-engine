# ADR 0025: Persistent asset metadata and derived catalog

Date: 2026-10-25. Status: accepted for checkpoint 11a; import/cache/jobs follow
11b/11c. This implements the asset-metadata row of [ADR 0004](0004-formats-and-identity.md)
without changing project/scene source schemas or the final cooked format.

Use an EngineTools `AssetId` domain over Core's existing validated UUIDv4 and OS
entropy generator. IDs are independent of paths/content; scene fixture references
retain the UUID. A source `name.ext` has adjacent `name.ext.sharkasset`, UTF-8
`shark.asset`, exact schema 1. Source location derives from sidecar location,
so moving both files requires no metadata rewrite. No legacy asset format exists:
schema 0, future versions and unknown envelope fields reject explicitly.

Sidecars store project/asset identity, kind, origin/license, a declared importer
identifier, bounded settings, dependency IDs, audience and bounded extras. A
declared importer is data, not a capability advertisement. Dependency resolution,
cache keys and background execution land in subsequent checkpoints. Audience
preserves the client/server content distinction from [ADR 0007](0007-mmorpg-architecture.md);
it does not authorize a client package to contain private server dependencies.

ProjectFiles extends its existing native ownership/revision/publication service
with typed sidecar reads/writes and source-presence inspection. The schema and
canonical encoder reuse its existing bounded JSON validation, including duplicate
keys, UTF-8, finite numbers and nesting. Native metadata reads have a smaller
64 KiB limit before allocation. There is no second filesystem writer or SQLite
dependency in Tools; EngineStorage remains server-only.

`AssetCatalog` borrows one ProjectFiles owner and publishes a complete validated
in-memory candidate from an explicit bounded sidecar list. A fresh owner UUID
and monotonically increasing revision fence query pages and refreshes. Failed
refreshes preserve the prior snapshot. An unchanged inventory/content/presence
does not advance the revision; input order is immaterial. Catalog records/pages
are owned values, never mutable views into an authoritative scene or registry.
The [contract](../asset-identity.md) fixes bounds and missing/duplicate policies.

Rename/move is a coordinated filesystem operation in this checkpoint. Neither
two file renames nor a directory watcher is presented as a multi-file transaction.
Refresh after an interrupted source-only move reports a missing source with the
same ID. A copied sidecar collision rejects the candidate and requires explicit
reconciliation; the engine never silently changes or guesses identity from bytes.
External file/topology changes obey ProjectFiles' existing final-interval
coordination contract. Catalog revision describes its snapshot, not a live watch.

The same CPU C++ metadata/catalog services feed human and terminal development
under [ADR 0008](0008-human-agent-authoring.md). No new editor, CLI protocol,
source decoder, renderer, runtime importer, streaming implementation or cooked
container is introduced. A saved FeatureWorld asset reference and actual
native metadata/move/missing/reopen case exercise the current supported surface.
The fixture reference uses bounded scene extras; it is not a new asset component
or a migration of the existing scene schema.
