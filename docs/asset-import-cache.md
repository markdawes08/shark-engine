# Development import and cache contract

11b extends [persistent asset identity](asset-identity.md) through
`shark/tools/asset_import.hpp`, following [ADR 0026](architecture/0026-development-asset-cache.md).
The service borrows one ProjectFiles owner, catalog and immutable native importers;
all must outlive it, with importer instances exclusively borrowed. Public operations
run on the creating offline authoring thread. Runtime/Server exclude EngineTools.
11c adds [bounded background jobs](asset-import-jobs.md) through the same preparation/
publication path; terminal/editor adapters keep 15/16. Source/metadata schemas remain 1.

12d adds [typed glTF resources and engine payloads](gltf-assets.md). Dependency
callback views additionally expose the captured child's importer/kind/versions
and original source digest. Existing byte importers and keys retain their format;
glTF requires its separate identity-only resource payload rather than interpreting
a possibly transformed byte payload as source. Owner capture and publication
fences are unchanged.

14a adds `AssetImports::check_context` for composition by the
[client export service](client-export.md). It checks the expected project,
catalog/cache scopes and revisions, cache disk state and callback availability
without importing or writing. It also fences built-in-only exports that have no
cache roots; freshness of actual roots still requires the existing query path.

## Inputs, discovery and keys

Importer discovery returns only registered implementations with bounded settings
schemas. A descriptor ID/kind is a lowercase token of at most 64 bytes; importer
and payload versions are nonzero uint32 values. At most sixteen importers register,
with distinct IDs. Changing a descriptor requires a new service owner. Trusted
callbacks normalize settings, import immutable values and validate output; thrown
exceptions become errors. They may not retain input views, mutate source/project
state or depend on undeclared clock/random/path/global inputs. Change importer or
pipeline versions when semantics change.

ByteAssetImporter advertises `shark.bytes`, kind `bytes`, version/payload version
1. Settings accept only optional `transform`, exactly `identity` (default) or
`uppercase-ascii`, and normalize the default explicitly. Its development payload
has fixed magic/version, asset UUID bytes, explicit byte count and transformed
source bytes. Other formats are rejected through missing importer/kind diagnostics;
metadata declarations do not install them. External decoders remain 12/24/27.

A target has a bounded lowercase ID and exact client/server side; this is declared
content context, not a tested hardware capability. Client closures reject server
assets, server closures reject client assets, and shared assets depend only on
shared assets. Missing IDs/sources, cycles and wrong projects diagnose with ID/path
context. IDs survive source/sidecar moves; source paths are absent from cache keys.

Keys use SHA-256 over a domain-separated, length-prefixed, explicit little-endian
encoding of pipeline version, project/asset UUID, kind/audience, source digest,
normalized settings digest, importer/version, payload version, target ID/side and
sorted dependency IDs/keys. Canonical settings remove JSON formatting as a key
input. Provenance/extras remain metadata, not importer inputs. Same declared
inputs reproduce keys and payload bytes. Content digests detect corruption;
they are not signatures, permissions or a hostile-code sandbox.

## Preparation, publication and last-valid output

Preparation captures an exact bounded input closure and current cache/catalog
revisions. It deduplicates matching keys, reusing validated artifacts without
calling import. The prepared value owns its bounded result and is scoped to its
service. It publishes nothing. Publication rechecks the same service/cache/catalog
scope, current source bytes and sidecar revisions before one native atomic
replacement. External filesystem/topology writers follow
[ProjectFiles coordination](project-files.md); they must not race final compare/
publication intervals. Revisions do not implement a hostile filesystem sandbox.

Query returns fresh, stale or missing, required/last-valid keys and an explicit
blocked-input error where applicable. It never runs import. Reading an explicit
retained key returns an owned artifact with its actual kind/version/profile and
dependency keys; that does not authorize stale output for export. Preparing a
failed update leaves the last valid binding and payload intact. A published
directory-flush warning reports commit, not rollback; check FileCommit durability.
Cache file edits by another writer conflict until explicit reload/reconciliation.

## Bounded snapshot and maintenance

Limits: 128 closure nodes, 256 metadata dependency IDs, 1 MiB/source, 2 MiB/payload,
16 MiB total source and separately total payload bytes per preparation/cache,
128 artifact entries/bindings, 4 MiB JSON index, 24 MiB complete snapshot and
4096 bytes per normalized settings/schema object. Existing metadata/catalog/path
limits remain. Budget checks precede allocations and publication. Full storage
rejects explicitly until prune/clear; it never drops a last-valid artifact to fit.

The fixed root `.shark-development-cache` contains a 56-byte explicit header:
eight magic bytes, uint32 container version, uint32 index length, uint64 payload
length and 32-byte SHA-256 of index plus payload. Index schema 1 is canonical JSON;
payload offsets are contiguous and checked against actual lengths/digests. Every
binding/dependency resolves within the validated snapshot, with no duplicate keys,
overlapping/unreferenced bytes, cycles or cross-side dependencies. Current versions
are exact-match only. Corrupt/wrong-project caches reject without altering source;
the owner may explicitly remove this disposable generated file and reopen.

Native publication uses `.shark-cache-stage-UUID` alongside the fixed cache.
At most one unpublished cache stage is admitted; a stage blocks another cache
publication until explicit recovery. Recovery scans at most the existing 8192
root entries and removes only one validly named, regular, unaliased cache stage
within the 24 MiB bound. Malformed/multiple/oversized stages reject for explicit
reconciliation. Ordinary `.shark-stage-*` source-save evidence is untouched.
Managed cache disk use is at most 48 MiB (committed snapshot plus stage), excluding
externally manufactured files and the fixed existing coordination lock.

Prune retains the closure of every last-valid binding, including dependencies of
stale parents. Clear explicitly replaces the snapshot with an empty one; source,
metadata, scene history and IDs remain unchanged. Both require the current cache
revision and use the same atomic writer. Interrupted native stages require recovery,
not blind repeated publication. The cache is ignored by Git and reproducible from
declared sources/tools. It is not an independent backup or final cooked package.

The public C++ service can prepare and publish a root without a display:

```cpp
#include <shark/tools/asset_import.hpp>
#include <utility>

shark::Result<shark::tools::AssetCacheStatus>
import_root(shark::tools::AssetImports& imports, shark::tools::AssetId root,
             const shark::tools::AssetTargetProfile& target,
             shark::tools::AssetCatalogRevision catalog_revision) {
    auto revision = imports.revision();
    if (!revision) return revision.error();
    auto prepared = imports.prepare(root, target, catalog_revision, revision.value());
    if (!prepared) return prepared.error();
    auto committed = imports.publish(std::move(prepared.value()), catalog_revision, revision.value());
    if (!committed) return committed.error();
    return imports.query(root, target, catalog_revision, committed.value().revision);
}
```
