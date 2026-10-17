# Assignment 00 handoff

Date: 2026-10-17. Assignment: **00 — Engine name, release scope and architecture
contract**. Engine working name: **Shark**, explicitly selected by the user.

## Starting and resulting state

The workspace initially contained only `AI_Game_Engine_Agent_Roadmap.txt`.
`git status --short` reported that this directory and its parents are not a Git
repository. There is no starting branch, base commit or result commit. No AGENTS.md,
architecture record or prior handoff existed in the workspace or checked ancestor
directories. Do not invent any such baseline. No Git repository was initialized;
repository/bootstrap work belongs to 01. This increment is a reviewable set of
new files alongside the preserved roadmap.

Original roadmap SHA-256:
`a4a80adf31e90e26da78d966f95e44624dbd84e9877e7554bc95ebf28d63e80c`.

## Delivered behavior and contracts

Six [architecture decisions](../architecture/README.md) record target boundaries,
library choices and pinning policy, candidate platforms/graphics evidence,
source/cache/cooked identity and compatibility, math/color/timing/mutation rules,
and the full v1 profile. These are design contracts; engine code does not exist.

The [requirements manifest](../coverage/requirements.json) covers every assignment
00–35 plus recurring development/testing/runtime-boundary obligations. Each entry
has an owner, observable acceptance criteria, planned fixture and verification
status. Feature/component/command/Lua API arrays are empty because no API has been
implemented. FeatureWorld begins in 10. The documentation validator checks this
planning structure and local links, with negative tests for omissions and false
implementation claims.

New files: root README; six ADRs and architecture index under `docs/architecture/`;
coverage README and JSON manifest under `docs/coverage/`; this handoff under
`docs/handoffs/`; `tools/validate_contract.py`. No source scene schema or public API
is implemented or migrated. The initial schema/version and future migration or
rejection policies are recorded in ADR 0004.

## Verification

Host observed: Microsoft Windows 11 Home, OS version `10.0.26200`, 64-bit;
PowerShell; Python `3.13.5`. The legacy `WindowsProductName` field reports Windows
10 Home; `OsName`/`OsVersion` above are the observed operating-system evidence.
No GPU model, driver, audio device or target Mac was qualified.

| Exact command/check | Result |
| --- | --- |
| `git status --short` | Baseline observation: exit 1, not a Git repository; no revision exists |
| `Get-FileHash -LiteralPath AI_Game_Engine_Agent_Roadmap.txt -Algorithm SHA256` | PASS: original roadmap digest recorded above and preserved |
| `python tools/validate_contract.py` | PASS, exit 0: 39 requirements cover 00–35; document links resolve |
| `python tools/validate_contract.py --self-test` | PASS, exit 0: the same contract check plus 14 negative checks for omissions, malformed data and false claims |
| Read all roadmap requirements against ADRs and requirement rows; independent consistency review | PASS: all mandatory requirements have owners and observable criteria; corrected front-face winding, graphics pin ownership, audio decoders, main-thread ownership and shadow binding budget |
| CMake/build/CTest, sanitizer, graphics, audio and exported applications | NOT RUN: not implemented in assignment 00 |
| Linux/macOS/Windows engine qualification and all nine real-GPU rows | NOT RUN: no engine or probe exists; hardware gates belong to 02 and later |

Documentation checks do not prove mathematical implementation, feature behavior
or compatibility. Future test results remain NOT_RUN. No missing platform test
has been treated as PASS.

## Limitations and next task

Assignment 00's architecture-contract gate is PASS, with no remaining blocker to
this increment. NVRHI/macOS feasibility, tested immutable dependency
combinations, actual build presets, code, GPU/image/audio baselines, performance
budgets and packaged release evidence are future gates with explicit owners.
The graphics route on Apple Silicon and Intel Macs may fail; assignment 02 must
produce a reproducer and decision request rather than silently replace NVRHI or
remove a platform. Detailed dependency locks are added before each dependency's
first integration through the mechanism created in 01.

**Next task: implement assignment 01 — Repository, build system and test
foundation**, reading the roadmap, ADRs and this handoff first. Establish the
repository and Engine/EngineTools/Editor/Runtime/Tests targets, real starter test,
CMake presets/locked inputs/warnings/formatting/CTest/sanitizers, CPU CI for the
four OS/CPU rows, AGENTS.md with actual commands and policies, and discoverable
versioned skills for build/test, renderer validation, component/API integration,
imports and release packaging. Prove that a deliberately failing test fails CI
before removing it, and prove Runtime's link boundary. Record missing runners as
NOT RUN. Do not advance beyond that gate or begin production renderer work.

Future handoffs retain: assignment and base/result revision; behavior delivered;
files/public contracts changed; exact commands/results; verified platforms/hardware;
checks not run/reasons; FeatureWorld/manifest changes; blockers/limitations; exact
next assignment.
