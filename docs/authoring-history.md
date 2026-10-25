# Shared authoring history

09c adds bounded in-memory undo and redo to the existing CPU `AuthoredScene`
owner. Follow [ADR 0022](architecture/0022-shared-authoring-history.md).
[Typed edits](authoring-commands.md) and [atomic batches](authoring-transactions.md)
use one history for human- and terminal-agent-equivalent callers. There is no
editor, terminal adapter, persistent undo journal or retained retry outcome here.
Gameplay continues to mutate Core scenes without EngineTools history.

## Groups and limits

Each changed single command is one group. Each changed atomic batch is one group,
including a batch whose intermediate edits cancel to source-equivalent values.
`command_count` reports the submitted group size. Empty batches, all-noop edits,
preview and rejection neither append history nor invalidate redo nor consume a
ticket. Preview checks the same history capacity policy as commit, but reserves
nothing. Grouping across multiple separate calls is unsupported; submit a batch.

`AuthoringHistoryLimits` defaults to 32 entries and a 32 MiB retained snapshot
charge. Creation accepts 1..32 entries and 1..64 MiB; named public hard bounds
are `authoring_maximum_history_entries` and `authoring_maximum_history_bytes`.
The entry count includes both undo and redo directions. Publication discards redo
and evicts oldest groups until both bounds admit the new group. An individual
group larger than the byte bound rejects before changing anything. A failed edit
does not evict old history. Clearing history cannot change configured limits.

Each entry conservatively charges its object, both `SceneDocument` objects,
entity vector capacities, Name/extras string capacities plus terminators, and
replacement-ID vector capacity. Shared immutable snapshots are charged again per
entry; small string storage can be counted twice. This is a deterministic local
storage budget within one implementation, not a portable serialized byte size,
allocator overhead measurement or process RSS. Entry count and existing source
bounds additionally bound ownership structures. The revision-bound cached snapshot
shares a retained entry value and is cleared with history; it adds no independent
mutable scene. No third-party entity/registry types enter this API.

## Observation and conflicts

`history(expected_revision)` returns the current revision, synchronization status,
undo/redo counts, charged bytes, configured limits and optional next groups.
Omitting the revision is a fresh observation. Tickets are owner-local monotonically
increasing uint64 values starting at one. They survive clearing and activation
without reuse; exhaustion rejects further changed edits. A ticket is not a durable
ID or retry identity. Pass both the observed next ticket and current complete
`SceneRevision` to `undo` or `redo`. There is no operation that jumps history.

Check both Result levels on a history movement. An outer failure means the owner
could not be accessed, such as a wrong thread or closed scene. Otherwise the
owned reply reports the requested ticket, current observed revision and nested
success/error. Missing direction, wrong ticket, stale/foreign revision, pending
Core mutations, active visits or a desynchronized history reject without moving
stacks, changing source metadata, dirty state, handles or revision. No successful
reply is retained for replay: a lost reply requires inspection and reconciliation
until 09d adds bounded retry behavior.

An out-of-band Core change while any group remains marks history unsynchronized.
New commands, preview and undo/redo reject instead of overwriting intervening
work. `clear_history(current_revision)` is explicit reconciliation: it retains
current source values, revision, dirty baseline, handles and query cursors while
clearing both directions and charged bytes. It requires the same owner/immediate
mutation guards. With no retained groups, new authoring can start from the current
Core revision. Public inspection remains read-only; mutable access is private to
invariant fixtures, never an adapter escape hatch.

## Restoration and publication

Undo/redo build a private Core candidate from the current scene and restore a
complete immutable authored snapshot through validated Core mutations. All source
values, parent links, global origin, entity order and bounded extras round-trip.
Internal child iteration order is not a source format guarantee. Detaching deepest
first and rebuilding parent-first avoids invalid transient composition/depth.
An entry records IDs replaced within its group: those incarnations are removed
and recreated when crossing that group in either direction. Deleted entities
also return with fresh generations; old handles never become valid again.
Surviving incarnations retain their handles. Restored extras bind to their newly
created handles rather than a removed/replaced incarnation.

Allocate the complete candidate, metadata and reply before publishing. A failed
preparation leaves both directions and live state intact. Publication swaps the
validated candidate into the same Scene owner and changes its revision exactly
once, even for source-equivalent history movement. The source scene instance is
unchanged. Core's private force-revision flag is inaccessible to public callers.
Effects conservatively return sorted unique IDs from the union of current and
target snapshots, at most twice the source entity bound (8192); deleted IDs may
be present. Query cursors are invalidated by every successful history movement.
Internal capture reuses canonical immutable extras validated on activation and Core-
validated components/hierarchy, while rechecking the aggregate extras budget for
new empty objects. Public capture and caller-owned source validation retain full
codec validation. History mutation never reparses immutable extras through the
pinned JSON DOM, whose noexcept destruction can allocate.
Single subtree deletion keeps the original immediate Core validated mutation;
its prepared history projection only removes that subtree from a revision-bound
snapshot, retaining the same atomic failure boundary and source ordering.

## Save and reload policy

Saving retains history and acknowledges only the exact saved revision. Undo and
redo preserve that baseline and are conservatively dirty, even if resulting
source values equal a prior save. Save again to mark the current revision clean.
Rejected saves or reloads preserve history. Successful activation/reload replaces
the scene with a fresh instance and clears both directions; tickets are never
reused on that owner. A newly created/reopened owner starts empty. No history is
serialized into project or scene JSON, and no source/protocol version changes.
Existing three-argument create calls retain source compatibility; the optional
fourth argument selects history limits. Rebuild C++ consumers with the engine.

## Complete CPU example

This compiles with only public Shark headers and centrally configured GLM.

```cpp
#include <shark/tools/authored_scene.hpp>
#include <utility>

shark::Result<void> edit_and_undo(shark::tools::AuthoredScene& owner,
                                  shark::AuthoredEntityId entity) {
    auto state = owner.status();
    if (!state) return state.error();
    auto edit = owner.execute(shark::tools::SetNameCommand{entity, {"Shared edit"}},
                              state.value().revision);
    if (!edit) return edit.error();
    if (!edit.value().outcome) return edit.value().outcome.error();
    if (!edit.value().outcome.value().changed) return {};
    auto history = owner.history(edit.value().observed_revision);
    if (!history) return history.error();
    if (!history.value().next_undo)
        return shark::Error{shark::ErrorCode::conflict, "No undo group"};
    auto undo = owner.undo(history.value().next_undo->ticket, history.value().revision);
    if (!undo) return undo.error();
    if (!undo.value().outcome) return undo.value().outcome.error();
    return {};
}
```

`shark.tools.authoring-history` uses a 60-second timeout and seed 1337. Its CPU
fixtures cover grouping, exact source replay, incarnation lifetimes, conflicts,
budgets, save/reload, allocation rollback and maximum source size. Earlier
persistence, command/batch and Core tests remain required. FeatureWorld connects
these services in 10; CLI/editor adapters remain 15/16 and retry retention is 09d.
Full REQ-09 remains NOT_RUN until 09e integrated qualification.
