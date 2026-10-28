# Client export content preparation

`prepare_client_export` in
[client_export.hpp](../engine_tools/include/shark/tools/client_export.hpp) is a
CPU EngineTools service. It borrows the current AuthoredScene, AssetCatalog and
AssetImports on their owner thread. It writes no source, cache or output file,
does not run imports, and initializes no Client/device. The
[export checkpoints](roadmap/export-plan.md) assign command/package work to 14b
and deployment to 14c. Final cooked content remains assignment 31.

The request requires exact scene/catalog/cache revisions, an explicit authored
camera ID and a syntactically valid client-side content profile. That target ID
selects existing cache bindings; it does not claim binary compatibility with a
host or platform. The owner must hold its project's declared startup scene and
the selected entity must have a Camera. Current unsaved edits are captured;
dirty state, source baseline, revisions, history and retained operations stay
unchanged. Authoring-only extras and unrelated project scenes are not exported.

Every renderer, including hidden entities, is checked. Built-in cube/plane use
the Core IDs; other roots must be fresh `shark.gltf` version-1 cache artifacts.
Missing/stale/blocked inputs reject with owned context and require explicit
import/refresh/reconciliation. The exporter never silently reuses last-valid
stale output, imports on demand or falls back to raw source conversion. Full dependency
closures retain existing project, target, audience, settings and byte checks.
Client export rejects server-private content; shared assets retain shared-only
dependency rules. A complete Core prepared load validates every mesh index.

The immutable move-only candidate contains project/world/name, startup scene,
camera/request revisions, source dirty status, prepared scene bytes/digest,
sorted unique prepared mesh payloads/digests/source keys and sorted source
attributions (asset/key/origin/license) for the used import closure. Import-only
dependencies contribute attribution, not raw payload files. Engine/library
redistribution notices remain the package builder's responsibility. Prepared
material limitations remain temporary opaque unlit; original cache textures/PBR
are not erased, but are absent from these runtime payloads.

Limits reuse 4096 scene entities, 126 external prepared meshes, 128 total import
closure sources, existing per-format limits and 32 MiB combined scene/mesh bytes.
Each freshness pass admits at most 512 dependency-node visits summed separately
per root, so shared dependencies count again for each root. The metadata graph is
precharged before source recapture. Existing cache queries read at most 1 MiB per
source node and 16 MiB per root closure; no repeated unbounded dependency scan is
hidden behind output deduplication. Attributions inherit bounded metadata strings.
This permits at most 512 MiB of source-read work per freshness pass, with two
passes during preparation; it is not a retained-memory budget.

`AssetImports::check_context` verifies project identity, catalog/cache scopes and
revisions, cache disk state and callback availability even for built-in-only
content. Fresh root queries and exact keys are rechecked after conversion.
`validate_client_export` repeats those fences and rejects a moved-from candidate.
Later publication must call it immediately before publishing. A subsequent scene,
source, metadata, catalog or cache change can invalidate the result. External
writers must coordinate around the final check/publication interval as required
by ProjectFiles; this is not a hostile-filesystem sandbox or durable transaction.

## Public CPU example

```cpp
#include <shark/tools/client_export.hpp>

shark::Result<shark::tools::PreparedClientExport>
prepare_startup(shark::tools::AuthoredScene& owner,
                shark::tools::AssetCatalog& catalog,
                shark::tools::AssetImports& imports,
                shark::AuthoredEntityId camera,
                const shark::tools::AssetTargetProfile& target) {
    auto scene = owner.status();
    if (!scene) return scene.error();
    auto inventory = catalog.revision();
    if (!inventory) return inventory.error();
    auto cache = imports.revision();
    if (!cache) return cache.error();
    return shark::tools::prepare_client_export(owner, catalog, imports,
        {camera, scene.value().revision, inventory.value(), cache.value(), target});
}
```

The caller must already have explicitly imported the selected mesh content for
that same client target. Keep these owners alive and call `validate_client_export`
before later publication. The candidate's bytes themselves remain owned after
the source owners are destroyed; they then describe a historical snapshot.
