---
name: shark-build-test
description: Configure, build and test Shark C++ targets, investigate CTest or sanitizer failures, and verify the runtime dependency boundary.
---

# Shark build and test

09a adds `shark.tools.authoring-commands` (60 seconds, seed 1337) to the five
existing Tools families. Its [command contract](../../../docs/authoring-commands.md)
uses AuthoredScene's single owner, required revisions, typed effects and read-only
Scene access. Preserve existing persistence cases as their ordinary edits move
through commands; private access is only for lifecycle invariant fixtures.
Run all eight authoring families in standalone CPU Tools on Windows and existing WSL, alongside
native/WSL regressions, sanitizers and protected boundaries. Compile all six
public Tools examples without private headers. 09a is PASS under the
current-machine scope; its [handoff](../../../docs/handoffs/09a-shared-commands.md)
and [receipt](../../../docs/coverage/evidence/09a-shared-commands.json) record
461 CPU CTests, 12 standalone Tools checks and protected package/API gates.
09b is also PASS under the current-machine scope. Follow its
[transaction contract](../../../docs/authoring-transactions.md),
[handoff](../../../docs/handoffs/09b-atomic-batches.md) and
[receipt](../../../docs/coverage/evidence/09b-atomic-batches.json): 605 full CPU
CTests, 14 standalone Tools checks, public examples, sanitizer objects and
protected packages pass. Full REQ-09 remains NOT_RUN.
09c is PASS under the current-machine scope. Follow the
[history contract](../../../docs/authoring-history.md),
[handoff](../../../docs/handoffs/09c-shared-history.md) and
[receipt](../../../docs/coverage/evidence/09c-shared-history.json): 610 full CPU
CTests, 16 additional Tools checks, 18 public-example compiles, actual sanitizer
objects and protected packages pass. Preserve `shark.tools.authoring-history`
(60 seconds, seed1337), all earlier families and the4096/4095 sparse workload.
Undo/redo require the next ticket and current revision; save retains history,
activation clears it, and restored incarnations receive fresh generations.
Full REQ-09 remains NOT_RUN. Next: 09d, Safe retries and lost replies.

09b adds `shark.tools.authoring-batches` (60 seconds) and `shark.core.scene-edit`
(30 seconds), both seed 1337. Preserve all earlier cases and exact inventories;
Core changes refresh both WSL Server profiles. Compile the fifth public Tools
example and audit candidate code plus fixture sanitizer objects. Windows uses
private allocation-boundary faults; Linux also sweeps ordinary/nothrow allocations.
Keep candidates and their handles private; preview never reserves a revision.

M03f connects the existing durable services in `shark.storage.integration-process`.
Follow the [qualification contract](../../../docs/durable-qualification.md):
validate one actual store lineage through migration, authenticated runtime work,
lost acknowledgement, native I/O failure, reconciliation and isolated restore.
Preserve every earlier storage/network family and the five public storage examples.
Freeze the connected fixture and supervisor along with implementation inputs;
audit every separate VFS object and require observed faults and exact state,
not only successful child exits or counts.

M03e adds `shark.storage.migration` (120 seconds, seed 1337) and
`shark.storage.migration-process` (240 seconds). Use independently authored
schema-1/2/3 SQL fixtures; preserve all earlier storage families. Validate complete
identities, records, revocations and economic outcomes across interrupted upgrade,
required pre-upgrade backup and isolated restore. Ordinary open still rejects
legacy/future versions. Never treat a stale restored high-water mark as authority
to replay an old operation. Compile the complete
[maintenance example](../../../docs/durable-maintenance.md) with public headers
only; fixture SQLite access stays test-private and packages exclude databases.

M03d adds `shark.storage.recovery` (120 seconds, seed 1337) and
`shark.storage.recovery-process` (240 seconds). Preserve the 69-row crash/VFS
matrix, actual mTLS/Scene fences, retained-work limits and explicit timeout
reconciliation. Never infer rollback from a timeout or issue a new transfer ID
to retry an uncertain effect. Compile the [recovery example](../../../docs/durable-recovery.md)
with public headers only; keep worker/SQLite ownership private to EngineStorage.

M03c adds `shark.storage.transactions` and `shark.storage.transactions-process`.
Preserve economic conservation, exact retries, terminal rejection, bounded
retention and permanently expired operation identities across restart. Compile
the [transaction example](../../../docs/durable-transactions.md) with public
headers only. Ordinary open accepts schema 3 only; M03e migration is explicit.

