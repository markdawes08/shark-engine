# Asset metadata and catalog contract

Checkpoint 11a provides CPU EngineTools services in
`shark/tools/asset_metadata.hpp`, `project_files.hpp` and `asset_catalog.hpp`.
Follow [ADR 0025](architecture/0025-asset-identity.md) and the
[four checkpoints](roadmap/asset-plan.md). Import/cache/jobs remain 11b/11c;
glTF remains 12, full adapters 15/16, streaming M05 and cooked containers 31.

## Persistent sidecars

The source `assets/one.txt` has sidecar `assets/one.txt.sharkasset`. Move both
together, including their containing directory if desired. Metadata has no
embedded filename to become stale. Paths use existing ProjectPath validation;
the exact lowercase sidecar suffix is required, and a sidecar cannot be a source
itself. Sources/sidecars cannot alias another asset's directory, collide by ASCII
case spelling, use unsafe native aliases or escape the canonical project root.
Unicode is preserved; no Unicode normalization/case-fold identity is promised.
Native path checks retain the existing current-filesystem spelling protections.

An example schema-1 envelope is:

```json
{
  "format": "shark.asset",
  "schema_version": 1,
  "project_id": "30000000-0000-4000-8000-000000000001",
  "asset_id": "30000000-0000-4000-8000-00000000000a",
  "kind": "fixture",
  "origin": "Shark authored offline fixture",
  "license": "MIT",
  "importer": "shark.fixture",
  "audience": "shared",
  "settings": {"quality": 2},
  "dependencies": [],
  "extras": {}
}
```

Only `extras` is optional. IDs use Core's validated non-nil UUIDv4 representation;
new production identities use `generate_stable_id()`. Seeded/fixed IDs are for
fixtures. `kind`/`importer` are lowercase ASCII identifiers starting with a letter
and continuing with letters/digits/dot/underscore/hyphen, at most 64 bytes.
Declaring an identifier does not install or run it. Origin/license are required,
nonempty UTF-8 data without controls, at most 1024/128 bytes; URLs are never fetched.
Audience is exactly `client`, `server` or `shared`. Missing dependency IDs are
preserved in metadata for subsequent graph resolution; self/duplicate IDs reject.

Metadata is at most 64 KiB on native read and encoding. Each settings/extras input
and canonical object is at most 4096 bytes. The existing JSON budgets apply:
32 container levels including the envelope, 200000 nodes and 4096 bytes per
decoded string. Raw NUL, invalid UTF-8, duplicate decoded keys, nonfinite or
overflowed numbers, unknown fields and schemas other than integer 1 reject.
At most 256 dependency IDs are sorted on validation. Settings/extras keys and
output use the existing canonical two-space encoder with a trailing newline.
Pure codec/validation calls perform no filesystem access.

`ProjectFiles::read_asset_metadata` returns typed values and its exact root/path/
content FileRevision. `write_asset_metadata` requires absence (`nullopt`) or that
exact revision; stale, foreign-root/path and externally edited values reject.
It uses the same staged native replacement and explicit FileDurability outcome
as [project files](project-files.md). A successful durability warning describes
a published file, not rollback. `asset_source_exists` checks a regular source
without reading/decoding it; absence is false, unsafe topology/I/O is an error.
These are explicit file edits outside authored-scene undo history. Recovery uses
the previous validated metadata and a current exact file revision; adapters may
not bypass conflicts or imply durable undo. Shared project attachment retains 15.

## Derived catalog and missing assets

Create a catalog for a project while retaining its ProjectFiles owner. Both are
creating-thread services; ProjectFiles must outlive the catalog. `refresh` takes
the complete inventory of sidecar paths and the current catalog revision. It is
an explicit snapshot operation, not a recursive scan, watcher or import. The
caller coordinates external file changes as in ProjectFiles. Rebuilding after
restart discovers the same IDs from the supplied persistent sidecars.

The fixed limits are 4096 entries, 16 MiB combined raw metadata plus source/sidecar
path bytes, and 1-128 records per query page. Native path inspection keeps its
existing directory-entry bounds. Records are ordered by AssetId independently
of inventory order. Every changed successful refresh publishes once; unchanged
raw content, paths and source presence preserve the revision. Scope is a fresh
UUID for each catalog owner. Revision exhaustion rejects and requires a new
owner. Cross-owner/stale expected revisions reject with `conflict`.

All metadata must parse, belong to the requested project and have distinct IDs
and portable paths before any candidate becomes visible. Errors/allocations
preserve the prior catalog; caller-owned snapshots stay valid. A duplicate
sidecar ID, even identical bytes at a new path, rejects the entire candidate
with asset/path context. Explicitly remove/reidentify the unintended duplicate;
there is no automatic ID regeneration, content matching or guessed move.

`resolve` returns `source_present` for a cataloged regular source, `source_missing`
for a cataloged sidecar whose source is absent, and `unknown_id` for an ID absent
from the current inventory. Missing sources retain metadata and identity; no
fallback ID, placeholder payload or silent success is manufactured. Missing or
malformed listed sidecars fail refresh instead of dropping entries. Omitting a
sidecar from an explicit new inventory removes only its derived record. It
does not delete source/metadata. A source-only interrupted move becomes missing
until its sidecar joins it and the caller refreshes the correct inventory.

Query/resolve results are snapshots. An outside edit is observed on refresh;
source-present does not mean decoded, imported, cached, renderable or cookable.
The full import/missing-cache policy will extend this distinction in 11b.

The following public example performs a complete bounded catalog query:

```cpp
#include <shark/tools/asset_catalog.hpp>
#include <array>

shark::Result<shark::tools::AssetResolution>
inspect_asset(shark::tools::ProjectFiles& files, shark::tools::ProjectId project,
              shark::tools::AssetId asset, const shark::ProjectPath& source) {
    using namespace shark::tools;
    auto sidecar = asset_sidecar_path(source);
    if (!sidecar) return sidecar.error();
    auto catalog = AssetCatalog::create(files, project);
    if (!catalog) return catalog.error();
    auto initial = catalog.value()->revision();
    if (!initial) return initial.error();
    const std::array inventory{sidecar.value()};
    auto refreshed = catalog.value()->refresh(inventory, initial.value());
    if (!refreshed) return refreshed.error();
    return catalog.value()->resolve(asset, refreshed.value());
}
```

`shark.tools.asset-catalog` has a 60-second timeout and Catch seed 1337.
`featureworld.asset-identity` adds the saved reference, actual native sidecar
publication/query/move/missing/reopen path and unchanged scene owner. The earlier
five cases and original process checks remain; mapped surfaces and independent
coverage expectations extend together. No device is required.
