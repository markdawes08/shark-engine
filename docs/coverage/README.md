# Requirement and API coverage

[requirements.json](requirements.json) is the version-3 requirement/coverage manifest for
Shark. Each requirement has a unique ID, one owning roadmap assignment, observable
acceptance criteria, a planned FeatureWorld case or external fixture, and an
honest verification state. Related assignments can contribute without changing
which assignment owns the gate.

Assignment 03 is split into [03a/03b/03c](../roadmap/core-plan.md). The implemented
Core diagnostics/configuration and identity/path features map to real unit and
process fixtures. The [03a handoff](../handoffs/03a-core-contracts.md) records their
local results. The [03c handoff](../handoffs/03c-time-and-work.md) closes full
`REQ-03` with final local Windows/WSL regressions, separate clock policies,
bounded completions and callback/thread rules.

The 03b [application lifecycle](../application-lifecycle.md) feature adds
`shark.core.lifecycle` and extends the real Server process path. Its
[handoff](../handoffs/03b-application-lifecycle.md) records native/injected signal
ownership, partial-start rollback, repeated cycles and cleanup-failure evidence.
The [03c timing/work](../time-and-work.md) features map to `shark.core.timing`
and `shark.core.work`, using controlled clocks and real thread fixtures. No world
simulation or production worker is inferred from those foundation tests.

Assignment 04 follows the [window/input checkpoints](../roadmap/window-input-plan.md).
The `client.raw-input` feature maps 04a's device-free state to `shark.client.input`
in client-enabled builds; headless Server builds exclude it. The
[04a handoff](../handoffs/04a-input-contracts.md) preserves its CPU evidence.
04b's `client.native-window` feature adds owned GLFW lifecycle, CPU backend/CLI
fixtures and explicit Windows smoke evidence in the
[04b handoff](../handoffs/04b-native-window.md). The `client.offline-frame-loop`
feature maps 04c's frame/tick input, pause/step and shared sample pump to
`shark.client.frame`, window fixtures and Core timing tests. The
[04c handoff](../handoffs/04c-frame-loop.md) records its final qualification.
Full `REQ-04` is PASS under the current-machine gate: all three checkpoints are
complete, with final evidence in [04c-local.json](evidence/04c-local.json).

The user selected a seamless open-world MMORPG. The
[versioned execution plan](../roadmap/mmorpg-plan.md) and
[ADR 0007](../architecture/0007-mmorpg-architecture.md) expand the original 00-35
assignments with M01-M10. There are 50 requirements across 46 ordered assignments.
`assignment_order` records dependency order; M01 follows 01 and precedes 02, and
the validator checks order rather than comparing IDs lexically. The product
profile and plan path are explicit machine-readable fields.

[ADR 0008](../architecture/0008-human-agent-authoring.md) and the
[human/terminal-agent plan](../roadmap/human-agent-plan.md) strengthen existing
owners and add `REQ-CROSS-HUMAN-AGENT` (audit owner 32). Shared services, terminal
discovery/results, one project owner and semantic editor/CLI parity are required
as their layers land; 33/M09 add fresh terminal and human continuation evidence.
This adds no assignment or implemented API and does not change schema 3.
Historical PASS records are retained; documentation checks do not qualify the new
authoring gates. The later [ADR 0009](../architecture/0009-windows-linux-platform-scope.md)
removes Mac requirements and retires 02c. The user then limited the project to
resources already available on this PC in
[ADR 0010](../architecture/0010-current-machine-scope.md). Active targets are the
native Windows client/editor on its RTX 4070 Laptop and Intel Raptor Lake-S Mobile
GPUs, plus CPU/headless-server and isolated package checks in the existing WSL2
Ubuntu environment. Native Linux graphics, other hardware and independent clean
physical machines are outside the current gate. Their historical NOT_RUN records
remain unchanged.

`qualification_profile: current-machine` and `scope_document` record that scope
as additive schema-3 fields. REQ-02 is PASS for the amended local gate, with
explicit scope and SHA-256-pinned references to the unchanged 02d receipts.
The validator verifies those references and retained bytes; it does not rerun
graphics or interpret an old aggregate NOT_RUN as a universal PASS.
M03a maps `server.durable-store` to real-file unit/process tests and locked
dependency checks. Its [contract](../durable-store.md) and
[six-checkpoint plan](../roadmap/durable-state-plan.md) distinguish the foundation
from character, economy, recovery and migration gates. M03a is PASS under the
current-machine scope: [the receipt](evidence/M03a-durable-store.json) and
[handoff](../handoffs/M03a-durable-store.md) record 525 full CPU CTests, ten
additional standalone Tools checks and protected package/API gates.
Full REQ-M03 is now PASS after all six checkpoints and integrated qualification.