M03b adds durable character records and actual authenticated restart coverage in
`shark.storage.characters` and `shark.storage.characters-process`. Preserve those,
the original storage families and all session regression tests. Compile the
[public character example](../../../docs/durable-characters.md) without private
SQLite/TLS headers. Ordinary open rejects schema 1/2; use explicit M03e maintenance.

M03a adds EngineStorage only with Server. Supply `SHARK_SQLITE_ARCHIVE` for
offline Server configurations using `tools/storage_dependency.py --output`.
Preserve `shark.storage.store`, `shark.storage.process` and
`shark.tooling.storage-dependencies`, including native/WSL sanitizer builds.
SQLite must remain absent from Core/Runtime/Tools source and target closures;
compile the [public example](../../../docs/durable-store.md) without sqlite3.h.

08d integrates legacy migration, activation, edits and native saves through
`shark.tools.persistence-integration` (90 seconds, seed 1337). Preserve this family
alongside all four earlier authoring families in native and standalone Tools
builds. The process fixture now interrupts `save_scene`, then reopens the exact
expected state and recovers through another authored save. Compile all three
published source-document, authored-scene and project-file C++ examples with
public headers only; keep private-dependency and protected-package audits.

08c file-service fixtures use native filesystem operations without graphics.
Keep test roots writable under build/out in retained WSL userspaces, and run
both `shark.tools.project-files` and `shark.tools.project-files-process` alongside
the source/activation families. Process interruption is distinct from hardware
power-loss evidence; retain fault outcomes and publication/durability flags.

Read [AGENTS.md](../../../AGENTS.md), [building](../../../docs/building.md) and
[CI](../../../docs/ci.md). Run from the repository root with the native compiler
environment; Windows Ninja presets need an x64 MSVC developer shell.

Use matching configure/build/test preset names: `windows-debug` or `linux-debug`;
corresponding `-release` variants; `windows-asan` or `linux-asan-ubsan` for sanitizers.
Mac compatibility and CI are outside scope under
[ADR 0009](../../../docs/architecture/0009-windows-linux-platform-scope.md).
The later [current-machine scope](../../../docs/architecture/0010-current-machine-scope.md)
uses native Windows builds and the existing WSL2 Ubuntu CPU/headless environments.
Local checks determine roadmap progression; existing hosted CI is supplemental
and remote branch protection is unchanged. Do not require another computer,
hosted runner or native Linux display to finish an increment.

```text
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
python tools/check_format.py
python tools/validate_contract.py
```

Replace the example preset for the host. `python tools/ci.py --preset
windows-debug --jobs 2` runs the recorded CPU workflow. Inspect the first failed
step and retained evidence before changing code; do not relabel an unavailable
compiler/runner as PASS. List tests with `ctest --preset windows-debug --show-only`
and select real test names/labels. Do not run multiple builds against one build
directory concurrently.

After changes to target dependencies, configure/build `runtime-only` in the host
compiler environment; its ALL boundary target must pass. This preset disables
tests, so a native debug preset must still run CTest. Engine/Core/Client cannot depend
on EngineTools; Runtime cannot acquire authoring or test libraries transitively.
Keep exact dependency inputs in the checked-in lock; do not resolve a newer
floating version as a repair.

For CI failure propagation, use the documented
`python tools/ci.py --preset windows-debug --inject-test-failure` path and expect
a nonzero result. Restore a normal run and record both results. Do not create a
commit or stage files to test CI. This project leaves all staging/commits/pushes
to the user; maintain `.gitignore` and provide suggested commit text.

Report exact command, revision/worktree state, toolchain, platform, outcome and
unverified gates. CPU tests do not qualify GPUs, audio or packaged games. Current
bootstrap executables are not working game/editor applications.

For MMORPG work, read the [execution order](../../../docs/roadmap/mmorpg-plan.md).
Use `server-only` for the no-tests/no-download Release Server package and
`server-debug` / Linux `server-asan-ubsan` for headless CTest. `runtime-only`
does not prove this boundary. The minimal Ubuntu 24.04 command in
[CI](../../../docs/ci.md) checks configure/download and final package closures
as well as links, rejecting presentation dependencies even if client-runtime-safe.
After dependency changes run both isolated builds and the actual server lifecycle
and boundary fixtures. Keep Core/Server free of client and authoring sources.
Add actual server/network/database/load commands only when their owners land;
ordinary bootstrap CTest results are not simulation, capacity or seamlessness evidence.

