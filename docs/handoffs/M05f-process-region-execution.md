# M05f: process region execution

Base: `2798879 feat(world): add fenced regional handoffs and durable ownership`,
clean `main` at start. The user owns staging, commits and pushing. Root README
is unchanged. Follow [ADR 0068](../architecture/0068-process-region-execution.md),
the [process contract](../region-process-execution.md) and
[M05 checkpoints](../roadmap/seamless-world-plan.md).

## Delivered behavior

Actual owned `Server --region-worker` processes now run the existing native
movement/combat algorithms through one verified-host seam. The gateway keeps
the actual session authority, original durable worker/tickets and stable recipient
Scene/interest streams. It borrows current TLS transports per call, preserving
disconnect/release lifetimes. Children have no player listener, database, source
importer or autonomous ticks.

The transfer unit is the complete interacting island: two through eight logical
regions share its fixed frame, hierarchy and prepared content. Every player writer
changes instance atomically, including stationary players. It does not split
native contacts or combat between simultaneously ticking independent islands.
The initial host bootstrap binds actual ready sessions at tick zero, then releases
its native owners before the first executing child is installed.

An explicit bounded BE image reconstructs real Scene, PhysicsWorld, static cell
collision/traversal, movement, combat and NPC owners without fake ticks. Exact
relocation maps preserve queued inputs, stream/life identities, health, cooldowns,
dead poses/deadlines, history and original flight decisions. Native values include
authored mesh/body settings, current/previous poses, linear/angular velocity,
pending force, kinematic targets, active contact pairs and contact sequencing.
Stale flight participants remain unavailable. Source handles never become fresh
authority. Jolt solver caches are rebuilt; identical future solver trajectories
are not claimed.

Transferable rules opt into a fresh VM per evaluation from their first use. Only
eight checked int64 slots and the evaluation count persist alongside captured
source/configuration. Ordinary mutable VMs and entity ScriptRuntime state reject.
This does not change their existing co-located support.

The creating-thread journal owns two bounded published slots and exact replacement
digests. It binds actual portable bytes before SQL, rather than persisting only a
diagnostic fingerprint. `commit_regions` checks all source writers and commits all
successors in one schema-4 transaction. It rejects mixed/subset retries and later
attempts to split the retained source group. The existing runtime worker uses one
bounded control slot, preserves original batch requests and publishes cached owners
only after the complete result is checked. Original economic/save jobs can remain
retained throughout. Immutable Store/world metadata is cached at gateway startup;
live checkpoints do not invoke blocking maintenance behind those pending jobs.

Input preparation reaches the same lower validation/admission boundary without
changing gameplay. The gateway spends the actual shared session sequence once,
then commits the immutable input and its original receipt to the child. Complete
route/tick state is checkpointed before publication. Up to eight post-cut intents
can wait without admission. A lost admitted reply freezes further execution and
retains the original route; recovery replays only receipts above the durable
watermark, without another session admission. Wrong-player durable takes reject
before consuming the original slot.

Normal transfer requires source freeze acknowledgement, a staged destination and
the exact all-writer SQL result. Loss/partition recovery confirms actual child exit
before a fresh incarnation and successor CAS. A timeout cannot revive a writer.
Unknown SQL stays unresolved until the existing durable-owner maintenance path
reconciles it. Gateway/session cold restoration and physical-host failover are
outside this child-recovery profile. Quiescence telemetry retains the provisional
500 ms budget and reports overruns without granting timeout authority.

Control frames have strict length/sequence/replay bounds. Windows children use
hidden jobs and restricted inherited handles; Linux children use owned pipes,
process groups and parent-death termination. Normal checked close tears down
native owners, acknowledges, then requires actual exit status zero. Separate
inherited stderr preserves sanitizer diagnostics. Recovery's intentional hard
termination is an exit fence, not clean-shutdown qualification.

## Verification

Checks follow the [testing policy](../testing-policy.md). The selected subsystem
contains 18 CTests, not the configured build's full inventory:

