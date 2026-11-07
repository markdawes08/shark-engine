# M05g: connected authored-world acceptance

Date: 2026-11-06. Base: `652a802` on `main`, initially clean.
Follows [M05f](M05f-process-region-execution.md), the
[M05 checkpoints](../roadmap/seamless-world-plan.md),
[ADR 0069](../architecture/0069-connected-world-streaming.md) and
[acceptance procedure](../seamless-world-acceptance.md).

## Delivered behavior

Real Server children now retain a bounded immutable cell catalog and use the
existing asynchronous reader/residency owner during native gameplay. Explicit
drain publishes residents; activation/deactivation updates the same collision and
navigation owners. Missing/corrupt input, cancellation, pending/resident pressure
and retry preserve last-valid content. Ticks may checkpoint committed residency
while IO is pending. Transfer requires every original job to settle and be taken;
gateway tickets include the child incarnation and cannot alias a replacement job.

NPC pursuit checks the actual current target session and life before native
movement. It uses native support and bounded vertical-only navigation projection,
fresh current-graph paths and checked native progress. An already adopted world
steering anchor survives float rounding, handoff and recovery; it is discarded
when current support or graph validity no longer agrees. Existing 0.02 m horizontal
and 0.04 m support agreement bounds and path tolerances are unchanged.

Process image v2 adds catalog sources/residency, limits and pursuit values. The
legacy v1 decoder retains active inline cells and exact authored native materials.
Lossless cell-aligned frame changes are available only when staging a new child.
Complete global positions and packed native poses, targets and geometry must
survive exactly; unrepresentable shifts reject before freezing or SQL. Source
handles remain relocation keys. The gateway publishes roots, interest and movement
corrections in its original client frame, retaining existing wire formats and
session/stream/life/replica identities. Jolt solver caches are still reconstructed.

Only validated collision proxies can change the otherwise fixed gameplay topology.
Occupied/contact/spawn content cannot be detached unsafely. Post-effect capture,
encoding, checkpoint or publication failures fence execution for recovery rather
than exposing an incomplete canonical state. Normal close checks actual child
exit status zero; deliberate recovery termination remains a separate exit fence.

## Connected observations

The fixture saves and reopens three real authored terrain scenes, exports native
cell files through the existing Tools/ProjectFiles owners and independently checks
literal v2 geometry. Actual Server children read those files. The same workload
runs at global X cell 0 and 1,000,000,000 with equal local observations, without
forming enormous floating-point world coordinates.

Native player movement stops at an unavailable footprint, then crosses the
distinct logical-region edge and streamed content seam after readiness. Actual
NPC pursuit crosses both edges. A live projectile survives a real process/frame
handoff and lost completed-step reply recovery, producing one original hit and
health 89. A second handoff preserves the return crossing. The first destination
shifts origin by one cell; the return retains that frame rather than claiming
arbitrary reverse shifts remain lossless after native motion.

Actual mTLS carries intents, reliable predictor corrections and replica traffic.
Seeded delay/loss/duplicates/reorder/partition affect verified combat observations
after receipt, not TLS bytes. Stale observations reject without changing the cache;
the final healthy observation reconciles one presentation effect. Replica IDs and
stream remain stable. Original economic request/ticket/writer identity survives
recovery and later handoff, with balances 93/18 and 107/22 exactly once.

The retained M05f process matrix still covers source/destination loss and partition
across prepare/ownership phases. New process cases additionally drop an admitted
combat reply and a completed-step reply, checking original admission recovery,
last-acknowledged tick/rules, one outcome and original durable receipts.

## Verification

The selected affected inventory contains 20 CTests:

```text
^shark\.(core\.(physics|world-position|world-cell|world-cell-residency|world-traversal|movement-prediction|combat-rules|combat-client|net-replication|net-interest)|server\.(movement|combat|gameplay-lifecycle|regions|transfer|region-process|region-image|seamless-world)|tools\.(world-cell|world-cell-authoring))$
```

