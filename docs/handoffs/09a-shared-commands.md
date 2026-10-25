# 09a: Shared command foundation

Date: 2026-10-25. **09a is PASS under the current-machine scope.**

Assignment: 09a, the first of [five authoring checkpoints](../roadmap/authoring-plan.md).
Base: `0b2726c1909457bdd6fbbe1f74289f7d3c987494` (`0b2726c`) on initially clean `main`.
Result: uncommitted working tree. The user owns staging, commits and pushing.
Full `REQ-09` remains **NOT_RUN**; 09b-09e retain their assigned gates.
Final [qualification receipt](../coverage/evidence/09a-shared-commands.json): **PASS**.

## Behavior delivered

The existing EngineTools `AuthoredScene` is the shared editing owner. Five typed
commands create an entity, delete a subtree, set Name, set Transform and reparent
with keep-local/keep-world semantics. Targets and parents use stable authored IDs;
every execution requires the expected Scene instance and revision. Core still
owns validation, hierarchy rules and safe immediate mutation. No second registry,
mutable document model, dependency or gameplay endpoint was introduced.

Successful changes advance the existing revision once; true no-ops do not. Rejected
commands preserve state, source metadata and the dirty baseline. Ordinary edits
retain Scene incarnation and surviving handles. Creation/recreation cannot revive
metadata from a deleted incarnation. Explicit activation and file publication keep
their existing replacement, conflict and clean-baseline semantics.

`Result<AuthoringCommandReply>` has two result levels. The outer error reports
owner access or inability to produce a reply before mutation; the inner `outcome`
reports command acceptance/rejection. Every reply carries the current observed
revision. Successful effects contain a sorted bounded refresh-ID set, including
deleted descendants and affected parent links. Receipt storage is prepared before
mutation; successful publication is followed only by nonallocating observations
and moves. Query/continuation/inspection wrappers reuse Core's bounded owned pages.

## Public compatibility and changed files

[ADR 0020](../architecture/0020-shared-authoring-commands.md) records the intentional
C++ source change. Public `AuthoredScene::scene()` now returns only a const Scene
borrow. Replace direct setters with `execute(typed_command, observed_revision)`
and check both Result levels. Read-only consumers continue to compile. The
[command contract](../authoring-commands.md) and updated
[capture/activation example](../authored-scenes.md) document the migration.
Source JSON/component versions remain 1; no wire or database version changes.

- `engine_tools/include/shark/tools/authored_commands.hpp`, `authored_scene.hpp`
  and `engine_tools/src/authored_scene.cpp`: typed values, owner service and queries.
- Private `engine_tools/src/authored_scene_test_access.hpp`: owner-checked access
  only for retained pending/visit/closed-state and Core scope invariant fixtures.
- `tests/authoring_command_tests.cpp` and `tests/CMakeLists.txt`: new CPU family;
  four persistence test/process sources migrate ordinary edits to commands while
  retaining their original cases and failure assertions.
- ADR/contract/checkpoint plan, requirements/component/command mappings, current
  guidance and repository skills describe the delivered service and later owners.
  `.gitignore` retains generated evidence under `out/` and the ignored local roadmap.

## Qualification evidence

Frozen inputs: **345 source/build/test/fixture/contract files and 17 helpers**.
`out/09a-work/source-freeze.json` SHA-256:
`0326ae11e65b608480aac4a2bd49d3eecb2dfe6b011e52cdf11b55db5c5ad82f`.
WSL unchanged-input verification: **PASS**, including before/after the closure
report recovery below. Final overall unchanged-input verification: **PASS**.

| Required final run | CTests | Result |
| --- | ---: | --- |
| Windows Debug | 95 | PASS |
| Windows Release | 95 | PASS |
| Windows ASan | 95 | PASS |
| WSL CPU Debug, GCC 13 | 88 | PASS |
| WSL CPU ASan+UBSan, Clang 18 | 88 | PASS |
| Standalone Windows Tools | 6 | PASS, separately counted |
| Standalone WSL Tools | 6 | PASS, separately counted |

Total: **461 full CPU CTests plus 12 standalone Tools checks**, with no failures
or skips. Every full profile also passes the five named Catch groups and all
12 process receipts. Both standalone Tools profiles retain the exact 60-test
inventory and pass their six selected authoring families and five named groups.
Server-only full CTest suites are not rerun by 09a; historical results are excluded.

The final WSL receipt, `out/09a-work/wsl-verification.json`, is **PASS**. Both full
CPU profiles also pass all five named Catch groups and their actual process
receipts. The four public examples compile with GCC 13 and Clang 18; ten actual
authoring objects contain the required ASan+UBSan instrumentation. Protected
Server and Runtime-only builds, Server dependency closure, the 12-file installed
Server package and its SDK-free smoke in the 92-package runtime environment pass.
Native gates also pass: the four public examples compile with MSVC, ten actual
authoring objects contain ASan instrumentation, and all six native compile
databases preserve the Tools/private-JSON boundary. Windows Runtime-only and
Server-only build and smoke checks pass, including the 12-file Server install.
The 33-payload-file Runtime package launches from a Unicode/space path outside
the checkout with a system-only PATH; ten missing-license controls reject.
Format (192 C++ files), contract (37 negative controls), repository/skill/ignore,
whitespace and empty-index checks pass.