```text
^shark\.(core\.(physics|combat-rules|lua|script-runtime|world-traversal)|storage\.(store|region-ownership|region-process|characters|transactions|recovery)|server\.(movement|combat|gameplay-lifecycle|regions|transfer|region-process|region-image))$
```

Windows Debug passes the five selected Core families, four image/Store families,
eight existing gameplay/runtime-storage families, and the corrected process family.
Valid unchanged results are reused across the focused fixes. Primary records:

- `out/M05f-work/windows-core-tests.xml`: five passes, 1.07 seconds.
- `out/M05f-work/windows-image-storage-tests.xml`: four passes, 2.66 seconds.
- `out/M05f-work/windows-server-tests.xml`: eight unchanged families pass; its
  initial new process failure is preserved below.
- `out/M05f-work/windows-process-tests-3.xml`: final NPC image validation passes.
- `out/M05f-work/windows-process-final.xml`: seven real-process cases pass after
  the final required-NPC publication and durable-failure coverage changes,
  4.57 seconds.

The process cases verify both flights across transfer, exact FIFOs/rule slots,
fresh whole-island writers at the crossed logical region, stable replica IDs and
stream, active contacts, one-use native force/NPC intent, bounded unadmitted
buffering, actual source/destination death before/after SQL, dropped real control
requests/replies, original admission replay, original economic tickets/effects
during process loss, dead-life pose/input/deadline continuity, respawn, released
TLS transports and checked clean worker shutdown.

WSL's retained Clang ASan+UBSan build passes the same 18 families in 57.19 seconds,
with leak detection and halt-on-error. Records are `out/M05f-work/wsl/`.
The final process-only follow-up in `out/M05f-work/wsl-final/` passes in
11.15 seconds and covers the last required-NPC policy and expanded original-economic
failure assertions; unrelated
families are not repeated. Both platforms retain seed 1337, explicit CTest
timeouts and controlled gameplay clocks. Each inventory is listed before its
selection. The process fixture uses actual OS children and actual mTLS; dropped
requests/replies are deterministic closed-link fault observations, not WAN tests.

Windows builds use matching `windows-debug` presets through
`tools/windows_dev.ps1`. WSL reuses
`/work/build-05a-final-client/linux-asan-ubsan` in the retained isolated userspace;
both use two build jobs. Server/Storage/Core protected dependency checks pass.
No host/WSL settings or dependency versions changed. No GPU, manual UI,
Release/package or whole-project run was selected: this increment changes CPU
native transfer, closed process control and durable authority, with no presentation
or package format change.

Initial compiler failures were a shadowed batch variable, Windows `quoted` ADL,
and a test's non-default-constructible empty CombatCut. Initial process setup
omitted its required managed journal directory; its next lifecycle fixture used
100 damage despite the existing 25-damage bound. The fixtures now create the
directory and use four legal strikes at the original cooldown. Failure logs and
JUnit remain under `out/M05f-work/`; no tolerance, workload or profile limit was
relaxed. Shared test diagnostics now survive until the failing assertion.

Review also corrected actual lifetime/authority issues: retained raw transport
pointers, wrong-player completion consumption, stale final command watermarks,
pending-job maintenance reads and poisoned prepare channels. Exact native material
settings and imported NPC intent bounds are validated at their owning factories.

## Continuation

M05f passes its declared current-machine functional profile. Full REQ-M05 remains
NOT_RUN. M05g connects authored continuous travel, actual process transfers and
faults, targeting/AI/traversal readiness, distant precision, streaming pressure
and resident budgets. Reuse the qualified owners; do not substitute independent
pose mirrors or infer independent neighboring simulation/host availability from
this whole-island profile. Final cooked manifests remain with 31/M08; later
LOCAL-MMO-1 load/endurance remains with its assigned milestones.

The manual UI pause and independent backlog statuses are unchanged. `.gitignore`
was reviewed: all new generated journals, builds and evidence use existing managed
`build/`/`out/` exclusions; source/fixtures/docs remain eligible. No new ignore
pattern was necessary. No staging, commit or push was performed.
