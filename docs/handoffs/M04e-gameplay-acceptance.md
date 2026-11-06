# M04e: connected authoritative gameplay acceptance

Date: 2026-11-05. Base: `8430d84`, initially clean `main`. The user owns staging,
commits and pushing. Follow the [acceptance contract](../gameplay-acceptance.md),
[M04 plan](../roadmap/authoritative-gameplay-plan.md) and
[testing policy](../testing-policy.md). M04e and full REQ-M04 PASS under the
declared current-machine functional profile. M05 is next.

## Delivered

GameplayProbe composes existing production owners in one headless server and two
client processes. Real Store enrollment, mTLS/session admission, native physics,
movement prediction, combat presentation and timed replicas connect through
actual loopback messages. This adds acceptance infrastructure; production
services, public protocols and the shipping bootstrap remain unchanged.

Three serial profiles cover baseline and nominal 100/200 ms application-layer
request/reply delay with 1/5% observation loss, seeded jitter, duplicates and
reorder. Each completes 320 native ticks at 20 Hz/three substeps, with shaped
observations through tick 319 and healthy final reconciliation at 320. Reliable
intents and replica traffic retain FIFO/session admission. Combat observations
and their embedded movement values are shaped after verified TLS receipt;
this is not TCP byte-loss or real WAN evidence.

Both clients accept health stages 100/75/50/25/0 and a fresh respawn. Four literal
25-damage hits, one cooldown denial, one zero-reward interaction, the original
60-tick respawn deadline, replica absence/reappearance and final poses/health
agree. Duplicate/stale attacks and an unsupported item-grant request reject
without another shared-sequence admission. Presentation outcomes are unique by
stream/action identity. Wall prediction correction is measured before the
trusted combat-layout teleport, so setup cannot qualify that check.

Client B pauses polling for 750 ms, later closes its actual connection and
reconnects with a fresh session. The other client and native world continue.
Windows records 15 native pause ticks and 23/24/26 detached ticks across profiles;
WSL records 15 and 21/22/23. All runs end with exactly two active character bodies
plus the static wall, two replicas, 100 health each and literal x positions 0/-2.

## Verification

Twenty unique Windows Debug CTests and seven WSL Clang ASan+UBSan CTests PASS.
Windows covers all 19 mapped M04 tests, including the new process case, plus outer
wire rejection. No whole-project matrix was run.

| Selection | Result | Evidence under `out/M04e-work/` |
| --- | --- | --- |
| Windows new connected case: three profiles, nine child processes | 1/1 PASS, 53.79 s | `windows-process-inventory.log`, `windows-process.log`, `windows-process.xml`, `windows-gameplay-process.json` |
| Windows remaining mapped M04 subsystem inventory plus outer wire | 19/19 PASS, 19.12 s | `windows-owners-build.log`, `windows-owners-inventory.log`, `windows-owners.log`, `windows-owners.xml` |
| WSL process, server movement/combat/lifecycles, prediction, combat cache and interpolation | 7/7 PASS, 62.70 s; process case 52.97 s | `wsl-corrected/build.log`, `inventory.log`, `tests.log`, `tests.xml`, `gameplay-process.json` |

Windows observations below are measurements, not new performance limits. Full
reports retain both recipients, configuration and distribution values.

| Profile | A loss / duplicated messages / delivered inversions | A wall correction maximum | A accepted semantic delivery p50 / p95 / max |
| --- | --- | --- | --- |
| Baseline | 0 / 0 / 0 | 0.6 m | 0 / 0 / 0 ms |
| 100 ms, 1% | 8 / 148 / 76 | 1.8 m | 76.26 / 92.39 / 94.25 ms |
| 200 ms, 5% | 14 / 160 / 65 | 2.4 m | 123.76 / 139.94 / 141.35 ms |

Every profile emits all six A outcomes once; B emits none. Both shaped recipients
observe actual loss/duplication/inversions. Prediction stays within eight pending
inputs and replica history within eight frames. Accepted delay excludes stale
rejections and pre-receipt transport time; B's pause contributes to its delay.
General correction distributions include the explicit staging teleport; the
separate pre-staging wall metric qualifies collision. These functional results
retain current-pose/zero-rewind combat; they do not qualify gameplay feel,
LOCAL-MMO-1 capacity or presentation performance.

Windows uses `windows-debug` through `tools/windows_dev.ps1`. Built targets are
GameplayProbe, Tests, ScriptRuntimeTests, RuntimeSpawnTests, CorePhysicsTests,
StorageCharacterTests, StorageRecoveryTests, ServerMovementTests, ReplicationProbe
and Server, with two jobs. Listed CTest inventories select the names above using
`--output-on-failure --no-tests=error` and JUnit. WSL reuses
`/work/build-05a-final-client/linux-asan-ubsan`, two jobs, leak detection and
halt-on-error. Protected Server/Core dependency checks pass. Profiles run serially;
each child has a 60 s deadline and the CTest 240 s. Failure retains original managed
scratch and stops later profiles; success removes only its verified owned root.

Two initial compile failures are retained. MSVC found helper `server` ambiguous
with its namespace (`windows-probe-build.log`); helpers now use `run_server` and
`run_client`. Clang rejected omitted aggregate-member initializers
(`wsl-final/build.log`); explicit empty defaults fix that warning. Final Windows
compilation (`windows-final-compile.log`) and WSL execution pass. The earlier
Windows behavior result remains valid: the only subsequent C++ edit makes the
same empty initialization explicit. No behavior test failed. Before the first
behavior run, source review rejected an invented ideal-surface +0.01 m wall oracle:
pinned native penetration slop is 0.02 m. The fixture instead reuses M04a's x<1.6
bound; no existing tolerance or workload changed.

M04a-d's unchanged malformed/cheat, authority, native partial-failure, Lua lifetime,
history/gap and original durable-operation tests remain applicable; their Debug
owners are included above. M04d's sanitizer script/durable evidence is reused for
unchanged owners. [04c](04c-frame-loop.md) retains actual native iconify/focus
evidence with independent server progress; this run adds separate-process
headless pause/disconnect. No new manual UI or GPU interaction occurred.
Release/package/whole-project reruns were unnecessary for this test-only change;
no dependency, installation, production source or public example changed.

Formatting, whitespace, contract/manifest and repository checks complete the
increment. `.gitignore` covers managed evidence/scratch; README is unchanged.
Independent UI/backlog statuses remain unchanged. M05 retains seamless
content/collision/navigation streaming and authority transfer; M06/M09/M10 retain
graphical, reference-world and workload/endurance qualification.