M03b maps `server.durable-characters` to real database records, shared session
admission and authenticated process restart. The
[character contract](../durable-characters.md) defines bounded enrollment and
revocation, immutable ownership/identity and roster/character revision conflicts.
Fixture-only local discovery does not add a player roster protocol. Economic
transactions, full runtime recovery and migration/restore remain separate gates;
FeatureWorld and MMOWorld attach in 10/M09. M03b is PASS under the current-machine
scope: [the receipt](evidence/M03b-owned-characters.json) and
[handoff](../handoffs/M03b-owned-characters.md) record 539 full CPU CTests,
ten separately counted standalone Tools checks and protected package/API gates.
M03c atomic transactions are also PASS under the current-machine scope;
[the handoff](../handoffs/M03c-atomic-transactions.md) and
[receipt](evidence/M03c-atomic-transactions.json) record 553 full CPU CTests, ten
standalone Tools checks and protected package/API gates. M03d is also PASS:
its [handoff](../handoffs/M03d-runtime-recovery.md) records 567 full CPU CTests
and 10 standalone Tools checks with protected gates. M03e is also PASS:
its [handoff](../handoffs/M03e-migration-restoration.md) records 581 CPU CTests
and 10 standalone Tools checks with protected gates. M03f and full REQ-M03
are PASS: the [handoff](../handoffs/M03f-integrated-qualification.md) records
588 CPU CTests and 10 standalone Tools checks with protected gates.
Assignment 09, shared authoring commands and undo/redo, is next.

`current_assignment: 09` follows the completed durable-state gate and
[five network checkpoints](../roadmap/network-plan.md) and
[four persistence checkpoints](../roadmap/persistence-plan.md). Project/scene
checkpoints 08a-08d and full REQ-08 are PASS under the current-machine scope.
The `core.network-foundation` feature maps M02a's bounded TLS/wire/fault APIs to
three unit families and a real-process encrypted fixture, plus exact dependency
and package checks. M02a is PASS: the [handoff](../handoffs/M02a-transport-wire.md)
and [receipt](evidence/M02a-transport-wire.json) record 404 passing CPU CTests
without failures/skips, protected builds and isolated packages. The narrow
postqualification CI compiler-forwarding amendment is recorded separately from
the unchanged compiled inputs. The prior
[07 checkpoint plan](../roadmap/scene-plan.md) is complete.
M02b's `core.owned-sessions` feature maps mutual TLS, explicit enrollment, typed
owned session controls, expiry/revocation and reconnect fencing to two new unit
families and a separate-process fixture. Its [contract](../network-sessions.md)
and [ADR](../architecture/0012-owned-sessions.md) separate session authority from
authoring, administration and future gameplay. M02b is PASS: the
[handoff](../handoffs/M02b-owned-sessions.md) and
[receipt](evidence/M02b-owned-sessions.json) record 425 passing CPU CTests, no
failures/skips, protected builds and isolated package checks. Later M02
checkpoints qualify replication, interest and integrated adversity below.
`core.scene-ownership` maps checked scene/entity lifetime, deferred mutation,
world coordinates and the private EnTT boundary to executable CPU fixtures.
The [07a handoff](../handoffs/07a-entity-ownership.md) records checkpoint results.
07b's `core.transform-hierarchy` feature and `component.name` / `component.transform`
entries map mandatory values, validated hierarchy and mutation behavior to
`shark.core.hierarchy`, `shark.core.transform` and their dependency fixtures.
07b is complete under the current-machine scope. The
[handoff](../handoffs/07b-transform-hierarchy.md) and
[receipt](evidence/07b-transform-hierarchy.json) record **362 passing CPU CTests
with no skips**: Windows Debug/Release/ASan 60 each, WSL client Debug/ASan+UBSan
53 each, and WSL Server Debug/ASan+UBSan 38 each. Protected Runtime/Server builds,
dependency/license closure and isolated package execution also pass. The
[transform contract](../transforms.md) specifies the delivered C++ behavior.
07c implements [fixed component schemas and bounded revisioned inspection](../scene-inspection.md).
`shark.core.schema` and `shark.core.scene-query` cover metadata/default agreement,
pure validation, invalid IDs/versions, bounded results, stale revisions and owned
snapshot lifetime. The [07c handoff](../handoffs/07c-schema-inspection.md) records
the completed gate. Full REQ-07 is PASS under the current-machine scope. The
[07c receipt](evidence/07c-schema-inspection.json) records **376 passing CPU
CTests with no failures/skips**: Windows Debug/Release/ASan 62 each, WSL client
Debug/ASan+UBSan 55 each, and WSL Server Debug/ASan+UBSan 40 each. Schema coverage
has 9 cases/654 assertions and query coverage 19 cases/3,916 assertions. Protected
Runtime/Server builds, source/link/license closure and fresh isolated package
checks pass; all 221 qualification source inputs were unchanged in that record.
M02a-M02e and full REQ-M02 are PASS under the current-machine scope.
Assignment 08, Project and scene persistence, is complete: 08a-08d and full
REQ-08 are PASS. M03 and all six checkpoints are complete; assignment 09 is next.
FeatureWorld integration remains assignment 10.
The user's sequencing amendment moved remaining 05c work to the
[nonblocking backlog](../backlog.md), preserving historical results and `REQ-05`
NOT_RUN. Existing presentation/device fixtures and their earlier handoffs retain
scope-specific evidence; backlog status is not a retroactive qualification pass.

