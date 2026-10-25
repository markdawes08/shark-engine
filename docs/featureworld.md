# Saved FeatureWorld

Checkpoint 10b connects the [CPU runner](scene-runner.md) to the versioned
[project](../assets/FeatureWorld/FeatureWorld.sharkproject) and
[scene](../assets/FeatureWorld/scenes/FeatureWorld.sharkscene), following
[ADR 0024](architecture/0024-headless-scene-runner.md). This small regression
fixture grows with supported components and APIs. It currently contains four
entities with mandatory Name/Transform, a three-level hierarchy, duplicate display
names, Unicode text, source extras and a stable global cell anchor. The anchor
does not implement streaming, origin rebasing or live region authority.

From the repository root, after building the existing Tools profile:

```text
build/tools-debug/bin/SceneRunner.exe --list-cases
build/tools-debug/bin/SceneRunner.exe --case featureworld --project assets/FeatureWorld/FeatureWorld.sharkproject --workspace out --ticks 3 --hz 20 --seed 1337 --run-id featureworld --revision my-revision --output out/featureworld-result.json
```

`out` must already exist and the result path must be absent. The project root
must permit the existing ProjectFiles coordination lock; an active cooperating
owner causes explicit failure. Copy the two source files to a writable project
root when testing from a read-only checkout. Run from any current directory by
passing explicit paths. There is no compiled-in checkout or fixture location.

`--case featureworld` selects all five cases in catalog order. Each concrete ID
also selects one case. Supply `--workspace EXISTING_DIRECTORY` for the suite or
persistence case. Only persistence uses it, creating a unique child with its own
project/scenes and removing its known files on exit. Input source bytes are
preserved. A failed cleanup leaves inspectable remnants; it cannot delete the
caller-selected parent. Generated results belong under ignored `out/` or `build/`.

| Executable case ID | Actual assertions |
| --- | --- |
| `featureworld.components` | Independent saved values/IDs/metadata, schema versions, shared validation, bounded query pagination and Name/Transform/hierarchy inspection |
| `featureworld.commands` | Create, subtree delete, Name/Transform edits, both reparent modes, world/local poses, stale handle/revision rejection, invalid values/cycles and unchanged metadata |
| `featureworld.batches-history` | Pure preview, late-invalid atomic rejection, one-revision commit, grouped undo/redo, surviving handles and explicit history clearing |
| `featureworld.retries` | Fresh owner scope, sequenced mutate, all three Result levels, retained query/exact replay, payload mismatch and preservation of a later edit |
| `featureworld.persistence` | Actual native create/save, dirty acknowledgement, retained history, exact-content file revisions, codec round trip, reopen/reload and fresh incarnation |

Every case receives a fresh owner from the same saved baseline. Functional work
occurs at tick zero; subsequent fixed ticks check stable identity/entity counts.
The fixture has no input actions and performs no RNG draws. Seed/rate/tick
context remains explicit in results; foundation cases separately exercise input
and seeded data. At three ticks, initial revision values are 4 and final values
are 4, 11, 7, 6 and 4. Persistence's final value belongs to a fresh incarnation.
Process-local incarnation IDs vary and are excluded from semantic repeat checks.

`--list-cases` emits UTF-8 `shark.scene-cases`, integer `schema_version: 1`,
`mode: cpu`, a unique ordered `cases` array and a `suites.featureworld` array.
It requires no project or device. Execution retains `shark.scene-run` version 1
and exits 0/1/2 as documented in the runner contract. Valid but unexpected source
values fail setup assertions with case/phase context. Malformed source, unsupported
versions, escaping paths and missing options produce structured ERROR results.
The saved source is schema 1; the existing codec migration/rejection policy is
unchanged. No renderer is initialized by these cases.

[requirements.json](coverage/requirements.json) records the saved paths/native
catalog and maps every current component/command to real case IDs. The independent
process fixture resolves discovery, executes every case, verifies repeat results
and checks that neither the versioned input nor its temporary copy changed.
Existing behavior suites still cover large scenes, allocation limits, native
faults and lifetime/resource boundaries. Network/storage/platform fixtures remain
separate. Lua stays empty until19; terminal/editor authoring remain15/16.
The [integrated qualification](featureworld-qualification.md) additionally audits
test registrations, independent surface completeness and assertion publication
through the shared CPU/GPU supervisor.

The shared public API uses no private headers or external test library:

```cpp
#include <shark/tools/feature_world.hpp>

shark::Result<shark::tools::SceneRunReport>
check_featureworld(const shark::tools::ProjectDocument& project,
                   const shark::tools::SceneDocument& scene,
                   const std::filesystem::path& existing_scratch_parent) {
    shark::tools::SceneRunConfig config;
    config.ticks = 3;
    config.tick_rate_hz = 20;
    config.revision = "my-revision";
    const auto cases = shark::tools::feature_world_cases(existing_scratch_parent);
    return shark::tools::run_scene_cases(project, scene, config, cases);
}
```

Always inspect both the outer Result and the report status. CTest families
`shark.tools.featureworld` and `shark.tools.featureworld-process` have 60-second
timeouts; the Catch family uses seed1337. They extend the earlier runner and
authoring tests. Full REQ-10 is PASS in the
[integrated handoff](handoffs/10d-integrated-qualification.md), including explicit
GPU qualification and the independent coverage/failure audit.