M02a's private static Mbed TLS integration adds a required C compiler alongside
C++ in Core/Server builds. Prepare the exact EnTT, GLM and complete Mbed TLS
archives through `tools/core_dependencies.py`; disconnected configurations pass
`SHARK_ENTT_ARCHIVE`, `SHARK_GLM_ARCHIVE` and `SHARK_MBEDTLS_ARCHIVE` explicitly.
The headless driver selects matching pinned GCC/Clang C and C++ compilers.
Source closure validates the bundled TF-PSA/framework inputs; package checks
require every locked nested Mbed TLS notice while excluding test credentials
and shared TLS libraries. M02a's local TLS diagnostics do not qualify account
authentication, gameplay authorization, replication or production capacity.

M02b [owned sessions](../../../docs/network-sessions.md) is **PASS** under the
current-machine scope; the [handoff](../../../docs/handoffs/M02b-owned-sessions.md)
records 425 passing CTests and protected package qualification. Follow
[ADR 0012](../../../docs/architecture/0012-owned-sessions.md).
Its `shark.core.net-mtls` and `shark.core.net-session` CTests use seed 1337 and
30-second timeouts; `shark.core.net-sessions-process` has a 90-second timeout.
Use actual mutual TLS peers and controlled monotonic clocks for service tests;
retain separate-process evidence for enrollment, ownership, revocation, expiry,
replay, reconnect and rate-limit behavior. Include public certificate fixtures and
their generator in source-input freeze hashes. Keep test keys and SessionProbe
out of installed packages. Preserve the existing Windows/WSL CPU, sanitizer and
protected package gates when extending these services. M02c replication is PASS
under current-machine scope: see the [replication contract](../../../docs/network-replication.md)
and [handoff](../../../docs/handoffs/M02c-authoritative-replication.md). Its three
additional CTest families use 20/30/90-second timeouts for wire, service and real
process cases; preserve these earlier network families and freeze source/fixture
hashes for full qualification. Session readiness alone does not qualify gameplay or capacity.

M02d [spatial interest](../../../docs/network-interest.md) is locally qualified.
Preserve the complete trusted catalog, authenticated character observer binding,
unthrottled privacy maintenance and bounded managed admission. Its two new CTest
families are `shark.core.net-interest` (30 seconds, seed 1337) and
`shark.core.net-interest-process` (120 seconds); retain these families alongside
the earlier network tests. Two active recipients and controlled reliable-stream
delay do not establish the full LOCAL-MMO-1 workload or WAN behavior. The
[M02d handoff](../../../docs/handoffs/M02d-spatial-interest.md) retains that gate's
historical evidence; M02e subsequently closes integrated qualification.

M02e adds `shark.core.net-adversity` (60 seconds, seed 1337) and
`shark.core.net-adversity-process` (360 seconds), bringing the network prefix to
fourteen CTest families. M02e and full REQ-M02 are PASS under the current-machine
scope in the [handoff](../../../docs/handoffs/M02e-integrated-qualification.md);
assignment 08 follows the [persistence checkpoints](../../../docs/roadmap/persistence-plan.md). Follow the
[qualification contract](../../../docs/network-qualification.md): semantic faults
operate on decoded authenticated state, while the bounded relay preserves TLS
byte order. Record actual injected events, convergence, cleanup, retained vector
capacity and separate process RSS/handle samples. Refresh all earlier network
families and protected packages against the same frozen source inputs.

08a adds CPU EngineTools source-document tests and a private JSON dependency.
The `tools-debug` preset must configure/build with Client/Editor OFF; inspect its
inventory before selecting `shark.tools.source-documents` (60 seconds, seed 1337).
`shark.tooling.source-document-dependencies` has a 240-second timeout. Offline
Tools builds additionally supply the exact `SHARK_JSON_ARCHIVE`; protected
Runtime/Server configurations keep Tools OFF and must not populate JSON. Compile
the documented public example without JSON include paths. See the
[source contract](../../../docs/source-documents.md) and
[08a handoff](../../../docs/handoffs/08a-source-documents.md).

08b adds `shark.tools.authored-scene` (60 seconds, seed 1337) and the separate
`AuthoredSceneTests` target. Run both Tools families in the independent CPU-only
configuration, alongside native/WSL regressions and protected boundaries.
Compile the complete [capture/activation example](../../../docs/authored-scenes.md)
without private dependency headers. Its [handoff](../../../docs/handoffs/08b-scene-activation.md)
records its historical qualification status; the completed 08c file services
participate in the integrated 08d gate.
