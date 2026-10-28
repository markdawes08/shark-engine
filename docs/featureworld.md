# Saved FeatureWorld

14a adds the seventeenth native case, `featureworld.client-export`, following
[client export preparation](client-export.md). It checks the startup scene/camera,
built-in content, independent saved hierarchy/global anchor, owned bytes/digest,
repeatability and context/revision failures without changing the authoring owner.
The current inventory is 56 FeatureWorld CPU surfaces, one external CPU export
command and two external GPU commands. 14b's export process fixture consumes the
same saved source through a separate native project owner; it preserves this
in-process catalog.

13c adds the sixteenth native case, `featureworld.scene-extraction`, following
[CPU scene extraction](scene-rendering.md). It prepares the saved scene into a
separate loaded owner and checks camera matrices, reflected cube bounds/normals,
local-origin/global-anchor separation, culling, hidden invalid references and
snapshot lifetime across reload. The authoring owner and four saved IDs remain
unchanged. The earlier prepared-content case still covers built-ins, imported
shared/mirrored geometry and immutable loading. At 13c, coverage mapped 55 CPU surfaces
and two external GPU commands; these CPU cases do not initialize a renderer.
13d adds the separate [native viewport fixture](scene-viewport.md), keeping this
CPU catalog and its Core/Tools dependency boundary unchanged.
13e extends that external fixture coverage with [actual Runtime scenes](runtime-scenes.md):
a prepared FeatureWorld view, a fresh cube/plane project, imported geometry and
visible-entity lifetime checks. The saved source and CPU case inventory stay unchanged.

Checkpoint 10b connects the [CPU runner](scene-runner.md) to the versioned
[project](../assets/FeatureWorld/FeatureWorld.sharkproject) and
[scene](../assets/FeatureWorld/scenes/FeatureWorld.sharkscene), following
[ADR 0024](architecture/0024-headless-scene-runner.md). This small regression
fixture grows with supported components and APIs. It currently contains four
entities with mandatory Name/Transform and optional Camera/MeshRenderer, a three-level hierarchy, duplicate display
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

`--case featureworld` selects all seventeen cases in catalog order. Each concrete ID
also selects one case. Supply `--workspace EXISTING_DIRECTORY` for the suite or
persistence/asset-identity/asset-import/asset-jobs/asset-pipeline/render-components/client-export case. Each creates a unique child with its own
project/scenes and remove their known files on exit. Input source bytes are
preserved. A failed cleanup leaves inspectable remnants; it cannot delete the
caller-selected parent. Generated results belong under ignored `out/` or `build/`.

| Executable case ID | Actual assertions |
| --- | --- |
| `featureworld.components` | Independent saved values/IDs/metadata, schema versions, shared validation, bounded query pagination and Name/Transform/hierarchy inspection |
| `featureworld.commands` | Create, subtree delete, Name/Transform edits, both reparent modes, world/local poses, stale handle/revision rejection, invalid values/cycles and unchanged metadata |
| `featureworld.batches-history` | Pure preview, late-invalid atomic rejection, one-revision commit, grouped undo/redo, surviving handles and explicit history clearing |
| `featureworld.retries` | Fresh owner scope, sequenced mutate, all three Result levels, retained query/exact replay, payload mismatch and preservation of a later edit |
| `featureworld.persistence` | Actual native create/save, dirty acknowledgement, retained history, exact-content file revisions, codec round trip, reopen/reload and fresh incarnation |
| `featureworld.asset-identity` | Saved UUID reference, native typed sidecar publication, bounded revisioned catalog query/resolve, coordinated rename/move, missing source versus unknown ID, reopen and unchanged scene owner |
| `featureworld.asset-import` | Synchronous dependency imports, matching-key reuse, source invalidation, last-valid output, atomic cache publication, prune/reopen/reload/recovery and explicit clear through the same scratch ProjectFiles owner |
| `featureworld.asset-jobs` | Actual private-worker dependency imports, pending-work deduplication, bounded progress, owner-side publication, cancellation before/after publication, retained outcomes, explicit retry and closed admission through one scratch owner |
| `featureworld.asset-pipeline` | One persisted scene/reference through async publication, coordinated source/sidecar move, matching-key reuse, a gated worker concurrent with owner source/settings edits, stale completion rejection, explicit retry, parent/leaf key and byte checks, cancellation, missing/private dependency rejection, fresh-scope reopen and explicit cache maintenance |
| `featureworld.gltf-source` | Pure glTF source inspection over an explicit authored triangle snapshot: counts/default scene, contained parent URI, persistent external reference, independent byte digest, repeatability, missing/escaping sources, required-extension/animation errors and cancellation with unchanged AuthoredScene |
| `featureworld.gltf-geometry` | Pure geometry conversion over explicit authored triangle/sparse bytes: independently expected vertices, normals, triangle indices and bounds, shared ordinary/mirrored instances, local/world transforms, inverse-transpose normals, retained source/resource identities, repeatability, invalid scene/singular transforms and cancellation with unchanged AuthoredScene |
| `featureworld.gltf-textures` | Pure texture conversion over an inline original PNG and triangle: five core roles share three semantic variants; independent pixels, color/data/normal mips, straight alpha, default and explicit samplers, image identity/digest, repeatability, missing/truncated images, missing UVs, invalid scene and cancellation with unchanged AuthoredScene |
| `featureworld.gltf-assets` | Core material defaults/factors/alpha/culling, independent triangle/node values, deterministic engine-payload roundtrip, corrupt payload and cancellation rejection with unchanged AuthoredScene |
| `featureworld.render-components` | Saved perspective Camera and mesh reference; optional schema/query selection, orthographic/clipping/visibility edits, pure preview and late-invalid rollback, undo/redo, retained removal/replay/mismatch, deleted-entity recreation and actual save/reopen/reload |
| `featureworld.prepared-content` | Built-in cube/plane topology, prepared saved hierarchy/camera/global anchor, shared mesh ownership across deletion/reload, imported triangle/material factors and mirrored hierarchy, missing-asset and corrupt replacement rejection with unchanged authoring owner |
| `featureworld.scene-extraction` | Revisioned prepared-scene extraction, camera projection/view, independent reflected cube bounds/normals, local origin and global anchor, perspective/orthographic culling, hidden invalid references and retained snapshots through reload |
| `featureworld.client-export` | Startup scene/camera preparation, exact project identity, independently expected saved entities/hierarchy/global anchor, owned bytes/digest, complete built-in loading, repeatability, wrong-project/camera and stale-revision rejection with unchanged source/history/cache |

