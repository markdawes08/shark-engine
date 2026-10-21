# M02e: Integrated network qualification

Dates: 2026-10-20 through 2026-10-21. Assignment: M02e, final checkpoint of REQ-M02.
Base: `901faa654517e344c7344b9ae0d116a7f0cb411d` on initially clean `main`.
Result: uncommitted working tree; the user owns staging, committing and pushing.

**M02e and full REQ-M02 are PASS under the current-machine scope.** All 474 CPU
CTests pass with no failures/skips, alongside protected builds, sanitizer audits
and isolated packages. The [receipt](../coverage/evidence/M02e-integrated-qualification.json)
records exact inputs and results. Follow the
[qualification contract](../network-qualification.md) and
[checkpoint plan](../roadmap/network-plan.md).

## Delivered behavior and contracts

Additive owner-thread C++ statistics report retained replication/interest/cache
counts and vector capacity bytes. The gameplay wire, dependencies, public pose
schema and authority boundaries are unchanged. Six integrated unit cases cover
32-round churn/resync, authenticated reconnect and ACK-deadline recovery, atomic
invalid-state rejection, sequence/epoch/replica boundaries and owner-thread checks.
The separate-process fixture has six scenarios and 21 children: baseline,
100/200 ms nominal RTT with jitter and 1%/5% semantic loss, actual partition and
authenticated reconnect, slow-reader ACK expiry and hostile authenticated
controls. Two recipients independently verify all 32 visible poses. The probe
asserts precise revision/session fences, useful healthy-client updates, privacy,
bounded retained storage and cleanup. Relay bytes remain intact and ordered;
semantic fault replay uses the existing FaultLink on authenticated decoded state.

Changed production files are the public interest/replication headers and their
implementations. New test files are `tests/net_adversity_tests.cpp`,
`tests/net_adversity_probe.cpp` and `tools/test_net_adversity.py`; CMake registers
the two new CPU families. Coverage, contributor/build guidance, the network
contracts and `.gitignore` are maintained. No dependency or wire version changed.

## Verification

Focused Windows Debug build and `shark.core.net-adversity` pass. The final focused
Windows process run passes all six scenarios/21 children in 64.40 seconds, with
277 unchanged source inputs and unchanged probe/harness hashes. WSL GCC13 Server
Debug and Clang18 Server ASan+UBSan each pass both adversity CTests, including the
same six scenarios and 21 children, without sanitizer or cleanup errors.
Formatting passes for 152 C++ files. Contract validation passes its 37 negative
checks, and repository validation passes skill/ignore checks and its self-test.

The first focused Windows process attempt is retained as FAIL. Its slow-reader
pause trigger ran before receipt created a pending ACK, allowing normal immediate
ACK handling to bypass the pause. Moving that trigger after state application
and before ACK submission exercises the unchanged 700 ms pause and 500 ms
deadline. The corrected process runs pass. Review also tightened exact final
revision acknowledgements, immutable old-session replay and useful healthy-peer
progress during active hostile traffic. No engine acceptance limit was relaxed.

| Final gate | Result |
| --- | --- |
| Windows MSVC Debug / Release / ASan | PASS, 76 CTests each |
| WSL GCC CPU client Debug / Clang client ASan+UBSan | PASS, 69 each |
| WSL GCC Server Debug / Clang Server ASan+UBSan | PASS, 54 each |
| Windows Runtime-only / Server-only build, lifecycle and install | PASS |
| WSL Release Server source/link/ELF/license/package closure | PASS |
| SDK-free WSL installed Server | PASS, 11 files and unchanged 92-package inventory |
| Fresh Windows Runtime package and isolated launch | PASS, 33 files and all 10 missing-notice rejections |
| Public/private compile inputs and documented C++ example | PASS |
| Sanitizer compile inputs | PASS, 116 private TLS C units and 16 required network implementation/test units in Windows and both WSL Clang profiles |

All fourteen network CTest families pass in every profile. The integrated
process matrix totals 42 scenarios and 147 owned children: 42 servers, 84 clients
and 21 relays. Every client converges to the exact final source tick, 120 or 320,
and verifies all authorized poses. Earlier transport, credential/session,
replication and interest process fixtures are refreshed in every profile.
All 17 public PEM fixtures, the three pinned dependency archives and dependency
lock bytes are unchanged. Test keys, probes and relay tools remain outside packages.

The final drivers require the same 277 source/build/test/fixture inputs throughout
all seven profiles; their before/after audits report no changed inputs. Test
inventories are checked by name and properties, including
the new 60-second unit and 360-second process deadlines. Native unit JUnit reports
also check the named network cases against their source inventory. Two independent
receipt validators accept the final focused result and reject 60 combined negative
mutations covering absent events, invalid types, exceeded limits and broken
resource/relay accounting. The first failed focused receipt remains separate.
The final aggregate revalidates all raw receipts before retaining compact results,
derived resource summaries, canonical sample hashes and raw receipt paths/hashes.
Complete samples and captured output remain under ignored `out/`. The final audit
reproduces compaction, verifies every retained hash and checks the unchanged base,
empty Git index, whitespace and ignore boundaries.

## Measured workload and bounds

