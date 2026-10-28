# FeatureWorld qualification

Assignment 10 closes through the [four checkpoints](roadmap/featureworld-plan.md).
The CPU [runner](scene-runner.md), saved [FeatureWorld](featureworld.md) and
explicit [CPU/GPU supervisor](scene-execution.md) keep their existing contracts.
This gate adds a development test fixture, not a new authoring API or renderer.

`shark.tools.scene-qualification` is a CPU-only integration family with a
90-second timeout. Its standard-library Python supervisor obtains the actual
native case catalog, device-free GPU catalog and current CTest inventory. It
compares all manifest mappings with real test registrations or explicitly
identified declarations for another profile. A declaration is not a passing
execution. Component/command mappings must have registered tests in the current
Tools profile; their independently reviewed exercise inventory contains twenty-four
scene surfaces and one external GPU command after 11a. 11b extends the independent
inventory to 34 scene surfaces and one external GPU command, adding ten actual
import/cache operations in the seventh native FeatureWorld case. 11c extends it
to 40 CPU scene surfaces and one external GPU command with six job operations in
the eighth native case. It exercises real private-worker imports, pending-work
deduplication, progress, owner-side publication, cancellation before/after commit,
retained outcomes, explicit retry and closed admission. Lua remains empty until
real bindings land. New surfaces extend their actual cases, independent inventory
and manifest together.

11d adds a ninth connected asset-pipeline case while retaining those forty CPU
surfaces. It carries one saved scene reference through asynchronous imports,
coordinated moves, a real worker gate with owner source/settings edits, stale
completion rejection, retry, cancellation, missing/private dependencies and
reopen/maintenance. It adds no new authoring API or decoder.

12a adds `asset.gltf.inspect` and a tenth pure source-inspection case, extending
the independent inventory to 41 CPU surfaces plus one external GPU command.
The case consumes explicit immutable authored bytes and validates source counts,
references/digests and rejection paths while preserving the saved owner. Geometry
conversion, image decoding and cache publication retain the later glTF checkpoints.

12b adds `asset.gltf.convert` and an eleventh geometry-conversion case, extending
the independent inventory to 42 CPU surfaces plus one external GPU command.
It checks independent dense/sparse triangle values, bounds, shared mirrored
instances, local/world transforms, source identities and explicit rejection while
preserving the saved owner. Image decoding and material/cache integration retain
their separate checkpoints; conversion alone claims no rendered result.

12c adds `asset.gltf.textures` and a twelfth pure texture-conversion case,
extending the independent inventory to 43 CPU surfaces plus one external GPU
command. One inline original PNG exercises five core usages sharing three
semantic variants. Independent expected RGBA values distinguish sRGB filtering,
linear data averaging, straight alpha and cancelling-normal fallback. The case
retains sampler defaults/overrides, source image identity/digest, repeatability,
rejection and cancellation through the same saved owner. No external image path
or scratch directory is required. The five existing native filesystem failure
controls remain, while this pure case succeeds with an invalid scratch parent.
The FeatureWorld process family now has thirty checks. The texture family has a
90-second limit and seed 1337; the private decoder dependency family has a
60-second limit. Material/cache publication and rendering remain later owners.

13a adds optional Camera/MeshRenderer and their two shared commands, bringing the
then-current inventory to 51 CPU surfaces, one external GPU command and fourteen native
cases. The new connected component case and saved values extend preview/history/
retry/persistence behavior; see the [current guide](featureworld.md). Earlier
counts above describe the original checkpoints, not the current catalog.

13b adds three preparation services and a connected prepared-content case, bringing
the then-current inventory to 54 CPU surfaces and fifteen native cases. Core-only
fixtures validate bounded runtime bytes and ownership; the saved baseline now
uses the built-in cube. No GPU acceptance is inferred from preparation/loading.

13c adds one extraction service and the sixteenth native case, extending the
independent inventory to 55 CPU surfaces and one external GPU command. It reads
a prepared saved scene through the public CPU API and checks camera/origin math,
reflected bounds/normals, culling, invalid references and retained lifetime. The
process family has 34 checks; actual GPU drawing remains a later checkpoint.

13d adds `graphics.scene.render` as a second external GPU surface, verified by
its own actual native viewport and fault fixtures. The independent audit keeps
the existing SceneRunner GPU catalog mapping strict and separately requires the
viewport fixture, wrapper and portable oracle tests. The CPU FeatureWorld
catalog remains sixteen cases and 55 surfaces; Tools never links Client.
13e extends the same external surface with actual Runtime captures and seven
native replacement/destruction/reload/resize phases. Its independent audit requires
the Tools preparer, Client lifecycle fixture, Runtime supervisor and oracle tests;
see the [Runtime scene handoff](handoffs/13e-runtime-scenes.md).

14a adds `content.client-export` and the seventeenth saved-world case. The current
inventory is 56 CPU surfaces plus two external GPU commands, with 35 process
checks. It verifies preparation and revalidation through public Tools services,
independent saved-scene expectations and unchanged owners; package deployment
remains the later export checkpoints.

14b maps the separate `content.client-export.package` command to actual CPU
export processes and package controls. The independent audit checks that external
mapping separately from the 56 in-process surfaces and two GPU surfaces. It
preserves the 17 saved cases and does not treat CPU export as GPU qualification.

The fixture runs all seventeen saved-world cases through the public supervisor, then
the intentional failing assertion through the same executable/result path. It
compares stdout with create-only published results, native results and every
artifact's bytes/hash. Failure remains exit 1 with the original case, phase,
tick and entity context; CPU results report no renderer initialization.
Negative controls reject missing surfaces/tests/fixtures, stale registrations,
catalog drift, unavailable profile tests, substituted statuses and corrupt
artifact bytes. Original workloads, bounds, seeds and timeouts remain intact.
The versioned source stays untouched: execution uses a writable temporary copy
for ProjectFiles ownership. CTest discovery reads the actual generated test tree
through a private metadata directory so it cannot overwrite the enclosing run's
logs or checkpoint files.

Run from the repository root in the documented developer environment:

```text
ctest --preset windows-debug --show-only
ctest --preset windows-debug -R ^shark.tools.scene-qualification$ --output-on-failure
```

The CTest receipt is `tests/scene-qualification.json` under that build directory.
It embeds successful and failing execution snapshots before managed temporary
cleanup. It qualifies no GPU. Both actual Windows GPUs must separately pass the
existing explicit offscreen/window cases with required validation and unchanged
numeric oracles. Optional assignment-02 probe CLI declarations retain their
separate historical qualification; they are not silently counted as current
CTest executions.

The [testing policy](testing-policy.md) governs subsequent verification. Select
affected owners and consumers, reuse valid evidence and retain failures; do not
repeat the historical seven-profile closure or add source-freeze ledgers to each
component change. Current mappings must identify actual registered cases. Reuse
build/cache/userspace directories and remove successful temporary deployments.
This establishes local development coverage; MMORPG capacity, MMOWorld and actual
authored scene rendering retain their later owners.
