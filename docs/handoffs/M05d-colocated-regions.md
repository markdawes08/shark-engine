# M05d: co-located regions and continuous gameplay

Date: 2026-11-06. Base: `8722c42`, initially clean `main`. The user owns staging,
commits and pushing. Follow the [regional contract](../region-world.md),
[ADR 0066](../architecture/0066-colocated-regions.md),
[projectile contract](../authoritative-projectiles.md),
[cell authoring](../world-cell-authoring.md), [M05 checkpoints](../roadmap/seamless-world-plan.md)
and [testing policy](../testing-policy.md).
M05d PASS under its co-located current-machine functional profile. Full REQ-M05
remains NOT_RUN; M05e is next.

## Delivered

RegionWorld privately owns the existing movement, combat and interest services
over one shared Scene, PhysicsWorld, traversal frame and actual character/session
authority. Two to eight nonoverlapping edge-connected half-open XZ rectangles
identify regional writers. Checked process-local grants gate gameplay, interest
and economic-operation ingress before shared session sequence admission. A
completed native tick publishes fresh grants for crossing actors, without
rebinding their session, entity, body or movement/combat streams.

Movement checks its full intended segment against the regional union, including
concave gaps. Missing regional/content coverage consumes an input as a horizontal
stop. Up to sixteen precreated dynamic NPCs use the same traversal and native
tick, with one bounded horizontal intent each; pursuit progress is adopted only
after observing committed native movement. Native writes followed by rejection
retain their partial-effect report and fault the regional owner.

InterestServer retains one catalog and replica domain across region edges.
Runtime `present` controls normal despawn without erasing disclosure policy;
privacy revocation still closes a previously disclosed stream even while absent.
CharacterAuthority's narrow interest adapters retain its private SessionAuthority
and durable-world boundary. Required visible gameplay actors cannot be silently
dropped to satisfy the existing 32-replica limit.

Targeted point projectiles extend CharacterCombat, with server Lua kind 3,
10 m/s straight flight, one 0.5 m current-native ray per 20 Hz tick, twenty flight
ticks and one retained flight per actor. Launch charges cooldown; final collision,
expiry or unavailable-participant outcome retains the original action. Damage,
death and cleanup share the existing partial-effect/lifecycle path. Combat v2
adds projectile actions/outcomes; ordinary v1 output bytes stay unchanged and
decoders accept both. No flight animation, homing, historical rewind or durable
reward is implied.

The retained authoring owner exposes `scene.cell.prepare`, `scene.cell.export`
and `cell.inspect`. Export rechecks exact source/catalog/cache inputs, requires
the prepared SHA256 and publishes create-only `.sharkcell` bytes through existing
ProjectFiles staging/rename. Target audience and complete fresh mesh closure are
checked, including hidden references. Explicit selected transformed geometry is
baked; Camera/Script source rejects. Files reference ordinary prepared assets,
which are not embedded. Inspection observes current bytes, not the original lost
publication/durability receipt. The World cells panel calls those same commands.

## Connected coverage

The actual mTLS/Store/native fixture crosses a cell seam and a distinct regional
edge. Two movement inputs admitted before crossing survive the new grant. Tests
check unchanged session/ownership epoch, entity/body, movement/combat streams,
health/cooldown and both real InterestServer recipient baselines. A later delta
keeps replica IDs without spawn/despawn. Old grants reject without consuming the
next command sequence.

An original pending economic transfer crosses ownership with its exact ticket,
request and OperationId. Another actor cannot take it; the new grant receives
the committed 93/18 and 107/22 balances, and an exact retry returns the original
receipt. This checks identity continuity through the real worker, not a frozen
SQL cut or distributed stale-writer fence.

A retained projectile crosses ownership while the actor moves. Actual TLS combat
state decoding and CombatClient application produce one impact and one
presentation effect; duplicates emit none. Separate native combat cases cover
wall blocking, miss/expiry and stale life/session/body behavior. A real native
NPC follows the authored cell route, changes regional grant once and retains its
body and public replica. This regional NPC fixture uses normal gravity and real
floor contacts over 70 controlled ticks; the other regional gameplay fixtures
retain M04's explicit zero-gravity functional profile. Neither is a capacity claim.

