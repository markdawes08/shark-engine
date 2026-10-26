# ADR 0026: Reproducible development imports and atomic cache snapshots

Date: 2026-10-26. Status: accepted for 11b. Follow the
[asset checkpoints](../roadmap/asset-plan.md), [ADR 0025](0025-asset-identity.md),
[MMORPG plan](../roadmap/mmorpg-plan.md) and [human/agent contract](0008-human-agent-authoring.md).

EngineTools borrows the existing ProjectFiles/catalog owner. A focused trusted
native importer interface describes an immutable ID, version, kind, payload
version and settings schema, normalizes bounded settings, imports immutable
values and validates its output. No paths, scene writer, dynamic plugin loader
or shell is supplied. ByteAssetImporter provides actual engine-owned development
byte payloads with identity/uppercase-ASCII transformations. glTF remains 12.
All CPU authoring stays independent of Client/Editor/display. Execution is
synchronous offline authoring in 11b; bounded workers keep 11c.

Core adds an engine-owned SHA-256 value/API over its already pinned private
Mbed TLS/PSA dependency. TLS and content hashing share the existing mutex,
initialization, connection-ID counter and shutdown lifetime, extracted into a
private runtime. This is not custom cryptography, a new dependency or an asset
service in Core/Server. Public headers contain only engine/standard types.

Import preparation validates a bounded dependency closure, detects missing/cyclic
references, enforces client/server/shared content edges and captures exact source
and metadata revisions. Keys use explicit domain/version/length encoding over
project/asset identity, source/settings digests, kind/audience, importer/version,
payload version, target profile/side and ordered dependency IDs/keys. Paths and
formatting are not identity. Inputs exclude wall time/randomness and callback
order. Importer/pipeline semantic changes require a declared version change.

Matching keys reuse validated artifacts without executing the importer. Preview
does not publish. Publication checks owner/cache/catalog/source/metadata fences
again; a late source/settings change cannot replace newer valid output. Timeout,
cancellation and background-job orchestration are added in 11c through this same
separation. Retained output remains explicitly stale/missing/fresh; a blocked
new input never erases the last valid artifact.

The disposable project-root `.shark-development-cache` is one bounded snapshot:
an explicit little-endian header, independently versioned canonical JSON index
and engine-owned binary payloads, with whole-container and per-payload SHA-256.
The index records dependency keys, target/side, versions, lengths and offsets.
It is a development format, not the final cooked format (31) or live storage.
Exact schema/container versions reject; no pre-11b cache exists to migrate.

Publishing the complete snapshot through ProjectFiles' existing native staged
replacement makes index/payload visibility atomic together. A failed or
interrupted import preserves the previous snapshot. Generated cache stages have
a separate reserved namespace. Publication refuses an unrecovered stage; explicit
bounded recovery deletes only that unpublished cache stage, preserving authored
files and ordinary source-save stages. Peak managed disk use is one committed
snapshot plus one stage. Explicit prune retains last-valid dependency closures;
clear discards the disposable cache. No automatic eviction loses valid output.

The [development cache contract](../asset-import-cache.md) fixes resource limits,
interfaces, compatibility, external-writer coordination and qualification.
Shared CPU services feed later human/terminal adapters; no import CLI, editor,
runtime source decoder, streaming implementation or final cook is introduced.
