# CPU and headless Server CI

Under [ADR 0010](architecture/0010-current-machine-scope.md), roadmap acceptance
uses this PC's native Windows and existing WSL Ubuntu checks. The nine-job hosted
workflow is retained as supplemental regression coverage; hosted availability is
not a prerequisite for local assignment progression. Existing remote branch
protection still governs GitHub merges and is unchanged. Do not label an absent
or failing hosted run PASS, and investigate real failures affecting the local
targets. No hosted run is required for this planning-only scope amendment.

Assignment 01's hosted gate is **PASS** on
`3620fab38a999a6dd6b6fdbb8c8a25b9a680b6de`. After repairing the macOS ARM Xcode
selection, the ordinary push passed all fourteen jobs. The subsequent injected
run failed only the sentinel in each of twelve CPU jobs; the restored run passed
all fourteen jobs and all 120 CPU tests on the same revision. See the
[CI verification handoff](handoffs/01-ci-verification.md) and
[durable gate evidence](coverage/evidence/01-ci-gate.json). The user reserves Git
staging, committing and pushing.

**M01's hosted gate is PASS** at
`d658d69c21c492c844f5678f931b1ddf955d25cd`. The ordinary push and restored manual
run pass all fifteen jobs and all 206 CTests. The intervening injected run reaches
193 tests, failing only the thirteen intended sentinels while the other 180 pass;
headless sanitizer and clean-runtime stages stop after its Debug failure.
The [M01 hosted follow-up](handoffs/M01-ci-verification.md) and
[durable gate evidence](coverage/evidence/M01-ci-gate.json) record actual runs,
Docker/package qualification and the fifteen checks required at that revision. The
[MMORPG execution plan](roadmap/mmorpg-plan.md) records the completed 04c and full
assignment-04 local gate, following local 03/04a/04b qualification.
05a device ownership is complete locally. **05b presentation lifecycle is
complete under the current-machine scope.** The
[05b handoff](handoffs/05b-presentation-lifecycle.md) records 236 passing CTests,
2,304 validated presentations across both local GPUs, application and protected
package checks. Deferred qualification work is tracked in the
[nonblocking backlog](backlog.md); the user's decision authorizes continued
development under each increment's own acceptance criteria. The
[06 checkpoint plan](roadmap/shader-resource-plan.md) and
[06a handoff](handoffs/06a-offline-shaders.md) record the completed offline checkpoint.
The [05a handoff](handoffs/05a-graphics-device.md) records 211 CTests and both-GPU
device qualification. The [opt-in probe](graphics-probe.md) adds CPU checks to
ordinary CI while graphics builds/device runs use explicit graphics presets.
The historical job/test counts above remain tied to their recorded revisions.
[ADR 0009](architecture/0009-windows-linux-platform-scope.md) removes Mac from
the active product and CI scope, retiring 02c and removing the six Mac CPU jobs.
The current workflow has nine jobs, described below. At `39110d4`,
[run 39092472102](https://github.com/markdawes08/shark-engine/actions/runs/39092472102)
passed all nine jobs and 156/156 CTests; the
[02d audit](coverage/evidence/02d-hosted-ci.json) preserves artifact digests,
toolchains, boundary results and protection readback. This verifies the platform
scope amendment, not the later Linux package and link changes committed at
`2ef281b`. No new hosted execution of that revision is claimed here. Windows
relocation and SDK-free WSL package checks meet the amended local acceptance
scope; they remain distinct from independent-machine or native Linux GPU evidence.
The CPU package rejection suite
is registered as `shark.tooling.graphics-package`, with the Linux package and
process-cleanup suite in `shark.tooling.graphics-package-linux`; Windows launcher integration and
GPU soak remain opt-in. M02a's [local TLS/wire foundation](handoffs/M02a-transport-wire.md)
passes current-machine CPU/sanitizer and protected package qualification.
M02b [owned sessions](handoffs/M02b-owned-sessions.md) is **PASS** under the
current-machine scope. M02c [replication](handoffs/M02c-authoritative-replication.md)
is also PASS locally: 446 CPU CTests and protected package checks. M02d
interest/budgets and M02e integrated qualification are complete. Full REQ-M02
is PASS under the current-machine scope in the
[M02e handoff](handoffs/M02e-integrated-qualification.md); database, world-handoff
and active-load tiers retain their later gates.

## Jobs and required checks

[The workflow](../.github/workflows/cpu.yml) runs on pushes, pull requests and
manual dispatch. It requests read-only repository permission, checks out without
persisting credentials, and uploads logs on failure as well as success.

| Job names | Runner / architecture | Selected compiler | Configuration |
| --- | --- | --- | --- |
| `quality` | `ubuntu-24.04` x64 | None | Contract/negative checks, workflow syntax and C++ formatting |
| `cpu-windows-debug`, `cpu-windows-release`, `cpu-windows-asan` | `windows-2025-vs2026` x64 | VS 2026, MSVC toolset 14.51, Windows SDK 10.0.26100.0 | Debug, Release, ASan |
| `cpu-linux-debug`, `cpu-linux-release` | `ubuntu-24.04` x64 | GCC 13 (`g++-13`) | Debug, Release |
| `cpu-linux-asan-ubsan` | `ubuntu-24.04` x64 | Clang 18 (`clang++-18`) | ASan + UBSan |
| `runtime-only` | `ubuntu-24.04` x64 | GCC 13 | Client Release, no Server/Editor/EngineTools/Tests |
| `headless-server` | `ubuntu-24.04` x64 host; locked Ubuntu 24.04 amd64 image | Snapshot GCC 13 and Clang 18 | Server-only Release package, Core/Server Debug tests, ASan + UBSan tests and clean-runtime smoke |

All nine jobs, including `headless-server`, are configured as required checks on
`main`, bound to GitHub Actions app 15368, with up-to-date branches required.
The [scope amendment handoff](handoffs/windows-linux-scope-amendment.md) and
[protection readback](coverage/evidence/windows-linux-ci-protection.json) record
removal of only the six Mac check names. Administrator bypass and all other
protection settings are unchanged. No approving-review or push-actor restriction
was added. The revised workflow passed at `39110d4`; a later revision needs its
own actual run before a hosted PASS can be claimed. This is separate from local
roadmap completion under ADR 0010.
The historical [M01 gate evidence](coverage/evidence/M01-ci-gate.json) and
[01 protection report](coverage/evidence/01-ci-protection.json) retain their
original fifteen- and fourteen-check policies. Missing, skipped or cancelled required jobs
do not establish a hosted PASS or satisfy those remote required checks. The Windows Server
runner is build evidence; it does not qualify Windows 11 graphics or deployment.
The matrix checks Debug and Release independently and runs sanitizers on both
OS/CPU rows. MSVC supplies ASan only; UBSan is exercised by Linux Clang.
Runtime-only has no CTest preset because Tests are disabled. That job explicitly
builds the client boundary target and runs Runtime's real `--version` behavior.
Ordinary CPU presets now also build Server and run its unit/lifecycle checks.
04b adds the [window contract](window-lifecycle.md) and display-free window/CLI
fixtures. Windows client builds compile the pinned GLFW backend but ordinary
CTest never opens a window; explicit native smoke runs qualify the current PC.
Linux CPU and isolated Server builds leave `SHARK_NATIVE_WINDOW=OFF`, preserving
their device-free dependency set. 04c adds `shark.client.frame` with seed 1337,
labels `cpu;unit;client` and a 20-second timeout. Its deterministic input/time
fixtures and the expanded `shark.core.timing` cover pause/step, first-tick input,
failure/cancellation and server independence. Native window smoke separately
exercises the shared launch helper and reserved F6/F7 sample controls. Final local
qualification is PASS in the [04c handoff](handoffs/04c-frame-loop.md).
05a adds production device ownership and `shark.client.graphics` CPU fixtures,
`shark.client.graphics-cli` device-free command checks in native builds, and
`shark.tooling.graphics-device-evidence` parser tests. Native Windows clients
compile Vulkan/NVRHI by default; WSL CPU and Server builds keep
`SHARK_NATIVE_GRAPHICS=OFF`. Actual device qualification runs the explicit
[graphics device fixture](graphics-device.md) on both current-PC GPUs with core,
synchronization and NVRHI validation. It is separate from ordinary CTest and
does not establish presentation or GPU submission. No new hosted result is
implied by local work.
05b adds `shark.client.presentation` with seed 1337, labels `cpu;unit;client` and
a 20-second timeout for injected lifecycle, launch and owned queue-waiter cases.
Device-free CLI checks cover `PresentationSmoke` and Editor/Runtime `--graphics`;
`shark.tooling.presentation-evidence` checks the strict native-evidence parser.
Actual presentation qualification separately runs the
[presentation lifecycle fixture](presentation-lifecycle.md) on both local GPUs
in Windows Debug, Release and ASan with required validation. Ordinary CTest does
not open a window or replace those hardware runs. Editor/Runtime's clear sample
uses the shared offline frame/input pump; its diagnostic completion is NOT_RUN
for qualification.
05c's [qualification guide](presentation-qualification.md) declares the fixed
native workloads, resource limits and commands. `shark.client.presentation-faults`
executes three isolated CPU fatal-destructor paths with a 45-second CTest limit;
`shark.client.presentation-stress-cli` checks device-free discovery and invalid
arguments with a 30-second limit. The `shark.tooling.presentation-faults`,
`shark.tooling.presentation-stress` and `shark.tooling.runtime-package` tests reject
malformed or false evidence and invalid package inputs. Native recording/surface/
device fault injections, sustained measured presentation and relocated Runtime
execution are separate opt-in runs. Their wrappers bind the executable, source,
lock, tools, validation payload and selected UUID to retained raw records; a test
child's expected fatal exit is accepted only with its matching cleanup evidence.
Windows tests cover finite-duration shutdown; POSIX SIGINT/SIGTERM and readiness
I/O failure checks execute on Linux, not through Windows `terminate()`.

06a registers `shark.tooling.shader-pipeline` with `cpu;tooling` labels and a
30-second timeout. Controlled tool fixtures cover the offline recipe/dependency/
artifact contract without preparing DXC or a GPU. The explicit real-tool command
in the [shader guide](shader-pipeline.md) is separate evidence. Full assignment
06 does not pass from this CPU suite alone.

06b registers CPU resource/owner tests (20-second limits), offscreen oracle/fault
protocol tests (30 seconds) and two device-free native fixture CLI checks
(30 seconds). Explicit native commands qualify both local GPUs separately, using
fixed color/depth tolerances and retained failure evidence; see the
[resource contract](gpu-resources.md) and [06b handoff](handoffs/06b-gpu-resources.md).
Hosted CI remains supplemental to these local gates.

06c adds `shark.client.shader-bundle` for device-free distribution framing,
SHA-256, Unicode paths and native shader-interface checks. Runtime/Editor CLI
tests reject malformed cube options and missing bundles before device creation.
The shader tooling suite covers explicit `prepare`/`export`, dependency rebuilds,
atomic bundle publication and source/tool-free export. Existing package tests
cover the additional precompiled closure and completion identity. None of these
CPU cases opens a graphics device or claims hardware qualification.

Actual 06c acceptance separately presents and captures the same textured cube on
both GPUs, checks resize/minimize/restore, resource lifetime and failure behavior,
and runs a relocated Release Runtime payload with the bundle and no source shader
compiler. The [build workflow](building.md#cube-sample-and-runtime-package) and
[resource contract](gpu-resources.md#window-cube-and-capture) define those paths.
Windows Debug/Release/ASan, WSL CPU/sanitizer and protected target boundaries remain
required regression. All passed for 06c: Windows 55 tests in each configuration,
WSL 48 tests in each configuration, six window/offscreen GPU suites, native failure
children and isolated Runtime launches. See the [06c handoff](handoffs/06c-window-distribution.md)
and [receipt](coverage/evidence/06c-window-distribution.json).
`shark.tooling.cube-smoke` and `shark.tooling.cube-faults` validate strict evidence
and fixed references without graphics; `shark.client.cube-cli` and
`shark.client.cube-faults-cli` validate native fixture arguments without a device.

07a adds `shark.core.scene` and `shark.core.world-position` to both ordinary and
headless test builds (`cpu;unit;server`, seed 1337, 20-second timeouts). They cover
scene ownership, checked identity and lifetime, bounded deferred mutation and
canonical global coordinates. 07b adds `shark.core.transform` (20-second timeout)
and `shark.core.hierarchy` (30-second timeout), with the same labels and seed.
They exercise numerical conventions, validated Name/Transform values, hierarchy
lifetime, atomic rejection and immediate/deferred reparenting.
07c adds `shark.core.schema` and `shark.core.scene-query`, each with a 20-second
timeout, seed 1337 and `cpu;unit;server` labels. They cover fixed metadata and
shared validation, bounded field-selected query pages, revision conflicts and
owned snapshot lifetime under the [inspection contract](scene-inspection.md).
Both are CPU C++ fixtures; they do not invoke terminal/editor adapters or qualify
a source or gameplay-wire format.

`shark.tooling.scene-dependencies` (`cpu;tooling;boundary;server`, 240-second
timeout) accepts only the locked EnTT, GLM and Mbed TLS archives, proves disconnected local
extraction, rejects source overrides and checks that Core configures without
client dependencies. M02a adds a C compiler for the static TLS graph. EnTT/TLS includes stay private; the approved
public `SharkMath` interface propagates GLM includes and its five central compile
definitions. Real C++ fixture compilations accept the wrapper with central
definitions and defaults, reject a prior GLM include, and reject each of the 20
incompatible configuration definitions. Existing graphics-dependency fixtures
receive the three verified Core archives, plus SQLite when Server is enabled,
and continue to reject all client source overrides before population. Both
dependency tooling families have 240-second suite
timeouts and 120-second configure deadlines, accounting for fresh exact TLS
source population; these are tooling limits, not transport budgets.
No fixture downloads dependencies during CTest.

The headless closure checks compare the three Core dependency source inventories
and the Server-only SQLite inventory with their pinned archives and verify their
installed notices. Runtime package
tests also require EnTT, GLM and all eight TLS notices. Windows CPU/sanitizer, existing WSL
CPU/headless and protected target builds qualify these checkpoints; the CPU scene
layer does not require repeating 06c's GPU acceptance.
07c and full REQ-07 are PASS under the current-machine scope. The
[07c receipt](coverage/evidence/07c-schema-inspection.json) records 376 passing
CTests with no failures/skips: Windows Debug/Release/ASan 62 each, WSL client
Debug/ASan+UBSan 55 each, and WSL Server Debug/ASan+UBSan 40 each. Protected
Runtime/Server builds, source/link/license closure and fresh isolated package
checks pass with all 221 qualification source inputs unchanged.
See the [07 plan](roadmap/scene-plan.md),
[07a handoff](handoffs/07a-entity-ownership.md),
[07b handoff](handoffs/07b-transform-hierarchy.md) and
[07c handoff](handoffs/07c-schema-inspection.md). M02 follows its
[five checkpoints](roadmap/network-plan.md);
hosted CI stays supplemental under the unchanged current-machine scope.

M02a adds `shark.core.net-wire`, `shark.core.net-fault`, `shark.core.net-transport`
and `shark.core.net-process`. The first three use seed 1337 and bounded CPU/loopback
fixtures; the process suite drives separate server/client processes and independent
TLS peers, with JSON evidence and process-tree watchdogs. They cover diagnostic
wire negotiation, TLS trust, bounded queues/deadlines and deterministic virtual-time
fault modeling, not player sessions or entity replication. See the
[network contract](network-transport.md).

M02b adds `shark.core.net-mtls` and `shark.core.net-session`
(`cpu;unit;server`, seed 1337, 30-second timeouts), plus
`shark.core.net-sessions-process` (`cpu;integration;server`, 90-second timeout).
The unit fixtures use actual mutual TLS peers and controlled monotonic session
time. The process suite drives separate authenticated server/client processes
with bounded watchdogs and JSON evidence. These exercise certificate rejection,
enrollment and ownership, revocation, malformed/replayed requests, expiry, rate
bounds and reconnect fencing. Session readiness is the only mutable admitted
state; movement, combat and entity replication remain outside this checkpoint.

Qualification is **PASS**: native Windows Debug/Release/ASan pass 69 CTests each,
existing WSL CPU client Debug/ASan+UBSan pass 62 each and Server Debug/ASan+UBSan
pass 47 each, totaling **425 with zero failures or skips**. Protected builds,
lifecycle checks, source/link/license closure and isolated package audits pass.
The fresh Runtime package passes relocation and ten missing-notice rejection cases;
the SDK-free WSL audit verifies the 11-file Server package and unchanged 92-package
runtime inventory. Compile-input and sanitizer audits pass, including all 116
private TLS C units. Public certificate fixtures and their generator
belong to the source-input receipt; test credentials and SessionProbe must not
enter installed packages. [ADR 0012](architecture/0012-owned-sessions.md) and the
[session contract](network-sessions.md) define the acceptance boundary. The
[M02b handoff](handoffs/M02b-owned-sessions.md) records the observed results.
M02c replication is PASS under the current-machine scope: Windows Debug/Release/ASan
pass 72 CTests each, WSL CPU client Debug/ASan+UBSan pass 65 each and WSL
Server Debug/ASan+UBSan pass 50 each, totaling 446 with no failures or skips.
The [M02c handoff](handoffs/M02c-authoritative-replication.md) records protected
build, package and real-process convergence checks. M02d spatial interest, privacy
and connection budgets is complete: its [handoff](handoffs/M02d-spatial-interest.md)
records 460 passing CPU CTests and protected package checks.

M02e and full REQ-M02 are PASS under the current-machine scope. Windows
Debug/Release/ASan pass 76 CTests each, WSL CPU client Debug/ASan+UBSan pass
69 each and WSL Server Debug/ASan+UBSan pass 54 each: 474 total without failures
or skips. All fourteen network families, protected builds and isolated package
checks pass against 277 unchanged frozen inputs. The new
`shark.core.net-adversity` and `shark.core.net-adversity-process` families have
60-second and 360-second timeouts respectively. The
[handoff](handoffs/M02e-integrated-qualification.md) and
[receipt](coverage/evidence/M02e-integrated-qualification.json) retain exact
commands, resource observations and acceptance limits. Assignment 08 follows the
[persistence checkpoints](roadmap/persistence-plan.md); hosted CI stays supplemental.

08a adds `shark.tools.source-documents` (CPU/unit/tooling, seed 1337, 60 seconds)
and `shark.tooling.source-document-dependencies` (CPU/tooling/boundary, 240 seconds)
when Tools is enabled. The first checks strict project/scene codecs, migration,
resource bounds and shared component validation against independent fixtures.
The second verifies the exact private JSON source and rejects graph/include
leaks into protected targets. `tools-debug` additionally demonstrates CPU authoring
without Editor or Client. See the [08a handoff](handoffs/08a-source-documents.md)
for final current-machine evidence: 484 full-suite CPU CTests pass (Windows
Debug/Release/ASan 78 each, WSL clients 71 each, WSL Servers 54 each), plus one
focused Tools-only CTest on each OS. Protected builds/packages and public API
audits pass. This 08a evidence remains separate from 08b qualification below.

08b adds `shark.tools.authored-scene` (CPU/unit/tooling/integration, seed 1337,
60 seconds) when Tools is enabled. It exercises complete capture, immutable
source metadata, cross-document references, composed-transform rejection,
revision/dirty/queue/visit/thread guards and replacement lifetime through the
public EngineTools owner. The [08b handoff](handoffs/08b-scene-activation.md)
and [receipt](coverage/evidence/08b-scene-activation.json) record PASS under
the current-machine scope: 489 full-suite CPU CTests, without failures or
skips (Windows Debug/Release/ASan 79 each, WSL clients 72 each, WSL Servers
54 each). Each independent Tools build passes two additional focused CTests.
The new family passes 23 cases and 26,624 assertions; all 291 frozen inputs
are unchanged. Protected package checks and public API audits pass. 08c safe
files and conflicts and 08d integrated qualification are complete below.

08c registers `shark.tools.project-files` (CPU/unit/tooling/integration, seed 1337)
and `shark.tools.project-files-process` (CPU/tooling/integration), each with a
90-second timeout. Their fixtures exercise native file revisions, path restrictions,
faults, dirty-safe save/reload, cross-process ownership and five deterministic
abrupt-exit stages. Each process run emits `tests/project-files-process.json` in
the build tree with complete-file hashes, unpublished-stage retention and bounded
child outcomes. See the [file contract](project-files.md) and
[08c handoff](handoffs/08c-project-files.md) for exact scope and qualification.

08c's [receipt](coverage/evidence/08c-project-files.json) records PASS: all 499
full-suite CPU CTests and eight additional standalone Tools tests pass without
failures or skips. Windows profiles register/run 81 tests each, WSL clients 74
each and WSL Servers 54 each. Protected packages and compiler/API audits pass
with all 298 source/build/test inputs and 19 helper hashes unchanged. This
historical gate is followed by the full 08d qualification below.

08d registers `shark.tools.persistence-integration` (CPU/tooling/integration,
seed 1337, 90 seconds). Its three cases pass 1,543 assertions; the retained
file family contains 31 cases, and seven process receipts each pass 55
checks with complete authored-save reactivation and recovery. **08d and
full REQ-08 are PASS under the current-machine scope:** all 504 CPU CTests
pass without failures/skips (Windows Debug/Release/ASan 82 each, WSL
clients 75 each and WSL Servers 54 each), with five additional standalone
Tools passes per OS. All three public examples pass 27 checks. Protected
Runtime/Server builds, isolated packages and dependency/API audits pass
against 299 unchanged source/build/test inputs and 18 helpers. The
[handoff](handoffs/08d-integrated-qualification.md) and
[receipt](coverage/evidence/08d-integrated-qualification.json) record exact
commands, the corrected file-case inventory and verification limits. M03
separately owns live durability; M03 and all six checkpoints are complete; assignment 09 is next.

## Locked tools and hosted-image limits

[requirements-dev.txt](../requirements-dev.txt) locks these distributions with
SHA-256 hashes for Windows x64 and Linux x64:
CMake 3.31.10, Ninja 1.13.2, clang-format 20.1.8 and PyYAML 6.0.3. Python is exactly
3.13.5 for the retained desktop CPU workflow. The separately locked minimal
headless image uses Ubuntu snapshot Python 3.12.3, as described below; this does
not relax the existing CPU helper's pin. Install only wheels with the documented hash-checking command in
[building.md](building.md). The Ninja wheel's executable identifies its vendor
build as `1.13.2.git.kitware.jobserver-pipe-1`; the wheel version remains 1.13.2.
The exact executable banner and hash are retained in evidence. PyYAML is only a
development workflow validator and never enters Engine or Runtime.

The action revisions are immutable commit pins, verified against their upstream
release tags on 2026-10-17:

- `actions/checkout`: `08c6903cd8c0fde910a37f88322edcfb5dd907a8`
  ([v5.0.0](https://github.com/actions/checkout/releases/tag/v5.0.0)).
- `actions/setup-python`: `e797f83bcb11b83ae66e0230d6156d7c80228e7c`
  ([v6.0.0](https://github.com/actions/setup-python/releases/tag/v6.0.0)).
- `actions/upload-artifact`: `ea165f8d65b6e75b540449e92b4886f43607fa02`
  ([v4.6.2](https://github.com/actions/upload-artifact/releases/tag/v4.6.2)).

Hosted runner OS labels select a platform, not an immutable VM snapshot. GitHub
updates these images regularly. The workflow explicitly selects an installed
MSVC toolset family/SDK and GCC/Clang executable family; it fails when a requested
toolchain is absent instead of silently choosing another Visual Studio major
version. Full compiler patch/build versions, compiler binary
hashes, SDK selection and `ImageOS`/`ImageVersion` are recorded after configure.
Upstream inventory documents the selected labels and installed tools; actual
results and recorded images are linked in the handoff.
[GitHub runner image policy and inventory](https://github.com/actions/runner-images)

Qualification amendment to ADR 0002: exact project tools and source dependencies
are immutable inputs; hosted CI compiler families are explicitly selected
and their exact observed binaries recorded per run. This is a controlled rolling
CI configuration, not a claim that a hosted VM can be recreated from its label.
Pinning the action or Python version does not freeze the OS, standard library,
OS SDK, compiler patch or Python distributor rebuild. Assignment 35 must archive
the actual toolchain/environment and build inputs needed for release reproduction,
or use an immutable controlled runner image. A missing toolchain or changed compiler
requires a reviewed configuration/evidence update; do not mask that failure.

## M03 durable-store and character checks

M03f's `shark.storage.integration-process` connects the existing services on one
real store lineage. Follow the [qualification contract](durable-qualification.md).
The fresh source freeze includes the connected probe/supervisor and independent
SQL inputs. Keep all prior families, five public storage examples, standalone
Tools checks and protected packages. Actual sanitizer dependency audits include
the integration probe's separate instance of the test-only VFS source.

M03b extends the same gate with `shark.storage.characters` and
`shark.storage.characters-process`, documented in [building](building.md#server-only-durable-storage).
Retain all session, storage and authoring regression families. Actual mutually
authenticated local clients exercise the same durable character coordinator,
then a fresh server process reopens the store without reseeding ownership.
Fixture-only ID discovery is local test metadata, not a product roster protocol.
The public character example compiles using only public Core/Storage headers.
M03b is PASS under the current-machine scope: its
[handoff](handoffs/M03b-owned-characters.md) and
[receipt](coverage/evidence/M03b-owned-characters.json) record 539 full CPU CTests,
ten additional standalone Tools checks and protected package/API audits against
318 unchanged source inputs and 15 final helpers. That checkpoint did not close full REQ-M03;
M03c atomic transactions are PASS under the same scope: its
[handoff](handoffs/M03c-atomic-transactions.md) and
[receipt](coverage/evidence/M03c-atomic-transactions.json) record 553 full CPU
CTests (89 native each, 82 WSL clients each, 61 WSL Servers each), ten additional
standalone Tools checks and protected package/API gates. M03d is also PASS:
its [handoff](handoffs/M03d-runtime-recovery.md) records 567 full CPU CTests
and 10 standalone Tools checks with protected package/API qualification.
M03e database migration/restore is PASS: its
[handoff](handoffs/M03e-migration-restoration.md) records 581 CPU CTests,
10 standalone Tools checks and protected package/API gates.
M03f and full REQ-M03 are PASS: the [handoff](handoffs/M03f-integrated-qualification.md)
records 588 CPU CTests, 10 standalone Tools checks and protected gates.
Assignment 09 is next.

M03a adds the three storage families documented in [building](building.md#server-only-durable-storage).
The Server build has a separate `storage` dependency domain; Core, Runtime and
Tools reject storage targets and headers. `tools/storage_dependency.py` prepares
and verifies the exact SQLite ZIP, four upstream files and preserved public-domain
notice. It is additional to the three unchanged Core archives. Dependency fixtures
reject missing/changed archives, sources, notices and unsupported overrides.

The process fixture exercises a real database across restart, concurrent ownership,
abrupt release, three initial-commit interruption phases and supported-version
hot-journal rollback. Fixed seeds, pipe barriers, deadlines and JSON outcomes
keep this noninteractive. The character, economy, runtime recovery and maintenance
families add their separate gates under the
[M03 checkpoint plan](roadmap/durable-state-plan.md). M03f closes full integrated
acceptance under the [qualification contract](durable-qualification.md).
Sanitizers must instrument both the storage C++ implementation and sqlite3.c.
The Server package gains only `licenses/SQLite/LICENSE.txt`; SQLite shells, shared
libraries, databases, journals and test probes are excluded. Runtime packages
retain their existing dependency closure.

## Minimal Ubuntu headless gate

[inputs.json](../tools/ci/headless/inputs.json) pins the Ubuntu 24.04 amd64 base
image to `sha256:a2d43bffc46670d7108bb3c0468c67c5557c4c92e83e1e34cbff1322ad1b94dc`,
the APT snapshot to `20261017T000000Z`, and the bootstrap CA package by SHA-256.
The [Dockerfile](../tools/ci/headless/Dockerfile) and
[setup script](../tools/ci/headless/setup.sh) install GCC 13, Clang 18 and its
sanitizer runtime, Python/venv, Git and binutils from signed snapshot metadata
with recommendations disabled. The complete installed package/version/architecture
inventory must exactly match [packages.tsv](../tools/ci/headless/packages.tsv).
Presentation, source-import and offline shader packages are rejected separately.
The locked wheel tools are installed with hash checking. Python 3.12.3 is a
specific exception for this isolated image, whose Ubuntu package revisions are
recorded; ordinary CPU jobs continue to require Python 3.13.5.

On a native Linux x86-64 host with Docker and GNU `timeout` available, run the
same entry point used by CI, choosing a fresh evidence directory for each run:

```sh
bash tools/ci/headless/run.sh out/ci/headless-local
```

The [wrapper](../tools/ci/headless/run.sh) builds its image from a temporary
context containing only the image recipe and locked tooling inputs. It binds
the repository read-only at `/source`, including its existing Git metadata, and
writes builds inside the disposable container and evidence into the selected
host directory. It performs no staging, commits or source copies. Docker image
setup changes the disposable environment, not host APT configuration. Do not
run `setup.sh` against a working host installation. No Podman or alternate-host
qualification is implied by this Linux command.

When Docker is unavailable, the checked-in Linux/WSL fallback uses the same
hash-verified base userspace and snapshot setup inside private chroots:

```sh
python tools/ci/headless/fetch_inputs.py --output .tools/headless
sudo bash tools/ci/headless/chroot.sh /var/tmp/shark-headless-local .tools/headless out/ci/headless-chroot-local
```

Choose an unused `/var/tmp/shark-headless-*` parent and empty evidence directory.
The [fetcher](../tools/ci/headless/fetch_inputs.py) verifies the manifest, image
configuration, base layer and CA package against the input lock. The
[chroot wrapper](../tools/ci/headless/chroot.sh) requires Linux privileges for a
private mount namespace, mounts source read-only and installs packages only
inside the disposable build root. It keeps a second pristine base root for the
installed-Server smoke, with a separate network namespace. It retains both
private root filesystems for inspection rather than deleting them automatically.
This records the shared host kernel explicitly; it does not claim an independent
VM or a successful Docker run. Unlike the Docker smoke, this fallback does not
claim a read-only runtime root.

[headless_ci.py](../tools/headless_ci.py) validates the OS/architecture, exact
installed package inventory, image provenance and Python/tool pins. It rejects
mapped GPU/audio devices and display sockets and clears display/audio-session
environment variables. Before configuring, it explicitly prepares the exact EnTT,
GLM and Mbed TLS archives under the writable build root and prepares SQLite with
`storage_dependency.py`. It passes `SHARK_ENTT_ARCHIVE`, `SHARK_GLM_ARCHIVE`,
`SHARK_MBEDTLS_ARCHIVE` and `SHARK_SQLITE_ARCHIVE` to each preset. This uses
[the shared locked archive helper](../tools/core_dependencies.py),
not an installed-package or source-tree fallback. It then runs these stages in
fresh build directories:

1. `server-only`: GCC Release with Client, Editor and Tests disabled, dependency
   network fetching disabled (verified local EnTT/GLM/TLS/SQLite extraction only), explicit
   Server/Core boundary check, component install including all dependency notices,
   source/build/package closure inspection and a finite installed-Server run.
2. `server-debug`: GCC Debug with the Core/Server Catch2 and process tests;
   CTest requires a nonempty selection and writes JUnit.
3. `server-asan-ubsan`: Clang 18 with ASan/UBSan and leak detection, running the
   same headless tests with failure halting enabled.

The wrapper then starts the installed Release Server in the unmodified pinned
base image with networking disabled and a read-only root filesystem. This last
smoke test uses no build tools or Python in the runtime environment. Headless
qualification requires that smoke result as well as the build/test summary;
unsetting display variables on a desktop runner alone is insufficient.

Closure checks inspect CMake's generated graph and file-api data, the compiler's
actual source/header dependencies and linker inputs, installed package contents,
and ELF direct/transitive library dependencies. The only permitted dependency
source trees in `server-only` are the exactly pinned EnTT, GLM, Mbed TLS and SQLite populations;
every regular file is compared with its verified archive, with only the 147 locked,
unused upstream symlinks omitted for portable extraction. The TLS archive supplies
3,927 verified regular files. EnTT/GLM are header-only;
the private TLS graph has three static libraries and seven reviewed object targets.
No shared TLS library enters the ELF closure. The installed package contains the
executable, `licenses/EnTT/LICENSE`, `licenses/GLM/copying.txt` and eight exact notices
under `licenses/MbedTLS/`, plus the preserved SQLite notice under
`licenses/SQLite/LICENSE.txt` (12 files total).
Negative fixtures exercise changed/missing locked inputs, extra dependency trees
and presentation/authoring leaks. Catch2 may be fetched for test-enabled
configurations; it must not enter `server-only` or the installed package. These
package and closure checks qualify an idle process lifecycle and dependency boundary, not world
simulation, networking, persistence, player capacity or release deployment.

The selected output contains `summary.json`, toolchain reports, phase logs,
JUnit, installed package inventory, input/package provenance, license records,
the Server package and closure report. Docker adds its image inspection; the
chroot path records its shared host kernel and pristine runtime package inventory.
`clean-runtime.json`
records the separate base-image smoke. CI uploads it as
`headless-server-<run_attempt>` even on failure, with a 30-day retention period.
Preserve required acceptance findings in the M01 handoff before artifacts expire.
Each phase has a deadline and a nonzero result stops dependent work. An absent
test or clean-runtime result is unrun, never a passing gate.

## Shared local and CI command

After installing the locked tools and entering the appropriate compiler environment:

```text
python tools/ci.py --preset windows-debug
python tools/ci.py --preset windows-release
python tools/ci.py --preset windows-asan
python tools/ci.py --preset linux-debug
python tools/ci.py --preset linux-release
python tools/ci.py --preset linux-asan-ubsan
```

Run only the presets for the current OS. Windows requires the native x64 MSVC
developer environment; a plain shell with MSYS CMake earlier on PATH is not a
valid substitute. The helper verifies the Python and tool pins before configuring.
Explicit `CC` and `CXX` environment values override the preset's portable C and
C++ compiler names. Select matching compiler families/versions for the TLS build;
Linux sanitizer presets use `clang` with `clang++`, and the retained headless
driver explicitly selects both versioned compilers. The M02a handoff records the
separately checked two-line `CC` forwarding amendment to the general CPU helper.

The helper configures with `SHARK_INJECT_TEST_FAILURE=OFF` unless explicitly
requested, builds with two parallel jobs, and runs CTest with `--no-tests=error`
and JUnit output. `--jobs N` changes parallelism; `--timeout N` sets a positive
per-subprocess timeout in seconds, default 1200. A configure/build/test failure
stops later phases and returns a nonzero exit status. There is no expected-failure
inversion or failure-swallowing path. Tool-version probes have a 30-second timeout.
The shared process runner owns a Windows job/process tree or a private Unix
session. Timeout terminates that tree, returns 124 and allows at most three extra
one-second cleanup waits on Windows or one on Unix. Output is captured into
temporary files, so inherited pipe handles cannot extend the deadline indefinitely.
Background descendants are cleaned up even after normal parent exit. Commands
must not deliberately escape their Unix session. Process creation and OS file
operations themselves cannot be interrupted by Python's subprocess deadline.

Evidence is written under `out/ci/<preset>/`: `summary.json`, `toolchain.json`,
phase logs and `ctest.xml` when CTest ran. The summary lists exact commands and
exit codes; an unexecuted phase has no result and is not a PASS. Previous phase
logs and JUnit are removed at invocation start to avoid stale successful evidence.
Only a fixed allowlist of non-secret environment metadata is recorded. Local
revision capture reports a missing Git repository honestly. Artifacts are uploaded
with distinct platform/architecture names and retained for 30 days; copy required
acceptance evidence to durable release/handoff storage before expiration.

## Repeating the red/green proof

The assignment 01 proof is complete: [injected run 39077652643](https://github.com/markdawes08/shark-engine/actions/runs/39077652643)
followed by [restored run 39078070735](https://github.com/markdawes08/shark-engine/actions/runs/39078070735),
both at `3620fab`. M01's separate proof uses
[injected run 39082507455](https://github.com/markdawes08/shark-engine/actions/runs/39082507455)
and [restored run 39083098689](https://github.com/markdawes08/shark-engine/actions/runs/39083098689),
both at `d658d69`; it includes the new headless job. The
[M01 hosted follow-up](handoffs/M01-ci-verification.md) records their actual
assertions, phase exits and remaining qualification limits.
Use this procedure when a future build/CI change requires renewed
failure-propagation evidence. Finish the first dispatch before starting the
second; the workflow's concurrency policy can cancel overlapping dispatches.

1. Open the repository's **Actions > CPU foundation > Run workflow** on the
   intended revision. Set `inject_test_failure` to **true**. The six CPU jobs
   must reach CTest, execute the deliberately failing test and fail. A tool-install,
   configure or compilation failure does not prove test failure propagation.
2. Save the run URL, commit, runner metadata, each CPU job's nonzero test-phase
   exit code and JUnit containing the injected test failure. `quality` and
   `runtime-only` may remain green; the overall workflow must be red. The M01
   `headless-server` job also receives the injection input; its test-enabled
   configuration must reach the real sentinel assertion, not fail preparation.
3. Dispatch the same revision again with `inject_test_failure` **false** (the
   default). All nine current jobs must pass, and no injected failure may remain in
   CTest. Save this run URL and artifacts alongside the failed-run evidence.
4. Update the current assignment's handoff and requirement status only after checking
   both actual runs and the two native CPU rows. Ordinary failed push runs do
   not prove deliberate test failure propagation. Until the injected failure
   and restored pass are verified on the same revision, a new red/green claim
   remains unverified. Keep the previously verified revision explicit.

The same real failure can be checked locally, for example in a Windows developer
shell: `python tools/ci.py --preset windows-debug --inject-test-failure` must fail;
then `python tools/ci.py --preset windows-debug` must pass. The second invocation
explicitly resets the cached option to OFF. Preserve the first output directory
before rerunning if both evidence sets are needed. Local proof meets the amended local failure-propagation gate; it does not claim
that a hosted job executed. Repeat a hosted proof only when separately verifying
hosted CI, without making access to a runner a local roadmap prerequisite.

For the headless profile, use a fresh output directory for each invocation:

```sh
bash tools/ci/headless/run.sh out/ci/headless-injected --inject-test-failure
bash tools/ci/headless/run.sh out/ci/headless-restored
```

The first command must return nonzero because CTest ran the injected assertion.
The second must finish all normal stages, including the clean-runtime smoke.
The package-only configuration keeps injection disabled because it has no Tests.
The headless driver stops at the first failed test phase; phases after that point
remain unrun in the injected evidence. The historical 01 red/green reports keep
their original fourteen-job/120-test counts and tested revision.

## Configuration verification

`python tools/check_ci_config.py` parses the YAML and checks matrix coverage,
immutable action references, timeouts, least-privilege checkout wiring and the
manual failure input. It does not execute GitHub expression evaluation or jobs.
`python tools/check_format.py` checks engine-owned C/C++ files against the pinned
formatter; `--fix` deliberately applies formatting. No future GPU, import or
release tests are invented for this foundation.

M03c adds `shark.storage.transactions` (90 seconds, seed 1337) and
`shark.storage.transactions-process` (180 seconds). Keep the original storage,
character and all network families. Its two-client process barrier supplies
simultaneous stale intents to one server owner; the same real database then
reopens in a fresh process for retained retry and expiry checks. Business
rejections consume durable identities, while session commands remain separately
sequenced. The [public transaction example](durable-transactions.md) compiles
without SQLite/TLS headers. Ordinary open explicitly rejects historical versions
1/2; M03e supplies separate offline migration. M03d adds `shark.storage.recovery` (120 seconds, seed 1337) and
`shark.storage.recovery-process` (240 seconds). Preserve all earlier families.
Its 69-row process matrix requires 207 real children and observed native-VFS
write/sync/full errors, with exact reopened/retried state and strict negative
receipt controls. Runtime service tests use actual mTLS, Scene references,
controlled clocks and deterministic worker barriers. Compile the
[public recovery example](durable-recovery.md) with public headers only, and
verify the private worker/SQLite source closure and protected packages.

M03e adds `shark.storage.migration` (120 seconds, seed 1337) and
`shark.storage.migration-process` (240 seconds), preserving earlier families and
their limits. Independent schema-1/2/3 fixtures and exact typed state checks
qualify sequential upgrades, required pre-upgrade backups, idempotent commit
reconciliation and create-only isolated restore. The process matrix requires
66 rows / 200 real children and 24 negative receipt controls. Its two hot-journal
rows observe a successful native main-file write during COMMIT, verify that
read-only inspection rejects without changing either file, then recover and
upgrade in a fresh process. The shared test VFS retains the earlier 69-row
recovery contract. Compile the [public maintenance example](durable-maintenance.md)
with public headers only; audit both probe copies of the VFS in sanitizer builds.
Preserve exact backup contents, economic operation retention, revocations and
fresh authentication across restored restart. An isolated old snapshot is not
authority to roll back acknowledged state. M03f closes integrated acceptance
through the [connected qualification](durable-qualification.md).
