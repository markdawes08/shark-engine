# Shared authoring command foundation

09a extends the existing [AuthoredScene](authored-scenes.md) owner with local
CPU C++ editing in EngineTools. See [ADR 0020](architecture/0020-shared-authoring-commands.md)
and the [checkpoint plan](roadmap/authoring-plan.md). The public headers are
[authored_commands.hpp](../engine_tools/include/shark/tools/authored_commands.hpp)
and [authored_scene.hpp](../engine_tools/include/shark/tools/authored_scene.hpp).

## Commands and ownership

`AuthoredScene::execute(command, expected_revision)` accepts one of these owned
typed values. IDs are authored entity IDs within this owner, never runtime handles,
player identities or authority credentials. Creation takes an explicit stable ID;
Core's existing ID generation can supply it before constructing the command.

| Command | Behavior |
| --- | --- |
| `CreateEntityCommand` | Create an ID with Name, local Transform and optional parent ID |
| `DeleteEntityCommand` | Delete the target and its complete subtree |
| `SetNameCommand` | Validate and set the mandatory display Name |
| `SetTransformCommand` | Validate/canonicalize local TRS and all affected world transforms |
| `ReparentEntityCommand` | Change parent using explicit keep-local or keep-world semantics |

Name/Transform remain mandatory; removal and unknown component types are not
supported. Validation uses the existing [component schemas](scene-inspection.md)
and [transform/hierarchy rules](transforms.md), including representability,
cycles, depth and composed-transform limits. Public `scene()` is read-only even
on a mutable owner. It cannot enqueue, commit, close or otherwise mutate the Scene.

Only the creating thread may access the service. Immediate commands reject
pending mutations and active visits through the existing Core boundary. No worker,
callback or second scene owns these edits. Core remains independently usable by
gameplay; this local service adds no player endpoint or live database authority.

## Revision and result rules

The expected revision is mandatory and contains both instance and counter. It is
checked before target resolution and before any edit, including a requested no-op.
An old instance after reload and a stale snapshot after another caller's edit both
reject. A successful actual change increments the counter exactly once. A Core
no-op returns success with no counter change. Normal edits preserve instance,
live handles of retained entities, source extras and the saved baseline. They do
not call reload or acknowledge a save. Deletion/recreation never revives extras
belonging to a prior entity incarnation.

The return type is `Result<AuthoringCommandReply>`:

- An outer error means the owner could not be accessed or the reply could not
  be produced before mutation. Wrong-thread and closed-owner access reject here.
- A reply contains `observed_revision`, the current revision after the decision,
  and `outcome`, a `Result<AuthoringCommandEffect>`. A valid outer Result alone
  does **not** establish that the edit succeeded.
- An inner error reports a rejected command, such as stale revision, missing ID,
  invalid component value, hierarchy conflict or pending/visit boundary. The
  complete scene, revision and dirty baseline are unchanged.
- A successful effect reports `changed` and sorted, unique `affected_entities`.
  This bounded set tells consumers which IDs to refresh. It includes a deleted
  subtree, affected transform descendants and old/new parents whose child links
  changed. Some IDs may no longer exist; inclusion does not assert that every
  component value changed. A no-op has an empty set. Its size is bounded by the
  configured authored entity capacity, at most 4096.

Receipt storage and the refresh set are prepared before entering Core mutation.
No allocation or user callback follows a successful mutation. Errors use the
existing owned codes and entity context, with operation `authoring.execute`.
Expected conflict information is structured: callers can inspect the reply's
observed revision and error code without parsing diagnostic text.

There is no mutation retry identity or retained outcome in 09a. Calling again is
a new invocation subject to the supplied revision; it does not recover an earlier
reply. After uncertainty, inspect state and reconcile deliberately. 09d supplies
explicit deduplication, retention and lost-reply behavior. [09b batches and pure preview](authoring-transactions.md) extend the same owner
through separate overloads; history retains 09c. This single-edit API remains
synchronous and has no retained retry outcome.

## Bounded inspection

`query_entities(options)`, `continue_query(cursor)` and
`inspect_entity(authored_id, fields, expected_revision)` use Core's existing
page/scan limits, exact name-prefix filtering, selected fields and immutable
owned results. Query pages include stable IDs and the scene revision. Parent
handles in Core query options/results remain checked, process-local values.
Continue a cursor only at its original revision; rejected continuation never
combines old and new state. Empty pages can still contain a continuation.

Queries do not mutate the owner or acknowledge dirty state. Existing source
capture, explicit activation and ProjectFiles save/reload retain their
[persistence contracts](project-files.md). Schema discovery remains the fixed
Core catalog. CLI framing and editor controls arrive in their assigned adapters.

## Complete public example

This synchronous helper applies an edit against a caller's observed revision,
checks both outcomes, then captures that exact committed revision. The caller
retains the full reply instead if it needs refresh IDs or a rejected current
revision. No file, display, device or private dependency header is required.

```cpp
#include <shark/tools/authored_scene.hpp>

shark::Result<shark::tools::CapturedScene>
set_authored_name(shark::tools::AuthoredScene& owner,
                  shark::AuthoredEntityId entity,
                  shark::SceneRevision observed,
                  const shark::Name& name) {
    auto reply = owner.execute(shark::tools::SetNameCommand{entity, name}, observed);
    if (!reply) {
        return reply.error();
    }
    if (!reply.value().outcome) {
        return reply.value().outcome.error();
    }
    return owner.capture(reply.value().observed_revision);
}
```

`shark.tools.authoring-commands` qualifies this C++ surface with seed 1337 and a
60-second timeout, including alternating human/agent-equivalent callers, stale
and invalid edits, query bounds, lifetime and real save-baseline behavior. Existing
persistence cases use the same editing path. FeatureWorld/AuthoringCommands
connects these commands in 10; full REQ-09 remains NOT_RUN until 09e.
