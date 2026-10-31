# 18b: authored bodies and native collider lifetimes

Date: 2026-10-31. Base: `96f1814` (`feat(physics): add private Jolt world
foundation`), initially clean `main`. Staging, commits and pushing remain the
user's work. **18b is complete under its headless scope.** Full REQ-18 remains
NOT_RUN; nonempty simulation, layers/events and connected editor acceptance are
later checkpoints. Desktop interaction remains paused.

## Delivered behavior and contracts

[ADR 0046](../architecture/0046-rigid-bodies.md) and the
[body contract](../physics-bodies.md) add one optional `RigidBody` containing
static/dynamic/kinematic motion, box/sphere/Y-capsule geometry, mass, friction
and restitution. EngineCore creates and observes real native shapes/bodies;
native IDs and SDK headers remain private. Body handles fence world identity,
slot and generation. Explicit remove and world teardown destroy bodies before
native services and the final registration lease, including after source Scene
destruction. Native publication has candidate cleanup on failure.

Scene enforces authored/authoritative roots, positive exactly uniform scale,
bounded local poses and scaled dimensions. Replica scenes reject bodies.
Render-only children and signed/nonuniform transforms retain their existing
support. Bodies are detached snapshots: source pose/settings/incarnation changes
reject observation until explicit remove/recreate. Names can change independently;
no Scene pointer is retained and no authored state is overwritten by observation.

Schema ID 5 (`shark.rigid_body`, version 1) and property IDs 13-20 expose all
eight fields, including kilogram units. Create and `SetRigidBodyCommand` share
the existing authoring owner, atomic candidates, preview, history and retained
outcomes. Capture/restoration and exact retry comparison preserve every field,
including inactive dimensions and floating input bits. History clears candidate
body constraints before restoring the target pose/settings, then publishes only
the complete valid state.

Source schema 1 gains the optional version-1 component with strict field/version
validation. Prepared scenes read versions 1 and 2; bodies use version 2 while
body-free scenes retain the original version-1 encoding. Core-only loading
preserves settings without a source importer. Terminal discovery, explicit query
fields and existing `scene.mutate` carry the same values; the 42-method inventory
does not grow. Real terminal pipes cover rejection, retry, undo/redo and native
project save/reopen. The eighteenth FeatureWorld case checks independent source
and native geometry expectations without altering its saved scene or IDs.

Nonempty `step()` rejects before native work and tick advancement. 18c must add
temporary-work admission and simulation pose ownership together; empty updates
remain real. No gravity/contact, arena-population capacity, collision performance
or MMO capacity qualification is claimed. Native inspector controls retain 18e;
the current widget skips the new schema rather than treating it as a renderer.

## Selected verification

Selection follows the [testing policy](../testing-policy.md): changed physics,
Scene/schema, authoring/persistence, prepared content/export consumers and real
terminal processes in Windows Debug; a portable seven-family WSL selection in
Debug and ASan+UBSan; protected Runtime/Server builds. Editor inspector and GUI
adapter checks are synthetic/headless, with no window or desktop interaction.
Logs below are relative to `out/18b-work/`.

| Selection | Result and evidence |
| --- | --- |
| Windows x64 MSVC Debug affected regression | All 27 unique selected families have passing results. `windows-tests.log/.xml` initially passed 25/26 in 46.73s; corrected schema and additional terminal discovery process pass 2/2 in 0.89s (`corrected-tests.log/.xml`). |
| Final Windows physics and rigid-body source rejection cases | PASS, 2/2, 0.11s; `body-final-tests.log/.xml`, after the exact source quaternion fence correction and final malformed-source cases. The subsequent GCC test-copy correction passes its one affected family in 0.06s (`body-corrected-tests.log/.xml`). |
| WSL Ubuntu GCC 13 Debug portable selection | PASS, 7/7, 3.72s; `wsl-final/linux-debug-tests.log/.xml`. |
| WSL Clang 18 ASan+UBSan portable selection | PASS, 7/7, 10.45s; `wsl-final/linux-asan-ubsan-tests.log/.xml`. Leak detection and vptr checks remain enabled. |
| Final WSL physics and rigid-body source rejection cases | PASS, Debug 2/2 in 0.03s and ASan+UBSan 2/2 in 0.11s; `wsl-body-corrected/linux-debug-tests.log/.xml` and `linux-asan-ubsan-tests.log/.xml`. |
| Protected Windows Release Runtime | PASS, final build; `runtime-body-final.log`. |
| Protected WSL GCC 13 Release Server | PASS, final build and boundary; `wsl-body-corrected/server-build.log`. |
| Repository, coverage/links, pinned formatting and diff | PASS; `repository.log`, `contract.log`, `format-check.log` and `diff-check.log`. |

