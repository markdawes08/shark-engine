# M02d: Spatial interest, privacy and connection budgets

Date: 2026-10-18. Starting revision:
`9b10b9110135d7084f17a765d5a621ddde8a6471` on `main`, initially clean.
Changes remain uncommitted; the user owns staging, commits and pushes.

**M02d is PASS under the current-machine scope.** The final source freeze passes
all 460 CPU CTests with zero failures/skips, alongside protected builds, sanitizer
audits and isolated packages. The [receipt](../coverage/evidence/M02d-spatial-interest.json) records
exact inputs and results. Full **REQ-M02 remains NOT_RUN** until M02e integrated
qualification. The full LOCAL-MMO-1 workload retains its later owners.

## Delivered behavior

[ADR 0014](../architecture/0014-spatial-interest.md) and the
[interest contract](../network-interest.md) define the new CPU service above the
existing Region Scene, session authority and TLS transport.

- A complete trusted catalog supplies at most 128 candidates. The actual session's
  CharacterId binds its server-owned observer. Default-hidden, public-pose and
  owner-pose policies are independent of authoring metadata and player messages.
- Interest and replication validate source scope/revision with a fields-disabled
  Scene query limited to one scanned slot and one returned row. This replaces
  a whole-Scene statistics traversal, keeping source validation independent of
  unrelated Scene population while retaining empty/sparse Scene and lifetime checks.
- Selection publishes at most 32 poses, using inclusive 100/110-meter entry/exit
  hysteresis. Required membership, priority, retained membership, distance and
  handle form a deterministic order. Too many eligible required actors reject;
  optional omissions are reported. World shear remains intact in the pose schema.
- Complete policy validation and at most 32 last-sent exposure checks run before
  throttles or ACK admission. Newly forbidden or uncertain policy closes the actual
  connection, revokes its exact session and drops queued frames. Ordinary spatial
  exit or destruction produces despawns. Already transmitted bytes cannot be recalled.
- Managed publication captures and encodes a candidate before TLS admission, then
  commits replica IDs/revision/ACK state without further throwing allocation.
  Queue/byte deferral preserves those values. The next attempt reads current Scene
  poses; admitted reliable frames remain ordered. Resync preserves cadence and
  waits for an empty application output queue. Manual and managed modes cannot mix.
- Default publication cadence is 100 ms, with 48 KiB/s framed-byte credit, an 8 KiB
  burst and 8192 selection work units. Overflow-safe refill, exact admission charges,
  bounded metadata and separate policy/selection/privacy counters make deferrals
  observable. Work counts are algorithmic bounds, not hard CPU-time guarantees.

Public changes are additive in `shark/net/interest.hpp` and
`shark/net/replication.hpp`. New source, unit and process fixtures are registered
in Core/tests CMake. The existing wire version/client cache remain compatible;
no dependency or platform change was needed. Human and terminal developers use
these same CPU APIs, an independently compiled public example and JSON evidence.
The manifest adds `core.spatial-interest`; authoring adapters retain 15/16 and
FeatureWorld/MMOWorld retain 10/M09. `.gitignore` documents the generated interest
traces/barriers under ignored output roots and keeps the local roadmap ignored.

## Behavior and measurements

`shark.core.net-interest` has a 30-second timeout and seed 1337. Its 20 named
cases cover trusted observers/private recipients, exact hysteresis,
priority/required overflow, tiny/extreme radii, affine shear, identity lifetime,
coalescing, exact token thresholds, large-time overflow, work exhaustion, privacy
before controls, malformed catalogs, reconnect, resync FIFO and owner/time fences.
Service behavior uses actual mTLS peers and controlled monotonic clocks.
The existing replication suite now has 21 named cases, including atomic managed
admission, latest-unsent capture, mode separation and empty/sparse Region Scene
validation with lifetime fencing.

`shark.core.net-interest-process` has a 120-second timeout. Each profile passes
three scenarios in ten children: crowd/churn/budgets, policy revocation and
reliable-stream head-of-line delay. Two enrolled clients each receive 32 moving
poses from 128 candidates; source mutation runs at 20 Hz independently of ACKs,
publication at 10 Hz. A 175 ms warm-up ACK delay exercises coalescing. After two
seconds of warm-up, twenty elapsed-normalized one-second windows measure actual
socket traffic, including TLS overhead. Clients independently check every visible
set/pose and reach source tick 440, with at least 190 measured applied updates.
The percentile rule is nearest rank; join traffic is measured separately.

Observed ranges below span seven profiles or fourteen recipient/profile pairs.
They are ranges of recorded percentiles, not pooled percentiles.