`tooling.offline-shaders` maps the [06a shader builder](../shader-pipeline.md) to
CPU and explicit real-tool fixtures. `client.gpu-resources` maps the
[06b resource contract](../gpu-resources.md) to typed shader/upload validation,
CPU ownership tests, native fault children and independent color/depth readbacks.
The [06b handoff](../handoffs/06b-gpu-resources.md) records scoped results on the
current Windows GPUs and device-free Windows/WSL regression. These fixtures
exercise the production device without exposing native handles or invoking a
runtime compiler. Full `REQ-06` is PASS after 06c window integration and distribution qualification;
see the [06c handoff](../handoffs/06c-window-distribution.md). FeatureWorld connects these isolated
fixtures in assignment 10.

`planned_fixture` describes future evidence, not an existing test. `NOT_RUN`
means precisely that; documentation validation cannot pass a future engine gate.
The `implemented_surface.features` entries map the bootstrap behavior and runtime
build boundary, server lifecycle, Server/Core closure and graphics setup/executable cases to their actual tests
and external fixtures. Name and Transform have implemented component entries with
C++ validation/mutation fixtures, fixed schemas and implemented source codecs.
Authoring command and Lua arrays remain empty because those adapters do not
exist yet. Filesystem services retain the remaining persistence checkpoints.
Assignment 10 connects supported scene surfaces to executable FeatureWorld
coverage; 32 audits completeness. Do not register placeholder APIs.

An implemented entry identifies its stable name, owning requirement IDs, actual
automated test IDs, test source paths and FeatureWorld or justified external
fixture paths. Coverage registration is not proof of correct behavior;
those tests must run with recorded platform and PASS/FAIL/NOT_RUN evidence.

Run from the workspace root:

```powershell
python tools/validate_contract.py
python tools/validate_contract.py --self-test
```

The check rejects malformed/duplicate IDs, missing owners or criteria, missing
assignment coverage, unsupported verification claims, missing ADRs, and broken
relative Markdown document links. Its negative cases confirm that omissions
actually fail. It does not parse natural language to prove all requirements are
present; the handoff records a separate review against the source roadmap.
Local-scope negative cases reject absent/altered scope and evidence references,
and retain rejection of future PASS claims after assignment 02 closes.

Schema 2 added `current_assignment` and structured implemented-surface entries to
schema 1. Schema 3 adds the MMORPG product/plan fields and ordered expanded owner
set. This planning manifest was migrated directly; the checker explicitly rejects
earlier/future versions, missing MMO owners, invalid ordering and unsupported
future PASS claims. No project/scene/runtime/wire/database data format has been
implemented or migrated by this amendment. M01 adds implemented server lifecycle
and dependency-isolation features; registration alone does not pass their gate.

