# Shared authoring qualification

09e connects the services from [commands](authoring-commands.md),
[atomic batches/preview](authoring-transactions.md), [history](authoring-history.md),
[retained retries](authoring-retries.md) and [project files](project-files.md).
The existing AuthoredScene remains the only mutable authoring owner. The fixture
uses real native files and the public C++ services; it does not add an editor,
terminal protocol, second scene model, job framework or live player authority.

## Connected acceptance

`shark.tools.authoring-integration` has a 90-second timeout and seed 1337. Its
independently authored legacy/current documents are the existing versioned source
fixtures. The main case follows one owner and one actual file lineage through:

1. Read version-0 project/scene data, preserving original bytes until explicit
   migration publication. Preserve Unicode, global cells, hierarchy, source
   entity order, canonical extras and stable authored IDs.
2. Alternate human-equivalent single edits and agent-equivalent atomic batches.
   Preview publishes nothing and reserves neither revision, history nor retry ID.
   Validate complete expected source values and retained survivor handles.
3. Save, lose a reply, apply a later edit, replay the original outcome, undo and
   redo through the same history. Replays cannot erase later work. Invalid batches
   roll back; cached terminal failures remain failures after state changes.
4. Make unsaved edits and a real external text edit to the saved scene. Save
   conflict and ordinary reload reject without changing scene, history, retry
   retention, dirty status or external bytes. Explicit discard/reload creates a
   fresh Scene incarnation and empty history while retaining the owner's ledger.
5. Replay earlier decisions across activation without modifying external source.
   Reconcile a new intent, save, destroy the owner/file service, reopen and inspect
   exact persisted values. The new scope reports old IDs as unknown without writes.

Additional connected cases preserve redo across preview/noops/rejections, then
invalidate it on a changed branch; restore deleted source metadata with fresh
generations; and permanently fence expired edits/history movements after reload.
Every new submission checks outer access, reply outcome and original operation
result. Queries/replays report live observation separately from original results.

Five deterministic native faults cover read, stage creation, write, file flush and
publication failure. Each must be observed as an I/O error before publication;
old disk bytes, full captured scene, history and cached outcomes remain intact.
Recovery uses the same owner, undo/redo and an explicit successful save. Stage
siblings must be cleaned. An injected postpublication directory-flush failure is
a committed durability limitation, including on Windows; it cannot become a
rollback. A callback that edits after file publication proves the saved snapshot
can be valid while the newer scene stays dirty. The fixture callback catches all
exceptions and cannot throw across the private native hook.

Earlier allocation sweeps, revision/owner/visit/pending guards, schema/inspection,
the 4096-entity/4095-delete workload, file interruption and real restart tests
remain required. The new family supplements those workloads without changing
their inputs, limits, timeouts or seeds. No production implementation change is
implied by adding the connected fixture.

## Qualification rules

Freeze source, fixtures, public contracts and verification helpers before final
execution. Preserve earlier test inventories and add only the new family.
Require every expected base Catch case and seed, plus zero CTest failures/skips.
Refresh all seven full profiles: native Windows Debug/Release/ASan, retained WSL
GCC Debug/Clang ASan+UBSan, and both headless Server profiles. Run all eleven
authoring families separately in standalone Tools on both OS environments.

Compile all eight public Tools examples on MSVC/GCC/Clang with only public headers
and central GLM definitions; inspect compiler-reported includes. Audit actual
sanitizer flags and object symbols for the implementation and connected fixture,
alongside existing retry/probe fixtures. Refresh protected Runtime/Server builds,
dependency boundaries, relocated Runtime notices/deployment and the isolated
SDK-free Server package. Use only this PC and retained WSL environments.

Gate evidence must hash source, executed helpers, inventories, full JUnit,
individual case reports, native process receipts, objects and package records.
Parser negative controls reject missing tests, duplicate cases, changed seeds,
timeouts, cleanup errors and malformed success records. Preserve failed attempts
and distinguish them from final gate evidence. Full REQ-09 closes only when the
connected behavior and every required profile/package check pass.

## Complete CPU example

The two callers in a future UI or terminal adapter share this same owner. Keep
the original request until its outcome is known; a transport adapter must retain
it before dispatch. A new owner requires reconciliation, never an automatic retry
under a new identity. File publication has its own explicit result and revision.

```cpp
#include <shark/tools/project_files.hpp>
#include <utility>

shark::Result<shark::tools::FileRevision>
preview_edit_and_save(shark::tools::ProjectFiles& files,
                       const shark::tools::ProjectDocument& project,
                       shark::SceneId scene, shark::tools::AuthoringBatch edits) {
    using namespace shark;
    using namespace shark::tools;
    auto loaded = files.load_scene(project, scene);
    if (!loaded) return loaded.error();
    auto& owner = *loaded.value().owner;
    auto state = owner.mutation_status();
    if (!state) return state.error();
    if (!state.value().next)
        return Error{ErrorCode::conflict, "Mutation sequence exhausted"};
    auto preview = owner.preview(edits, state.value().revision);
    if (!preview) return preview.error();
    if (!preview.value().outcome) return preview.value().outcome.error();
    AuthoringMutationRequest intent{*state.value().next, state.value().revision,
                                    std::move(edits)};
    auto edit = owner.mutate(intent);
    if (!edit) return edit.error();
    if (!edit.value().outcome) return edit.value().outcome.error();
    const auto original = edit.value().outcome.value();
    if (!original->result) return original->result.error();
    auto saved = files.save_scene(owner, edit.value().observed_revision,
                                  loaded.value().file_revision);
    if (!saved) return saved.error();
    auto retry = owner.mutate(intent);
    if (!retry) return retry.error();
    if (!retry.value().outcome) return retry.value().outcome.error();
    if (!retry.value().outcome.value()->result)
        return retry.value().outcome.value()->result.error();
    return saved.value().file.revision;
}
```

The actor names describe equivalent C++ service callers, not shipped adapters.
FeatureWorld connects this supported surface in 10; terminal/project ownership
and editor integration remain 15/16. Long import/export jobs retain 11-14 and
their existing bounded completion primitives. These local source-authoring
guarantees do not implement M03 durable economy or a public gameplay protocol.