Every case receives a fresh owner from the same saved baseline. Functional work
occurs at tick zero; subsequent fixed ticks check stable identity/entity counts.
The fixture has no input actions and performs no RNG draws. Seed/rate/tick
context remains explicit in results; foundation cases separately exercise input
and seeded data. At three ticks, initial revision values are 4 and final values
are 4, 11, 7, 6, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4 and 4. Persistence and render-components
finish with fresh incarnations. The MeshRenderer reference is
`00000000-0000-4000-8000-000000000013`, built-in cube mesh zero, visible.
The component case still deliberately changes it to unresolved asset
`20000000-0000-4000-8000-00000000000c`, mesh seven/hidden, to prove field persistence
without mistaking a syntactically valid reference for a resolved resource. The
prepared-content case loads the valid baseline and separately rejects missing
assets. Its imported triangle uses the latter ID in an explicit immutable snapshot.
The asset reference in scene extras is `20000000-0000-4000-8000-00000000000a`;
it is preserved through native save/reopen and is not a new asset component.
Identity fixtures create their own authored source/metadata in scratch; no source
decoder success is inferred. The seventh native case exercises the focused byte
importer and dependency cache; follow the [cache contract](asset-import-cache.md).
The eighth native case moves the same byte-import path through bounded background
jobs and owner-side publication; follow the [job contract](asset-import-jobs.md).
Its progress waits yield with a fixed failure deadline and add no variable
assertion counts, keeping semantic repeated runs comparable.
The ninth case connects those services through one saved project and cache lineage.
An explicit native byte-import gate proves the owner edits a dependency while the
real worker is running; release occurs before service teardown on every exit.
Independent expected root/leaf bytes and dependency keys distinguish unchanged
source identity from changed content. The saved scene bytes and authored owner
remain unchanged through missing/private-source controls and cache reopen/clear.
The eleventh case converts the same captured-byte boundary into owned geometry
and node values. Independent dense/sparse triangles verify actual vertex/index
contents; a shared mesh retains its local winding across ordinary and mirrored
instances with different bounds and normal matrices. Geometry conversion performs
no file or Scene writes; see the [geometry contract](gltf-geometry.md).
The twelfth case converts one original 2x1 RGBA8 PNG used as base color,
metallic-roughness, normal, occlusion and emissive. Literal transparent-black and
opaque-white source pixels yield RGB 188 for color, RGB 128 for data and the
documented +Z fallback for cancelling normals; all one-pixel mips retain linear
alpha 128. It checks semantic sharing independently from source texture and
sampler identity. Default linear/repeat behavior and explicit nearest/clamp/mirror
fields remain per use. The inline PNG needs no test-fixture directory or image
file on disk. Invalid scratch parents therefore leave pure source, geometry and
texture cases successful while the original five native file-service cases retain their
original failures. Material/cache integration is qualified through the separate owner fixtures;
rendering remains a later owner. See the [texture contract](gltf-textures.md) and
[static glTF qualification](gltf-qualification.md).
The identity case keeps the [identity contract](asset-identity.md).
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
It now performs 35 checks. The independent coverage inventory contains
56 FeatureWorld CPU surfaces, one external CPU export command and two external
GPU commands; `asset.gltf.textures` maps to the
twelfth native case and the separate texture/dependency families. The texture
Catch family keeps a 90-second limit and seed 1337; its private dependency family
keeps 60 seconds.
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