The ignored local roadmap is amended for consistency but remains optional context,
not a CI input. REQ-00's PASS records its historical planning evidence, not MMO
implementation. REQ-01 is PASS: the repaired hosted CPU matrix and required
deliberate-failure/red-to-green proof are verified at the recorded historical
revision. REQ-M01 is PASS. Its
implementation and local qualification are recorded in the
[M01 handoff](../handoffs/M01-headless-server.md). The
[local receipt](evidence/M01-local.json) records Windows 45/45 CTests,
Ubuntu 26/26 restored CTests, the expected injected sentinel failure and package
closure/lifecycle evidence. The [hosted follow-up](../handoffs/M01-ci-verification.md)
and [gate receipt](evidence/M01-ci-gate.json) record the historical fifteen jobs passing,
206/206 restored CTests, the expected thirteen injected sentinel failures,
Docker/package closure and the then-required fifteen checks. The current Windows/Linux
CI policy is documented in [CI](../ci.md). Existing hosted jobs and branch
protection remain unchanged; hosted runs are supplemental to local roadmap gates.
Assignment 02 delivered setup, executable GPU/validation fixtures and probe packages.
Native Mac 02c is retired by ADR 0009; the earlier
[deferral](../handoffs/02c-mac-deferred.md) remains a historical record.
The [02b handoff](../handoffs/02b-graphics-cases.md) separates local evidence from
the hardware matrix required at that time. The [graphics checkpoint plan](../roadmap/graphics-plan.md)
and [probe guide](../graphics-probe.md) distinguish setup diagnostics from the
current local hardware gate.
The [Windows 02d handoff](../handoffs/02d-windows-validation.md) and
[receipt](evidence/02d-windows-local.json) record the portable package, supplemental
NVIDIA full soak, Intel short cases and retained failures at their recorded input
hashes. Those receipts retain their original nine-row scope. The later six-row
Windows/Linux matrix and independent clean-machine results also remain NOT_RUN
in their original evidence; ADR 0010 removes those unavailable targets from the
current plan. The
[final local handoff](../handoffs/02d-final-verification.md) records the current
package/link changes, refreshed Windows evidence and isolated Ubuntu
CPU/package checks. The [hosted audit](evidence/02d-hosted-ci.json) verifies nine
jobs and 156 CTests at `39110d4`; it does not cover the subsequent 02d changes.
The [scope amendment](../handoffs/current-machine-scope-amendment.md) reassesses
the [Windows refresh](evidence/02d-windows-refresh.json) and
[Linux local](evidence/02d-linux-local.json) receipts at `2ef281b`. Both actual
Windows GPUs passed the complete 100 resize / 20 device / 600-second lifecycle
and readback/validation cases; local package isolation and failure checks passed.
This closes REQ-02 only for LOCAL-W-NV and LOCAL-W-INTEL plus the stated local
package/CPU evidence. It is not a new run, production renderer or general Windows
support claim. Future renderer work revalidates those devices as features land.

Future packaging, release and MMORPG gates use this PC, separate real local
processes, the actual local store, shaped network faults and existing WSL
userspaces. ADR 0007's provisional LOCAL-MMO-1 profile uses 16 active synthetic
players, 16 NPCs, two region processes, 32 visible animated actors, 100 crossings
and a two-hour active soak. The two real graphical-client semantic test runs
separately from synthetic load. These are planned tests, not achieved capacity;
external hosting, multihost failover, WAN testing, paid services and public signing
credentials are not prerequisites. Human/terminal parity and seamless authoritative
world behavior remain required. Multiplayer requirements remain NOT_RUN. See the
[CI verification handoff](../handoffs/01-ci-verification.md) for 01's tested
revision, run URLs and qualification limits.

Full `REQ-06` is PASS under the current-machine scope. The
[06c handoff](../handoffs/06c-window-distribution.md) and
[receipt](evidence/06c-window-distribution.json) record 261 CPU CTests, both-GPU
window/offscreen references, lifetime failures, shader rebuilds and relocated
Runtime shader closure. `client.window-cube-distribution` maps the implemented
surface to device-free tests and explicit hardware fixtures. 07a introduces the
CPU scene foundation; 07b implements Name/Transform and hierarchy. 07c adds fixed
schema metadata, shared pure validation and bounded revisioned CPU inspection.
These C++ APIs do not claim source persistence, terminal/editor adapters or Lua
bindings. Those layers retain their later assignment owners and planned fixtures.