Public terminal process coverage independently parses exported envelope bytes,
SHA256 and geometry. CPU panel/protocol cases exercise source/history preservation,
stale inputs, audience/freshness rejection, typed paths and publication/durability
faults. Actual native World cells interaction remains NOT_RUN under
[BL-M05D-UI-01](../backlog.md#deferred-items) and the user's manual UI pause.

## Verification

The 21 selected Windows Debug CTests and 20 selected retained WSL Clang ASan+UBSan
CTests PASS across the initial and corrected final selections. The WSL inventory
excludes native ImGui; its extra Lua selection passed in the final focused run.
These are unique selected owners, not a whole-project inventory. Exact commands,
logs and JUnit are under `out/M05d-work/`:

| Selection | Result | Evidence |
| --- | --- | --- |
| Windows initial affected owners | 19 PASS, 2 FAIL, 16.80 s | `windows-build-final.log`, `windows-selected-inventory.log`, `windows-tests.log`, `windows-tests.xml` |
| WSL initial affected owners | 17 PASS, 2 FAIL, 26.08 s | `wsl/build.log`, `wsl/inventory.log`, `wsl/tests.log`, `wsl/tests.xml` |
| Windows final traversal/movement/combat/lifecycle/regions | 5/5 PASS, 4.90 s | `windows-ground-fix-build.log`, `windows-ground-fix-inventory.log`, `windows-ground-fix.log`, `windows-ground-fix.xml` |
| WSL final same owners plus Lua | 6/6 PASS, 10.60 s | `wsl-ground-fix/build.log`, `wsl-ground-fix/inventory.log`, `wsl-ground-fix/tests.log`, `wsl-ground-fix/tests.xml` |

Windows builds the changed native Editor without launching it, plus Tests,
CoreLuaTests, ServerMovementTests, AuthoringWorldCellTests, SharkAuthor,
EditorGuiTests, EditorProjectSessionTests, ProjectFilesTests, AuthoringProtocolTests,
CorePhysicsTests, ScriptRuntimeTests, RuntimeSpawnTests, WorldTraversalTests,
WorldCellToolsTests and Server. The matching `windows-debug` presets run through
`tools/windows_dev.ps1`, with two build jobs. WSL reuses
`/work/build-05a-final-client/linux-asan-ubsan` in the retained isolated userspace,
with two jobs, leak detection and ASan/UBSan halt-on-error. No host/WSL settings
changed. The initial WSL target list incorrectly included Windows-only
EditorGuiTests; that selection failure is retained and corrected, not a behavior
pass. A later driver-path typo also failed before building; its log is retained.
`windows-final-link.log` records final application/test relinking after the
traversal correction; its unchanged consumers reuse their valid behavior results.

The initial selection (Windows shown) was:

```text
^shark\.(core\.(lua|combat-rules|combat-wire|combat-client|net-interest|net-replication|physics|script-runtime|runtime-spawning|world-traversal)|tools\.(world-cell-authoring|world-cell|authoring-protocol|authoring-protocol-process|project-files)|editor\.(project-session|gui-adapter)|server\.(regions|movement|combat|gameplay-lifecycle))$
```

Inventory precedes each selection; CTest uses `--output-on-failure --no-tests=error`
and JUnit. New CTest families retain seed 1337 and 60-second timeouts. Contract,
repository/ignore/link checks and diff review pass. No new framework or
source-freeze ledger was added.

Failures were investigated rather than retried unchanged. The first Windows
compile found a missing fixture initializer brace; the next found Catch/JSON
scalar comparison ambiguity. Both are corrected with explicit values
(`windows-build.log`, `windows-build-corrected.log`). The initial behavior run
found a projectile fixture selecting an already revoked actor; it now reads the
survivor's own health without that forbidden visible target.

The regional NPC fixture exposed a real portal stall: cursor-relative clamping
and graph-frame arithmetic rounded differently, and a rounded exact endpoint
could retain its preceding segment index. Traversal now uses the same frame
subtraction/length as navigation and skips only exact zero segments before
offering one nonzero segment. A direct endpoint regression extends the existing
traversal fixture. Intermediate failed results remain in `windows-traversal-fix*`
and `windows-final-region*` / `wsl-final/`.

After that fix, the route and all regional identity/replica checks passed, but
the new ground-height assertion exposed an unsuitable zero-gravity fixture:
small native upward contact velocity accumulated without gravity. The NPC
fixture now uses normal gravity and ground wide enough for the stationary players,
retaining the route, 70 ticks and existing position/height tolerances. The final
five/six owners pass. No tolerance was widened or workload shortened. The other
initial passes remain valid on unchanged relevant inputs; no broad rerun followed.

No GPU, manual UI, Release/package or whole-project run was selected. The changes
are CPU authority, native physics queries, local file publication and authoring
adapters, without renderer/dependency/package changes. M04e's unchanged shaped
separate-process traffic evidence is retained; M05d does not claim new distributed
qualification. Final cooked cell packaging remains outside this increment.

## Continuation

M05e is next: consistent prepare/cut/commit state, final tick/input and original
flight/durable-operation state, plus stale-owner fencing at mutation and
persistence boundaries. Co-located grants are not distributed leases or persisted
fencing tokens. M05f owns actual separate-process routing and crash/partition
recovery; M05g connects the full authored continuous-world gate. Full REQ-M05
remains NOT_RUN.

Keep the borrowed Scene/native/traversal/session owners alive through regional
owner destruction, then close traversal before physics/residency. NPC records are
retained for this owner's lifetime; registration is not a spawn/despawn service.
Fixed local frames and development cell content remain explicit. Final cooked
cell/dependency packaging remains 31/M08. `.gitignore` covers the default generated
`World.sharkcell`, outputs, caches and local roadmap; source fixtures remain
eligible. Root README is unchanged.