The Windows selection covers physics/public example, schema, Scene/edit/query,
hierarchy, render components, prepared scene, rigid bodies, source documents,
AuthoredScene, authoring commands/batches/history/retries/integration/protocol,
editing/discovery processes, persistence integration, prepared content, client
export, FeatureWorld and its process, plus editor inspector/headless GUI adapter.
The WSL seven-family selection covers physics/public example, prepared scene,
rigid bodies, source documents, FeatureWorld and the editing process.

```text
tools/windows_dev.ps1 cmake --build --preset windows-debug --target CorePhysicsTests PhysicsPublicExample RigidBodyTests FeatureWorldTests SceneRunner SharkAuthor AuthoringProtocolTests --parallel 2
tools/windows_dev.ps1 ctest --preset windows-debug --show-only -R "^shark\.(core\.physics|tools\.rigid-body)$"
tools/windows_dev.ps1 ctest --preset windows-debug -R "^shark\.(core\.physics|tools\.rigid-body)$" --output-on-failure --no-tests=error
tools/windows_dev.ps1 cmake --build --preset runtime-only --target Runtime --parallel 2
```

The existing x64 MSVC wrapper selected the locked tools. Inventories were listed
before CTest selections. `wsl-commands.sh` and `wsl-body-final-commands.sh` retain
the WSL commands against canonical retained builds, native window/graphics OFF,
two jobs and unchanged host/WSL settings. Passing unaffected families were reused;
only changed/failing behavior reran. No bare CTest, whole-project CI or dependency
configuration matrix was selected.

Initial failures were corrected without weakening warnings or acceptance:
an aggregate initializer lacked a closing brace; the old command-variant count
and unknown-schema fixture needed the new command/schema IDs; terminal discovery
expected four components/twelve properties instead of five/twenty. GCC also
rejected a temporary-reference expression, an unused new test helper and an
unnecessary string copy in the final malformed-source loop. The borrow now has
an explicit lifetime, the unused helper was removed and the loop uses a const
reference. Original
diagnostics remain in `build-tests.log`, `build-regression.log`,
`windows-tests.log`, `wsl/`, `wsl-fixed/` and `wsl-body-final/`. An intermediate WSL wrapper referenced
a missing script before any engine work; its failure remains recorded separately.
Review also corrected source matching to retain the exact observed canonical
quaternion rather than a second normalization's rounded value; the native test
now uses a nonidentity rotation across all nine motion/shape combinations.

## Limits and next work

Proceed with **18c** in the [physics plan](../roadmap/physics-plan.md): qualify
bounded nonempty work before native updates, attach to the existing schedulers,
add gravity and static/dynamic/kinematic pose authority, and keep render-pose
interpolation separate. Preserve checked Scene/native lifetimes, exact authored
source fields and candidate/retry/history guarantees. Tick frequency and collision
substeps remain distinct. Layers/triggers/events retain 18d; queries/Lua retain
20, and streaming/origin transfer retains M05.

Native UI/rendering and connected body controls remain NOT_RUN under the
[desktop backlog](../backlog.md#desktop-ui-deferral). No windowed tests, GPU
qualification, package launch, hosted CI or new deployment evidence is claimed.
Prepared-format behavior is covered by CPU encode/load and export-consumer tests
plus the protected Runtime build; unchanged packaging, licenses and dependency
inputs do not trigger another package assembly/deployment matrix.

`.gitignore` was reviewed; existing build/out/cache/local-roadmap rules cover all
generated outputs, while new source/tests/docs remain eligible for versioning.
No dependencies, install rules, compiler flags or host settings changed. No
staging, commits or pushes occurred.

Suggested commit: `feat(physics): add authored rigid bodies and collider lifetimes`.