M02c adds `core.authoritative-replication`, mapped to strict wire, service and
real-process fixtures. Its [handoff](../handoffs/M02c-authoritative-replication.md)
and [receipt](evidence/M02c-authoritative-replication.json) record 446 passing CPU
CTests and protected package checks. The public field allowlist is independent
of authoring schemas. M02d [spatial interest](../network-interest.md) is qualified
under the current-machine scope: its
[handoff](../handoffs/M02d-spatial-interest.md) records 460 passing CPU CTests,
protected package checks and active two-recipient privacy, byte/work and
reliable-stream delay measurements.

M02e adds integrated adversity and retained-storage observations to the existing
network contracts. `shark.core.net-adversity` (60 seconds) and
`shark.core.net-adversity-process` (360 seconds) bring the network prefix to
fourteen CTest families. Full REQ-M02 is PASS under the current-machine scope:
Windows Debug/Release/ASan pass 76 CTests each, WSL CPU client Debug/ASan+UBSan
pass 69 each and WSL Server Debug/ASan+UBSan pass 54 each, totaling 474 with no
failures or skips. The [handoff](../handoffs/M02e-integrated-qualification.md) and
[receipt](evidence/M02e-integrated-qualification.json) record 277 unchanged frozen
inputs, all six integrated scenarios and 21 children per process run, independent
receipt checks and protected package qualification. These are local network
foundation results; the full LOCAL-MMO-1 workload, durable gameplay and MMO
release qualification retain their later owners. FeatureWorld and MMOWorld
remain planned for assignments 10 and M09.

08a adds `tools.source-documents`, mapped to `shark.tools.source-documents`
(60 seconds, seed 1337) and independently authored version-0/version-1 project
and scene fixtures. Its [source contract](../source-documents.md) specifies the
strict bounded parser, in-memory legacy migration, stable references, shared
component validation, canonical extras and deterministic version-1 encoder.
Tests cover explicit expected migrated values, Unicode and world-cell endpoints,
malformed input, duplicate keys/IDs, unknown schemas, hierarchy rejection and
resource limits. The separate EngineTools/Catch2 target supports CPU authoring
without Editor, Client or a graphics device; public values expose no JSON library.

08a is PASS under the current-machine scope: 484 full-suite CPU CTests and two
additional standalone Tools tests pass, with protected builds/packages and public
API audits. The [handoff](../handoffs/08a-source-documents.md) and
[receipt](evidence/08a-source-documents.json) retain exact evidence and limits.
Full `REQ-08` subsequently passes the
[integrated persistence checkpoint](../roadmap/persistence-plan.md), 08d,
recorded below. No game-authoring command,
Lua binding or FeatureWorld case is fabricated: FeatureWorld/Persistence connects
in 10, the shared authoring services and adapters retain 09/15/16, and live
durable gameplay state retains M03.

08b adds `tools.authored-scene`, mapped to `shark.tools.authored-scene`
(60 seconds, seed 1337) and independent public API fixtures in
`tests/authored_scene_tests.cpp`. The [contract](../authored-scenes.md) covers
complete revision-consistent capture, catalog/world references, composed-transform
validation and replacement preserving unsaved state on rejection. The fixtures
cover maximum-size and sparse scenes, hierarchy bounds, source metadata lifetime,
dirty/revision/queue/visit/thread guards and old-handle invalidation.
08b is PASS under the current-machine scope: 489 full-suite CPU CTests pass
without failures or skips (Windows Debug/Release/ASan 79 each, WSL clients
72 each and WSL Servers 54 each). Independent Tools builds each pass two
additional focused CTests. The new family has 23 cases and 26,624 assertions;
291 frozen source/build/test/fixture inputs remain unchanged. Protected
packages and public API audits pass. The
[08b handoff](../handoffs/08b-scene-activation.md) and
[receipt](evidence/08b-scene-activation.json) retain exact evidence and limits.
08c safe files and conflicts and 08d integrated qualification are complete
below. General authoring commands and Lua registries remain unchanged;
FeatureWorld integration stays with 10.

08c adds `tools.project-files`, mapped to `shark.tools.project-files` and
`shark.tools.project-files-process` (90 seconds each; Catch2 seed 1337). The
[contract](../project-files.md) specifies typed I/O, exact-content revisions,
native path/ownership checks and publication with explicit durability outcomes.
Thirty cases verify revisions, Unicode, aliases, limits, faults, owner-thread and
dirty save/reload behavior, plus the different native ancestor-relocation rules.
The process fixture verifies 44 checks across exclusive ownership, five abrupt
publication phases, whole-file results and preservation of orphan stages.

