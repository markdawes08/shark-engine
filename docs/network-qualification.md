# Integrated network qualification

M02e integrates the existing [transport](network-transport.md),
[session](network-sessions.md), [replication](network-replication.md) and
[interest](network-interest.md) services. Its execution and final status belong
in the [handoff](handoffs/M02e-integrated-qualification.md). The production wire,
authentication requirements and public pose allowlist are unchanged.

M02e and full REQ-M02 are PASS under the current-machine scope. The
[qualification receipt](coverage/evidence/M02e-integrated-qualification.json)
records 474 passing CPU CTests across seven profiles, protected package checks
and 277 unchanged source/build/test/fixture inputs. Assignment 08, Project and
scene persistence, is next.

## Fault layers and recovery

An opaque loopback relay holds intact TLS bytes in FIFO order. Its configured
one-way delay is 50 or 100 ms, with seeded additional jitter of 0..30 ms in each
direction. These profiles model nominal 100/200 ms RTT plus jitter; they do not
measure an Internet path. Application-message loss, duplication and reordering
are injected separately, after authenticated replication frames are decoded,
through the existing bounded `FaultLink`. No corrupted TLS bytes are described
as packet loss. A partition scenario closes the affected relay connection and
requires an explicit authenticated reconnect while the other recipient advances.

Managed publication retains one pending update and does not retransmit it.
Losing that update therefore requires bounded resynchronization or reconnect.
ACK and resync controls share the session's exact command sequence. A fixture
uses the existing `SessionReply` as a control-admission confirmation, allowing
only one outstanding control and advancing its sequence after acceptance.
This is fixture orchestration, not a new wire kind, gameplay transaction or
shipping authentication bypass. Uncertain admission after disconnection uses
fresh authentication and session/stream fences rather than guessing a sequence.

The process matrix has six scenarios and 21 children: one server and two clients
per scenario, plus one relay for each shaped profile and the partition. Its
66-row catalog contains 32 owner-visible actors and one hidden observer for each
recipient. Independent 20 Hz source mutation feeds publication capped at 10 Hz.
Baseline, partition, slow-reader and hostile scenarios run 120 source ticks;
the two seeded profiles run 320 ticks before healthy recovery. The 1% semantic
loss profile uses seed 1; the 5% profile uses seed 1337. Both additionally select
duplication at 50% and reordering delay at 30%, with 0..30 ms semantic jitter
and a 1500 ms reorder delay. Receipts must show actual losses, duplicates and
delivered reorder inversions, not merely enabled probabilities.
The unaffected recipient must apply at least 30 useful updates in each six-second
scenario and 80 in each sixteen-second scenario; baseline recipient A also needs
at least 30. These conservative cadence floors are fixed before qualification.
The fixture compares two `FaultLink` instances using identical real submission
and drain timestamps and payloads. This verifies deterministic replay in memory;
it is not an offline saved trace or a promise of identical OS scheduling across
runs. Existing golden-byte/trace unit tests separately fix algorithm behavior.
`ack_latency_ms` measures control submission through its confirming session reply,
including transport delay and server handling, rather than the age of source state.

The slow reader pauses polling for 700 ms against an explicit 500 ms application
ACK deadline, then reconnects. The partition relay withholds bytes for 500 ms,
closes both endpoints and accepts the fresh connection. A 55-second internal
watchdog and 60-second process-runner deadline bound each child. The relay stores
at most 16 KiB per direction. The semantic queue stores at most 16 packets/64 KiB
and drains at most eight copies per call; transport queues retain at most four
frames/16 KiB, with at most 16 poll operations per call.
The fixture allocates two accounts, characters, certificates, live sessions and
replication streams, with three listener slots to permit replacement connections.
It explicitly selects 32 resync admissions per ten-second window to exercise the
aggressive semantic fault schedule; the production default remains four. The ACK
deadline is five seconds except in the deliberate 500 ms slow-reader case.
Existing unit tests retain default rate-limit and rejection coverage. Passing
these configured scenarios does not imply unlimited retries or identical recovery
latency under the default retry budget.

## Evidence responsibilities

| Behavior | Executable evidence |
| --- | --- |
| Two active recipients, privacy, exact final poses, shaped transport and semantic adversity | `shark.core.net-adversity-process`, test-only `NetAdversityProbe` and bounded Python relay |
| Retained storage during 32 full population replacements and resynchronizations | `shark.core.net-adversity` with actual Scene, session, interest and mTLS objects |
| Repeated reconnect and ACK-deadline recovery, stale authority and cleanup | Three 32-round lifecycle cases across `shark.core.net-adversity` |
| Invalid decoded state leaves cache values and owned storage unchanged | `shark.core.net-adversity`, existing replication and wire families |
| Sequence/epoch exhaustion and replica revision/identity fences | Near-maximum public helper and decoded-cache cases; real session rejects forged future sequences |
| Invalid, expired, revoked and foreign credentials; malformed/versioned session traffic | Refreshed mTLS/session unit and separate-process families |
| Oversized outer declarations, truncated/invalid codecs and queue pressure | Refreshed wire/transport/replication unit families; parser evidence is distinguished from encrypted process traffic |
| Socket-byte budgets, crowded selection, membership churn and FIFO delay | Refreshed M02d process fixture: 128 candidates, two recipients, 32 visible poses each, 20 Hz source and 10 Hz publication |

Exhaustion tests exercise the checked helpers used by admission, plus actual
session rejection and decoded client boundaries. They do not send 2^64 commands
or add a production setter to bypass sequence progression.

## Resource accounting

Public owner-thread statistics expose retained logical counts and actual vector
capacity bytes. Per stream, mappings, acknowledged poses and pending target
poses are bounded to 32, pending change records to 64 and pending updates to one.
Interest selection and last-sent state each retain at most 32 handles. Client
caches retain at most 32 visible entities/live-ID fences and 64 latest records.
The tests measure capacity as well as live size and verify exact-session cleanup.
Reserved stream slots persist until service destruction. Clearing a client
baseline intentionally retains identity fences and may retain allocated capacity.

These observations exclude allocator overhead, temporary validation candidates,
caller-owned snapshots, TLS allocations and kernel buffers. Separate owned-process
sampling records resident memory and Windows handle/Linux file-descriptor counts.
The fixture predeclares a 1 GiB resident-memory and 512 handle/descriptor ceiling
per process, including sanitizer processes. These are acceptance ceilings on
sampled values, not OS-enforced limits or bounds on unsampled peaks. They are not
production memory budgets or proof of a long-running leak-free server.

## Running and scope

Build the normal preset, inspect its inventory, then select the network families:

```text
ctest --preset windows-debug --show-only
ctest --preset windows-debug -R "^shark.core.net-" --output-on-failure
```

Use the corresponding native WSL presets as described in [building](building.md).
The integrated unit family has a 60-second timeout; the process family has a
360-second timeout. Generated process receipts, relay barriers, PID files and
resource samples stay under `build/` or `out/`. Test-only keys and executables
are excluded from installed Runtime/Server packages.

The final gate includes Windows Debug/Release/ASan, WSL CPU client and headless
Server Debug/ASan+UBSan, protected Runtime/Server builds, static dependency and
license closure, isolated packages, public-header/example and sanitizer audits.
Each receipt identifies exact compiled inputs. Source changes require fresh
qualification; a passing subset cannot be combined across different inputs.

The achieved qualification scope is two local authenticated recipients and
bounded network fixtures. The full 16-player LOCAL-MMO-1 workload, region transfer, gameplay,
durability and endurance retain their later assignment owners. Authoring CLI,
editor and FeatureWorld/MMOWorld integration retain 15/16 and 10/M09.