| Measurement | Observed | Gate or interpretation |
| --- | ---: | --- |
| Measured applied updates per client | 199-200 | >=190 in 20 seconds |
| Socket downstream p95 | 34.49-34.54 KiB/s | <=64 KiB/s |
| Socket upstream p95 | 1.30-1.30 KiB/s | <=16 KiB/s |
| Join downstream / upstream maximum | 4707 / 1252 bytes | <=32768 each |
| Selection-call wall p95 | 0.25-5.08 ms | Informational elapsed work |
| Selection-call wall p99 | 0.55-6.50 ms | Informational elapsed work |
| Selection work peak | 2821 units | <=8192 per selection |
| Application queue peak | 3753 bytes / 2 frames | <=16384 bytes / 4 frames |
| Shaped admission-to-ACK completion | 544.26-562.88 ms | 300-3000 ms |

All-call timing includes cadence/pending outcomes; actual selection-call timing is
recorded separately. These wall-time distributions do not measure process CPU time
or qualify the full simulation tick budget. Exact raw windows, work counters and
per-profile distributions are retained with the receipt.

The relay holds the first affected downstream bytes for at least 100 ms, then
forwards intact end-to-end TLS bytes at 8 KiB/s with a 16 KiB directional buffer.
A real typed Heartbeat reply follows a full state frame and cannot overtake it.
The other client continues progressing. Unshaped initial full-state ACK and
SetReady reply RTT are separate reference intervals, so no slowdown ratio is
claimed. This is controlled reliable-stream delay, not packet-loss emulation;
it does not establish combat latency suitability, which retains M02e/M04 gates.
All privacy scenarios discard a queued revoked frame before polling, preserve
socket-byte counters at closure, and reconnect with only the new allowed selection.

## Qualification

| Gate | Result |
| --- | --- |
| Windows MSVC Debug / Release / ASan | PASS, 74 CTests each |
| WSL GCC CPU client Debug / Clang client ASan+UBSan | PASS, 67 each |
| WSL GCC Server Debug / Clang Server ASan+UBSan | PASS, 52 each |
| Windows Runtime-only / Server-only build, lifecycle and install | PASS |
| WSL Release Server source/link/ELF/license/package closure | PASS |
| SDK-free WSL installed Server | PASS, 11 files and unchanged 92-package inventory |
| Fresh Windows Runtime package and isolated launch | PASS, including all 10 missing-notice rejections |
| Public/private compile inputs and public C++ example | PASS |
| Sanitizer compile inputs | PASS, 116 private TLS C units in Windows and both WSL Clang profiles |

All seven full profiles pass all twelve network CTest families and preserve the
earlier transport/session/replication process fixtures. Windows and WSL record
the same 274 replacement frozen source/build/test/fixture inputs with no changes
during the final qualification runs. All 17 public PEM fixtures and three pinned
archives are unchanged.
Runtime/Server retain private static TLS/EnTT, central public GLM, all eight TLS
notices and the exact installed Server payload. No test key, probe or relay tool
enters a package. Formatting covers 150 C++ files; contract/repository negative,
whitespace, ignore and empty-index checks are recorded in the receipt.

The receipt also preserves both earlier attempts. The first Windows execution
passed all 222 native CPU tests, then detected the bounded Scene probe fixes and
new replication regression before its final source audit; the driver recorded
FAIL and those results were superseded. The first WSL Server ASan+UBSan run timed
out in `shark.contract` at 30.04 seconds while its other 51 tests passed.
Unchanged diagnostic checks passed, but did not replace that failed CTest gate.
Stack samples identified repeated path resolution over the Windows-mounted
source. Contract validation now uses bounded 4096-entry caches for resolved link
and surface paths within each invocation, preserving per-reference diagnostics.
Regression checks cover path aliases and fresh resolution after deletion,
recreation or symlink retargeting in a later invocation. No timeout or acceptance
limit was relaxed. The replacement source freeze, original freeze,
failed/superseded logs and complete new qualification results are identified
separately by hashes in the receipt.

Reproduce the focused registered fixtures after building:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset windows-debug --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R ^shark.core.net- --output-on-failure
```

Exact retained qualification entry points:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M02d-work/verify-windows.py
wsl -d Ubuntu -u root --exec bash /mnt/c/Users/mrkda/Projects/shark/out/M02d-work/wsl-run.sh
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M02d-work/verify-runtime-package.py
python out/M02d-work/api-audit.py
python out/M02d-work/audit-sanitizers.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M02d-work/compile_documented_example.py
python out/M02d-work/measurement-summary.py
```

These ignored local drivers retain exact commands, logs, cleanup results and
source hashes. Ordinary registered fixtures and [build instructions](../building.md)
do not depend on ignored helpers or the local roadmap.

## Limits and next checkpoint

The active two-recipient fixture establishes this network checkpoint's per-connection
budgets. The full 16-player/NPC, two-region, gameplay and endurance workload remains
unqualified. No production capacity, WAN behavior or additional-machine support is
claimed. Installed bootstraps still have no public player listener. Windows
development tools remain installed; isolated WSL userspaces share this PC/kernel.
GPU behavior did not change. Durable storage, combat/prediction and global-origin
region transfer retain M03, M04 and M05 respectively.

Next: **M02e - Integrated qualification**. It closes full REQ-M02 through the
planned real-process adversity, resource bounds and recovery matrix.