The integrated fixture uses 66 catalog rows and two enrolled recipients with
32 authorized actors each, independent 20 Hz source mutation and publication
capped at 10 Hz. Measured peaks across the seven profiles are within the unchanged
predeclared limits:

| Observation | Measured peak | Fixed limit |
| --- | ---: | ---: |
| Interest/replication retained vector capacity | 34,912 bytes | 262,144 bytes |
| Client replica retained vector capacity | 7,424 bytes | 32,768 bytes |
| Transport application queue | 3,564 bytes / 2 frames | 16,384 bytes / 4 frames |
| Server pending updates / change records | 2 / 64 | 2 / 128 across two streams |
| Client latest change records | 32 | 64 |
| Selection work / policy and privacy work | 793 / 488 units | 8,192 / 4,384 units |
| Semantic pending queue | 5 packets / 17,270 bytes | 16 packets / 65,536 bytes |
| Relay directional buffer | 3,626 bytes | 16,384 bytes |
| Sampled process RSS | 106,782,720 bytes | 1 GiB per process |
| Sampled Windows handles / Linux file descriptors | 122 / 6 | 512 per process |

Capacity accounting covers the declared owned vectors, not total memory. Resource
samples include acknowledged startup and post-cleanup live checkpoints. Some
sanitizer processes retain substantial RSS growth during the short fixture;
passing the sampled ceiling does not establish zero growth or a leak-free soak.

The shaped 1%/5% semantic profiles record one/three actual dropped state messages
respectively, 17-19/16-17 duplicate admissions and 8-10/6-7 delivered reorder
inversions. These are event counts under configured probabilities, not measured
TCP packet-loss percentages. Every partition and slow-reader run reconnects once
and rejects an old-session control; all slow-reader runs cross the explicit ACK
deadline. The unaffected recipient advances 4-5 useful updates during partition,
four during slow-reader failure and 3-4 during hostile traffic. Its full-run
progress is 55-61 updates in normal profiles and 148-161 in shaped profiles,
above the unchanged 30/80 floors.

The refreshed M02d workload separately retains 128 candidates, 32 poses per
recipient and 20 measured seconds after warm-up. Across fourteen recipient/profile
pairs, clients apply 199-200 measured updates; socket downstream p95 is
34.40-34.53 KiB/s and upstream p95 is about 1.30 KiB/s, within 64/16 KiB/s limits.
Join traffic peaks at 4,707/1,252 downstream/upstream bytes. The FIFO delay fixture
records 547.62-574.84 ms admission-to-ACK completion within its unchanged
300-3,000 ms gate. Selection-call wall p95 is 0.25-5.74 ms; elapsed wall-time
measurements are informational and do not qualify a simulation CPU budget.

## Commands and retained evidence

Exact retained qualification entry points, run from the repository root:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M02e-work/verify-windows.py
wsl -d Ubuntu -u root --exec bash /mnt/c/Users/mrkda/Projects/shark/out/M02e-work/wsl-run.sh
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M02e-work/verify-runtime-package.py
.tools/venv/Scripts/python.exe out/M02e-work/api-audit.py
.tools/venv/Scripts/python.exe out/M02e-work/audit-sanitizers.py
.tools/venv/Scripts/python.exe out/M02e-work/wsl-runtime-audit.py
.tools/venv/Scripts/python.exe out/M02e-work/wsl-receipt.py
.tools/venv/Scripts/python.exe out/M02e-work/measurement-summary.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M02e-work/compile_documented_example.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M02e-work/final-checks.py
.tools/venv/Scripts/python.exe out/M02e-work/record-evidence.py
.tools/venv/Scripts/python.exe out/M02e-work/final-audit.py
```

The ignored drivers retain exact nested commands, toolchains, logs, source hashes,
raw process measurements and cleanup results. Ordinary registered fixtures and
[build instructions](../building.md) do not depend on these helpers or the ignored
local roadmap. Reproduce the focused tests after configuring and building the
matching preset:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug --show-only
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R ^shark.core.net-adversity --output-on-failure
```

## Limits and next assignment

Qualification uses this Intel Core i9-14900HX Windows 11 PC (build 26200),
MSVC 19.51.36248/Windows SDK 10.0.28000.0, and its existing WSL2 Ubuntu CPU
userspaces with GCC 13 and Clang 18. CMake 3.31.10, Ninja 1.13.2 and
clang-format 20.1.8 remain pinned. Exact compiler, Python and kernel versions
are retained per profile. Native Vulkan/GPU behavior is unchanged and was not
rerun for this CPU statistics and network-fixture increment.

No new hardware, hosted runner, public certificate or external account is needed.
No gameplay editor, authoring command, durable store or production capacity is
claimed. FeatureWorld and MMOWorld remain assigned to 10 and M09. The workload
uses two enrolled recipients, not the full 16-player/two-region LOCAL-MMO-1
scenario. Tests cover defined recovery and sampled resource ceilings, not WAN
packet loss, long-running leak freedom or production memory budgets. Windows
development tools remain installed; the SDK-free WSL runtime shares this PC's
kernel. Bootstrap executables still expose no public player listener. Persistence,
combat/prediction and seamless region transfer retain M03, M04 and M05.

Exact next assignment: **08 - Project and scene persistence**. This checkpoint
does not implement that assignment or advance live durable state out of M03.