Windows Debug: **20/20 PASS**, 21.63 seconds. Build uses the matching preset through
`tools/windows_dev.ps1`, two jobs. Inventory, JUnit and complete output:
`out/M05g-work/windows-selected-inventory.log`, `windows-final.xml` and
`windows-final-output.log`. Protected Server/Storage/Core/Tools boundaries pass;
only the test host links Tools to the Server fixture. Source, scene/history and
actual process/TLS checks remain separate from GPU or native UI evidence.

WSL Clang ASan+UBSan: **20/20 PASS**, 44.30 seconds, leak detection and
halt-on-error enabled. The retained canonical
`/work/build-05a-final-client/linux-asan-ubsan` build uses two jobs and the existing
isolated userspace; no host/WSL resource or kernel settings changed. Inventory,
build output, complete test output and JUnit are under `out/M05g-work/wsl/`.
Normal worker teardown checks actual zero exit status and inherits sanitizer
stderr. No compiler, dependency or instrumentation setting changed.

Windows connected telemetry, eight child samples per near/far workload:

| Measurement | Near | Far |
| --- | ---: | ---: |
| Largest sampled child working-set, bytes | 38,006,784 | 38,047,744 |
| Logical resident high-water charge, bytes | 34,084,894 | 34,084,894 |
| Logical pending high-water charge, bytes | 4,459,168 | 4,459,168 |
| Largest observed handoff quiescence, ms | 66.7941 | 67.4551 |

The two-cell cap and one-complete-reader-reservation pending cap are exercised;
each child sample is below the fixed 1 GiB functional limit. These are sampled
working-set values, not process-lifetime peaks or production capacity. Both
Windows quiescence observations are below the provisional 500 ms budget; timeout
still grants no authority. Each near/far observation link records 5 loss drops,
11 duplicates, 19 reorders, 2 partition drops and 12 stale observations, with no
backpressure and one original presentation effect.

WSL records eight samples per workload, largest child VmRSS 115,224,576 bytes
near and 112,099,328 bytes far. Its logical resident/pending high-water charges
are 34,084,750 / 4,459,096 bytes in both workloads; platform allocation accounting
is separate from actual process RSS. Largest observed quiescence is 153.415171 ms
near and 156.472465 ms far, with no provisional budget overruns. Verified
observation fault counts and once-only effects match Windows.

Initial failed/compiler runs remain under `out/M05g-work/`. The first compiler
failure was Windows SDK include ordering in the test-only RSS helper. The first
connected harness treated expected stale observation rejection as a test failure;
it now checks that precise rejection and unchanged client state. Later
runs exposed the native portal stall above and an expected writer incorrectly
captured before the request's first handoff. The anchor fixes the stall; the test
now compares the actual original submission writer. Review also fixed packed
kinematic-target frame validation and contextual post-effect capture errors.
No tolerance, workload, baseline or original acceptance criterion was relaxed.

Unchanged M05a-f qualification remains historical evidence in its original
handoffs. This selection reruns the complete affected content/traversal/gameplay
owners and their prediction/replication consumers. Store schema, SQL worker,
terminal protocol, GPU, packaging and deployment inputs are unchanged; their
qualified previous results are reused. No whole-project matrix, Release/package,
GPU or manual desktop run was selected for this CPU integration.

## Continuation

M05g passes the declared current-machine headless functional profile. Contracts,
coverage mappings, repository checks and changed C++ formatting pass.
Full REQ-M05 remains NOT_RUN for native World cells interaction under
[BL-M05D-UI-01](../backlog.md#deferred-items). Manual UI is paused; headless or
synthetic results do not close that gate. The existing backlog amendment permits
continued implementation. M06, animated characters and dense-world presentation,
is next in manifest order; plan its reviewable checkpoints before implementation.

This profile moves one complete interacting island, with one executing child at a
time. It does not qualify simultaneously independent neighboring simulations,
gateway-session cold restoration, automatic SQL-fault recovery, production
capacity, final cooked manifests or later LOCAL-MMO-1 load/endurance. Keep those
boundaries and original request identities explicit.

`.gitignore` was reviewed; existing managed `out/` and `build/` exclusions cover
all generated files and evidence. Source/fixtures/docs remain eligible. Root
README is unchanged. No staging, commit or push was performed.
