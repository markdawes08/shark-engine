# M05c: static world collision, navigation and readiness

Date: 2026-11-06. Base: `f0ed443`, initially clean `main`. The user owns staging,
commits and pushing. Follow the [traversal contract](../world-traversal.md),
[ADR 0065](../architecture/0065-world-traversal.md),
[M05 checkpoints](../roadmap/seamless-world-plan.md) and
[testing policy](../testing-policy.md).
M05c PASS under the declared current-machine authored-surface profile. Full
REQ-M05 remains NOT_RUN; M05d is next.

## Delivered

Core accepts optional explicit cell-local collision/navigation geometry in cell
envelope v2. Geometry-free v1 bytes remain identical and readable. EngineTools
captures an exact authored revision, resolves explicit mesh selections and bakes
full hierarchy transforms into the bounded exact grid, preserving reflection
winding. No source/history/import state changes or source importer enters Core.

The existing leased PhysicsWorld creates real static MeshShapes with checked
region proxy identities, native geometry limits and original body/contact/query
generation fences. Proxies have no authored RigidBody. PhysicsBodyState now makes
settings optional and reports geometry/filter/material/bounds explicitly; existing
primitive component/source/wire representations remain unchanged. ScriptRuntime
accepts owned mesh contacts while rejecting primitive body recreation for meshes.

Residency provides at most 32 exact pins. WorldTraversal pins the selected content,
prepares a complete immutable navigation candidate and creates collision before
publishing readiness. Replacement/eviction cannot discard active content.
Deactivation removes native collision before releasing pins and invalidates old
paths. Native uncertainty retains the original failure and requires explicit
physics close followed by pin release/Scene discard; there is no claimed rollback.

Bounded A* joins exact canonical edges across at most eight authored tiles and
2,048 walk triangles. Endpoints require surface membership; graph/frame/path
identities fence cursors. Portal routes and bounded steering offers preserve
corners. The initial profile is upward, single-layer centerline navigation with
authored clearance, not an automatic radius/voxel bake or stacked-floor routing.

Authoritative movement declares its optional traversal instance at construction.
Every movement/combat tick requires that owner, origin and exact Scene/PhysicsWorld
context before effects. A missing swept-cell footprint stops horizontal movement
while consuming the admitted input; loading content cannot turn that input into a
later authority grant. Legacy primitive mode remains explicit and source-compatible.

## Connected observations

The native fixture loads two cells at global x cell 1,000,000,000, activates their
actual triangles and follows a six-meter seam route with an owned sphere. It runs
20 settling ticks and 80 pursuit/settled ticks at 20 Hz with three collision
substeps, checking committed horizontal motion, ground height, contacts on both
cells and actual ray hits. Cursor progress is adopted only after checking native
movement. Missing readiness rejects the route; removing a tile invalidates its
native handle/proxy and old paths. Pinned replacement and eviction preserve the
original live content until explicit release.

An actual mTLS/Store character fixture proves omitted/replaced/late traversal
binding rejects before the pending input or native tick changes. The first
admitted input is acknowledged as a stop with an unavailable neighbor; a new
input moves after neighbor activation through CharacterCombat's normal host path.
That fixture qualifies admission, while the separate native fixture qualifies
ground contact. Direct negative context fixtures reject foreign Scene/native
owners and closed traversal owners without duplicating the TLS fixture.

## Verification

Fourteen selected CTests PASS on Windows Debug and retained WSL Clang ASan+UBSan.
Protected Server/Core dependency checks and the public physics example PASS.
The exact-context additions also PASS in the focused final selections below;
these repeat four owners and are not additional unique CTests.

| Selection | Result | Evidence under `out/M05c-work/` |
| --- | --- | --- |
| Windows affected owner inventory | 14/14 PASS, 5.04 s | `windows-build.log`, `windows-inventory.log`, `windows-tests.log`, `windows-tests.xml` |
| WSL affected owner inventory | 14/14 PASS, 9.14 s | `wsl/build.log`, `wsl/inventory.log`, `wsl/tests.log`, `wsl/tests.xml` |
| Windows final traversal/movement/combat/lifecycle | 4/4 PASS, 3.65 s | `windows-final-build-corrected.log`, `windows-final-inventory.log`, `windows-final-tests.log`, `windows-final-tests.xml` |
| WSL final traversal/movement/combat/lifecycle | 4/4 PASS, 8.27 s | `wsl-final/build.log`, `wsl-final/inventory.log`, `wsl-final/tests.log`, `wsl-final/tests.xml` |

Both build WorldTraversalTests, CorePhysicsTests, ScriptRuntimeTests,
RuntimeSpawnTests, WorldCellResidencyTests, WorldCellTests, WorldCellToolsTests,
PreparedSceneTests, PreparedAssetsTests, PreparedContentTests, ServerMovementTests,
Server and PhysicsPublicExample with two jobs. Windows uses
`tools/windows_dev.ps1` and matching `windows-debug` build/test presets. WSL reuses
`/work/build-05a-final-client/linux-asan-ubsan` in the retained isolated userspace,
with leak detection and ASan/UBSan halt-on-error. No host/WSL settings changed.
Inventory precedes selection, `--output-on-failure --no-tests=error` and JUnit:

```text
^shark\.(core\.(world-traversal|physics|physics-public-example|script-runtime|runtime-spawning|world-cell-residency|world-cell|prepared-scene|prepared-assets)|tools\.(world-cell|prepared-content)|server\.(movement|combat|gameplay-lifecycle))$
```

The new traversal family and native mesh additions use the existing seed 1337,
60-second owner timeouts and controlled explicit native ticks. No arbitrary
sleep, new framework, image baseline or source-freeze ledger was added.
The conservative Update arena formula is retained after reviewing the pinned
static mesh visitors and existing capped pair/contact buffers; geometry has
separate vertex/triangle limits. Reservations are logical bounds, not RSS or
LOCAL-MMO-1 qualification. No behavior check failed.

Final review tightened readiness to the exact Scene and PhysicsWorld, in addition
to retained traversal instance/origin. The first rebuild found a missing complete
WorldTraversal include in combat.cpp on both compilers (`windows-final-build.log`,
`context-build-failure.log`); the include is fixed and final compilation/behavior
pass. Only the affected four owners were rerun. The other ten selected results
remain valid on unchanged relevant inputs. No workload/tolerance was changed to
obtain a pass.

No manual UI, GPU, Release/package or whole-project run was selected. The changed
owners' complete physics, script/spawn, source-content and actual-session movement,
combat/lifecycle regressions cover their behavior. M04e's unchanged process/wire
adversity evidence is retained rather than repeating its 53-second transport
profiles for new local content activation. No dependency or host setting changed.

## Continuation

M05d is next: adjacent regions within one process, overlapping interest, continuous
travel and cross-boundary gameplay/durable fixtures. Carry cell geometry through
the retained human/terminal authoring adapters before full M05 acceptance; the
shared preparation service does not advertise a new terminal method or editor
panel. Cross-region projectiles, transfer cuts/fencing/recovery and connected
qualification remain M05d-g. Full REQ-M05 remains NOT_RUN.

Keep native/Scene/residency owners alive through traversal close. Fixed local
frames are explicit; live native origin rebasing is not claimed. Preserve the
single-layer authored navigation profile and original failures. Independent
manual UI/backlog statuses are unchanged. `.gitignore` already covers generated
build/output/cache and local-roadmap data; sources and fixtures remain eligible.
README is unchanged.
