# Authored Scene capture and activation

08b connects [source documents](source-documents.md) to the existing Core
[Scene owner](scene-contracts.md). The EngineTools C++ API is
[authored_scene.hpp](../engine_tools/include/shark/tools/authored_scene.hpp).
It runs without Editor, Client, a display or GPU. The
[checkpoint plan](roadmap/persistence-plan.md) and
[handoff](handoffs/08b-scene-activation.md) record its scope and qualification.

## Ownership and references

`AuthoredScene::create(project, scene, limits)` validates complete owned copies
of both documents. The scene's project ID must match the project, its scene ID
must occur in the project's catalog, and an optional world origin must use the
project's world ID. The selected scene may be any declared scene, not just the
startup scene. Unrelated catalog entries are not loaded. Source paths remain
lexical values here; [ProjectFiles](project-files.md) performs actual lookup and containment.

The owner retains immutable project settings and one active `Scene`. Loading
always creates `AuthoredSceneScope`, with authored entity IDs and no region,
replica or session authority. A world origin is authored metadata; it does not
shift local transforms, grant server ownership or implement seamless streaming.
Live durable state remains M03. No JSON reader or authoring dependency enters
Core, Runtime or Server.

Construction uses the existing Scene validators. Parents are created before
children even when the document lists children first. Every local transform and
composed world matrix must pass before the candidate becomes active. World shear
from valid hierarchy composition remains supported. Bounds may be lower than the
source defaults, but entity capacity cannot exceed 4096 and hierarchy depth cannot
exceed 256; Core's pending-work bounds still apply.

All methods require the creating thread, checked before reading mutable owner
state. The owner cannot move or copy. Its Scene must be destroyed on its owner
thread while live, following the existing Core lifetime contract. Concurrent
destruction is never supported.

`scene()` returns a checked local reference to the existing Scene for its normal
queries and mutations. That borrow ends at successful activation or destruction;
never retain it across either boundary. Retain checked handles, references or
revision tokens instead. There is no second authoritative mutable document model.
The caller must not destroy the owner from one of its Scene callbacks.

## Complete capture

`capture(expected_revision)` returns an owned `CapturedScene`: the project
document, current scene document, exact `SceneRevision` and dirty flag. The
optional expected revision rejects stale observations. Pending deferred writes
reject capture; callers must resolve them through the existing Scene boundary.
Capture does not apply queued work, mutate the Scene, clear dirty state or
acknowledge a disk save.

Capture follows all bounded query pages at one committed revision, including
empty pages with continuations. It reads mandatory Name/Transform values and
resolves parent handles into authored IDs. A final source validation checks the
complete candidate; failure returns no partial snapshot. Returned values survive
later mutations, replacement and owner destruction and can be passed to the
existing encoder. Encoding retains its separate output-byte budget.

World origin and scene/project extras survive capture. Existing entities retain
their original source array order and extras. Extras are bound to the loaded
entity incarnation, not just its durable ID: deletion followed by recreation of
the same ID cannot revive deleted metadata. New entities follow retained original
entities in stable query slot order and start with empty extras. No process-local
handles, cached matrices, revisions or network authority are serialized.

## Activation and unsaved changes

`status()` returns the current revision, pending-mutation count and dirty flag.
Dirty means the committed Scene revision differs from the last successful
in-memory activation or a successful ProjectFiles save acknowledgement. It is conservative: changing a value and changing it back
remains dirty, while a true Core no-op does not. Deferred work is separately
visible and does not advance the committed revision until applied. The baseline
alone is not evidence that any file exists or has been saved. ProjectFiles marks
the saved revision clean only while it still matches the owner and has no pending
work; a successful file publication reports newer unsaved edits separately.

`activate(candidate, expected_revision, policy)` resolves a new scene within the
same immutable project catalog. The expected instance and revision are mandatory.
The default `UnsavedScenePolicy::reject` preserves unsaved edits. Explicit
`discard` permits replacement of dirty committed state, but does not bypass a
stale revision, pending work, a closed Scene, a live visit or invalid input.
Unknown policy values reject.

The complete candidate and source metadata are prepared privately before touching
the active Scene. A failure leaves its identity, revision, entity values, queued
work, source metadata and dirty baseline intact. After final checks, the old
Scene closes and only nonthrowing ownership swaps publish the candidate. No
callback observes a partially built scene. This replacement operation is atomic;
ordinary Scene setters and deferred batches retain their existing semantics and
do not acquire assignment 09's undo or transaction behavior.

Every successful activation has a fresh instance, including identical-source
reloads. Old handles, references, cursors and expected revisions reject against
the new Scene. Durable IDs remain unchanged and can be looked up afresh. Success
establishes a new clean in-memory baseline. The retired Scene is destroyed; old
borrowed C++ references must not be used.

## Errors and use

Expected failures use owned `Result`/`Error`. Operations are
`source.scene.create`, `.status`, `.access`, `.capture` and `.activate`.
Validation preserves the underlying Core/source code and identifies a relevant
JSON-pointer field and authored entity when available. Revision, owner-thread,
dirty, pending-work and iteration-boundary failures use `conflict`. Allocation
or unexpected internal failures return `execution_failed` without exposing
third-party exception text. No user prompts or filesystem operations occur.

This complete C++ example loads values, performs a normal Scene edit and captures
the result. The second helper demonstrates explicit revision-checked replacement.
Neither helper reads or writes a file.

```cpp
#include <shark/tools/authored_scene.hpp>

shark::Result<shark::tools::CapturedScene>
rename_authored_entity(const shark::tools::ProjectDocument& project,
                       const shark::tools::SceneDocument& source,
                       shark::AuthoredEntityId entity) {
    auto loaded = shark::tools::AuthoredScene::create(project, source);
    if (!loaded) {
        return loaded.error();
    }
    auto access = loaded.value()->scene();
    if (!access) {
        return access.error();
    }
    auto handle = access.value().get().find(entity);
    if (!handle) {
        return handle.error();
    }
    auto renamed = access.value().get().set_name(handle.value(), shark::Name{"Spawn"});
    if (!renamed) {
        return renamed.error();
    }
    return loaded.value()->capture();
}

shark::Result<shark::SceneRevision>
replace_authored_scene(shark::tools::AuthoredScene& owner,
                       const shark::tools::SceneDocument& candidate,
                       shark::SceneRevision observed,
                       shark::tools::UnsavedScenePolicy policy =
                           shark::tools::UnsavedScenePolicy::reject) {
    return owner.activate(candidate, observed, policy);
}
```

[File services and external-edit conflicts](project-files.md) are implemented in
08c. The [08d integrated fixture](handoffs/08d-integrated-qualification.md)
connects legacy files, activation, edits, saves and external-edit recovery. Shared
history remains 09, FeatureWorld 10, terminal adapters 15 and editor controls 16.
This API is not a cross-process project lock or a player-facing protocol.