Completed preflight: native `native-r3` passes 12 focused CTests; WSL `wsl-r2`
passes 13 focused CTests in each CPU profile and six standalone Tools checks.
The five named Catch groups pass: source 28 cases/10,604 assertions; authored
23/30,783; commands 24/14,635; files 31/2,054 Windows or 2,052 WSL; persistence
3/1,596. Native inventory preparation and four MSVC examples pass; WSL GCC/Clang
examples pass. Preflight counts do not contribute to final totals.

Retained failed preparatory attempts: `native-r1` failed MSVC `/WX` because a new
test helper returned after Catch's terminating `FAIL`; the unreachable return
was removed. `native-inventories-r1` encountered an absent CTest command field
before the new executable existed; inventory preparation now builds that target.
`native-r2` and `wsl-r1` passed focused CTests but the stricter evidence parser
rejected existing empty/single-space dynamic sections with identical rendered
names. The parser now permits exactly those two rows and rejects missing/extra
copies. Current 22 parser controls and seven aggregation phase controls pass.
These attempts remain FAIL/preparatory evidence, never final PASS rows.

The first final WSL receipt-consumer invocation failed with `KeyError: 'status'`
after all WSL execution phases had passed. The closure phase wrapper had written
successful phase metadata to `server-only/closure.json`, replacing the detailed
report produced at that same path by `tools/check_server_closure.py`. This was an
evidence filename collision; the original validator command exited zero and its
log records PASS. The overwritten phase record is preserved separately as
`out/09a-work/wsl-final/server-only/closure-phase-original.json`; the original
closure log and overall execution summary remain byte-for-byte unchanged.

A separate recovery reran the exact frozen closure validator against the same
unchanged Server build and package in the retained userspace, with read-only
source and writable evidence mounts. Recovery command metadata and logs use
`wsl-final/closure-followup/closure-recovery.*`, leaving the validator's actual
report at `wsl-final/server-only/closure.json`. The recovery's `before.json` and
`summary.json` bind the original failure, command, controller/wrapper hashes and
before/after checks: all frozen inputs and all 12 package file hashes stayed
unchanged. The unchanged `wsl-receipt.py` then passed. No source, frozen helper,
acceptance condition or suite changed; no full test suite was rerun for recovery.

Exact workflow commands, run from the repository root, all pass. The explicit recovery
invocation is a separately recorded post-run artifact repair, not a frozen helper
amendment:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/09a-work/verify-windows.py final
.tools/venv/Scripts/python.exe out/09a-work/launch-wsl.py final
.tools/venv/Scripts/python.exe out/09a-work/wsl-final/closure-followup/recover-closure.py
.tools/venv/Scripts/python.exe out/09a-work/wsl-receipt.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/09a-work/public-api.py --compiler cl --build build/windows-debug --output out/09a-work/public-api-msvc --final
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/09a-work/native-audit.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/09a-work/verify-runtime-package.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/09a-work/repository-checks.py --output out/09a-work/preclose-checks
.tools/venv/Scripts/python.exe out/09a-work/final-receipt.py --final-checks out/09a-work/preclose-checks/summary.json
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/09a-work/repository-checks.py
.tools/venv/Scripts/python.exe out/09a-work/final-receipt.py
```

The native driver records configure/build/inventory/CTest commands, exact archives,
build `--parallel 2`, explicit seeds, phase exits and cleanup outcomes. WSL uses the
retained pinned userspaces with read-only source and writable evidence. The receipt
binds those logs, public examples, dependency/object audits and package evidence.

## Scope, coverage and next checkpoint

Hardware recorded in `out/09a-work/hardware.json`: Lenovo 83DG; Intel Core
i9-14900HX, 24 cores/32 logical processors; 34,053,414,912 bytes physical memory;
Windows 11 Home 10.0.26200, build 26200. Linux checks use this PC's existing WSL2
Ubuntu environment. No machine resource settings changed and no GPU gate is added.

Five command entries plus `tools.authoring-owner` and Name/Transform coverage map
to real CPU fixtures. FeatureWorld/AuthoringCommands attaches in 10. Human/agent
equivalents share the C++ service; CLI/process ownership remains 15 and editor
integration 16. There is no batch/preview, undo/redo, retained mutation identity,
lost-reply lookup, async job API, MMO simulation or capacity claim in 09a.

No blocker remains for the next checkpoint.
**Exact next checkpoint: 09b — Atomic transactions and preview.**
Suggested commit: `feat(authoring): add shared revision-checked commands`.