08c is PASS under the current-machine scope: **499 full-suite CPU CTests**, zero
failures or skips (Windows Debug/Release/ASan 81 each, WSL clients 74 each and
WSL Servers 54 each). Standalone Tools builds add four focused passes per OS.
Windows has 1,832 file-case assertions; Linux has 1,830 because native topology
checks differ. Protected packages, public/private API audits and all seven
process receipts pass against 298 unchanged source/build/test inputs and 19
qualification helpers. The [handoff](../handoffs/08c-project-files.md) and
[receipt](evidence/08c-project-files.json) retain exact commands and evidence.
08d subsequently closes full REQ-08 below. No command, Lua or FeatureWorld
coverage is fabricated before those owners land.

08d's `shark.tools.persistence-integration` (CPU/tooling/integration,
90 seconds, seed 1337) adds three cases with 1,543 assertions spanning
legacy files, migration, live Scene edits, save/reopen, nine invalid-source
variants and explicit external-edit reconciliation. The file family now
has 31 cases; seven process receipts each pass 55 checks, including exact
reactivation and recovery after interruption of an authored save.

**08d and full REQ-08 are PASS under the current-machine scope:** all 504
full-suite CPU CTests pass without failures or skips (Windows
Debug/Release/ASan 82 each, WSL clients 75 each and WSL Servers 54 each).
Standalone Tools builds add five passes per OS. All three public C++
examples pass 27 checks; protected builds, isolated packages and API audits
pass against 299 unchanged inputs and 18 helpers. The
[handoff](../handoffs/08d-integrated-qualification.md) and
[receipt](evidence/08d-integrated-qualification.json) retain exact evidence,
including the corrected final file-case inventory. M03 separately owns live
player/economy durability; its store, character, atomic transaction and runtime
recovery, database maintenance and integrated qualification are complete;
assignment 09 is next.

M03c maps `server.durable-transactions` to `shark.storage.transactions`,
`shark.storage.transactions-process` and authenticated owner regressions in
`shark.storage.characters`. The [transaction contract](../durable-transactions.md)
defines conservation, expected economic revisions, complete-payload retries,
terminal business outcomes and a permanently advanced operation high-water mark.
The completed [M03c handoff](../handoffs/M03c-atomic-transactions.md) records
exact source/receipt counts and current-machine qualification. Economic fixtures are external CPU
fixtures; FeatureWorld/MMOWorld remain 10/M09. M03f subsequently closes full REQ-M03.

M03d maps `server.durable-recovery` to the two recovery CTest families and
earlier authenticated owner/transaction regressions. Twenty-one runtime cases
exercise bounded retained work, real session/Scene fences, timeout, exceptions,
shutdown and explicit failed/successful reopen. Sixty-nine process rows cover
every mutation commit boundary, lost acknowledgements, pruning and observed
native-VFS write/sync/full errors. The [receipt](evidence/M03d-runtime-recovery.json)
binds full local qualification; migration/restore and integrated M03 remain
separate gates. No new component, authoring command or Lua entry is introduced.

M03e maps `server.durable-maintenance` to `shark.storage.migration` and
`shark.storage.migration-process`, retaining the earlier recovery VFS regression.
Independent schema-1/2/3 SQL fixtures cover complete identity, character,
revocation and economic-history preservation. Twenty-five unit cases exercise
explicit maintenance, strict compatibility and fresh authenticated restored
restart; 66 process rows cover interrupted/failed upgrades and copies, including
actual hot-journal recovery. The [maintenance contract](../durable-maintenance.md)
keeps ordinary open nonmigrating and old restore candidates isolated. These are
CPU external fixtures; FeatureWorld/MMOWorld remain 10/M09. The [receipt](evidence/M03e-migration-restoration.json) records PASS under the
current-machine scope. M03f subsequently closes full REQ-M03 in its
[integrated receipt](evidence/M03f-integrated-qualification.json).

M03f adds `shark.storage.integration-process` to all five existing durable
feature mappings. It connects one independent legacy store to real mTLS runtime
operations, economic contention, committed lost reply, observed native database
outage, recovery and isolated restored restart. The
[qualification contract](../durable-qualification.md) fixes the connected workload
and evidence requirements. This extends executable CPU external coverage;
FeatureWorld/MMOWorld integration remains assigned to 10/M09. A passing individual
process run alone cannot close the complete cross-platform and package gate.
