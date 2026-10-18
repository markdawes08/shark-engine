# Requirement and API coverage

[requirements.json](requirements.json) is the version-3 requirement/coverage manifest for
Shark. Each requirement has a unique ID, one owning roadmap assignment, observable
acceptance criteria, a planned FeatureWorld case or external fixture, and an
honest verification state. Related assignments can contribute without changing
which assignment owns the gate.

Assignment 03 is split into [03a/03b/03c](../roadmap/core-plan.md). The implemented
Core diagnostics/configuration and identity/path features map to real unit and
process fixtures. The [03a handoff](../handoffs/03a-core-contracts.md) records their
local results. The [03c handoff](../handoffs/03c-time-and-work.md) closes full
`REQ-03` with final local Windows/WSL regressions, separate clock policies,
bounded completions and callback/thread rules.

The 03b [application lifecycle](../application-lifecycle.md) feature adds
`shark.core.lifecycle` and extends the real Server process path. Its
[handoff](../handoffs/03b-application-lifecycle.md) records native/injected signal
ownership, partial-start rollback, repeated cycles and cleanup-failure evidence.
The [03c timing/work](../time-and-work.md) features map to `shark.core.timing`
and `shark.core.work`, using controlled clocks and real thread fixtures. No world
simulation or production worker is inferred from those foundation tests.

Assignment 04 follows the [window/input checkpoints](../roadmap/window-input-plan.md).
The `client.raw-input` feature maps 04a's device-free state to `shark.client.input`
in client-enabled builds; headless Server builds exclude it. The
[04a handoff](../handoffs/04a-input-contracts.md) preserves its CPU evidence.
04b's `client.native-window` feature adds owned GLFW lifecycle, CPU backend/CLI
fixtures and explicit Windows smoke evidence in the
[04b handoff](../handoffs/04b-native-window.md). The `client.offline-frame-loop`
feature maps 04c's frame/tick input, pause/step and shared sample pump to
`shark.client.frame`, window fixtures and Core timing tests. The
[04c handoff](../handoffs/04c-frame-loop.md) records its final qualification.
Full `REQ-04` is PASS under the current-machine gate: all three checkpoints are
complete, with final evidence in [04c-local.json](evidence/04c-local.json).

The user selected a seamless open-world MMORPG. The
[versioned execution plan](../roadmap/mmorpg-plan.md) and
[ADR 0007](../architecture/0007-mmorpg-architecture.md) expand the original 00-35
assignments with M01-M10. There are 50 requirements across 46 ordered assignments.
`assignment_order` records dependency order; M01 follows 01 and precedes 02, and
the validator checks order rather than comparing IDs lexically. The product
profile and plan path are explicit machine-readable fields.

[ADR 0008](../architecture/0008-human-agent-authoring.md) and the
[human/terminal-agent plan](../roadmap/human-agent-plan.md) strengthen existing
owners and add `REQ-CROSS-HUMAN-AGENT` (audit owner 32). Shared services, terminal
discovery/results, one project owner and semantic editor/CLI parity are required
as their layers land; 33/M09 add fresh terminal and human continuation evidence.
This adds no assignment or implemented API and does not change schema 3.
Historical PASS records are retained; documentation checks do not qualify the new
authoring gates. The later [ADR 0009](../architecture/0009-windows-linux-platform-scope.md)
removes Mac requirements and retires 02c. The user then limited the project to
resources already available on this PC in
[ADR 0010](../architecture/0010-current-machine-scope.md). Active targets are the
native Windows client/editor on its RTX 4070 Laptop and Intel Raptor Lake-S Mobile
GPUs, plus CPU/headless-server and isolated package checks in the existing WSL2
Ubuntu environment. Native Linux graphics, other hardware and independent clean
physical machines are outside the current gate. Their historical NOT_RUN records
remain unchanged.

`qualification_profile: current-machine` and `scope_document` record that scope
as additive schema-3 fields. REQ-02 is PASS for the amended local gate, with
explicit scope and SHA-256-pinned references to the unchanged 02d receipts.
The validator verifies those references and retained bytes; it does not rerun
graphics or interpret an old aggregate NOT_RUN as a universal PASS.
`current_assignment: 04` records the completed window/input assignment. Assignment
05, production graphics device and presentation lifecycle, is next. All later
implementation requirements remain NOT_RUN.

`planned_fixture` describes future evidence, not an existing test. `NOT_RUN`
means precisely that; documentation validation cannot pass a future engine gate.
The `implemented_surface.features` entries map the bootstrap behavior and runtime
build boundary, server lifecycle, Server/Core closure and graphics setup/executable cases to their actual tests
and external fixtures. Component, authoring
command and Lua arrays remain empty because those APIs do not exist yet.
Assignment 10 connects supported scene surfaces to executable FeatureWorld
coverage; 32 audits completeness. Do not register placeholder APIs.

An implemented entry identifies its stable name, owning requirement IDs, actual
automated test IDs, test source paths and FeatureWorld or justified external
fixture paths. Coverage registration is not proof of correct behavior;
those tests must run with recorded platform and PASS/FAIL/NOT_RUN evidence.

Run from the workspace root:

```powershell
python tools/validate_contract.py
python tools/validate_contract.py --self-test
```

The check rejects malformed/duplicate IDs, missing owners or criteria, missing
assignment coverage, unsupported verification claims, missing ADRs, and broken
relative Markdown document links. Its negative cases confirm that omissions
actually fail. It does not parse natural language to prove all requirements are
present; the handoff records a separate review against the source roadmap.
Local-scope negative cases reject absent/altered scope and evidence references,
and retain rejection of future PASS claims after assignment 02 closes.

Schema 2 added `current_assignment` and structured implemented-surface entries to
schema 1. Schema 3 adds the MMORPG product/plan fields and ordered expanded owner
set. This planning manifest was migrated directly; the checker explicitly rejects
earlier/future versions, missing MMO owners, invalid ordering and unsupported
future PASS claims. No project/scene/runtime/wire/database data format has been
implemented or migrated by this amendment. M01 adds implemented server lifecycle
and dependency-isolation features; registration alone does not pass their gate.

The ignored local roadmap is amended for consistency but remains optional context,
not a CI input. REQ-00's PASS records its historical planning evidence, not MMO
implementation. REQ-01 is PASS: the repaired hosted CPU matrix and required
deliberate-failure/red-to-green proof are verified at the recorded historical
revision. REQ-M01 is PASS. Its
implementation and local qualification are recorded in the
[M01 handoff](../handoffs/M01-headless-server.md). The
[local receipt](evidence/M01-local.json) records Windows 45/45 CTests,
Ubuntu 26/26 restored CTests, the expected injected sentinel failure and package
closure/lifecycle evidence. The [hosted follow-up](../handoffs/M01-ci-verification.md)
and [gate receipt](evidence/M01-ci-gate.json) record the historical fifteen jobs passing,
206/206 restored CTests, the expected thirteen injected sentinel failures,
Docker/package closure and the then-required fifteen checks. The current Windows/Linux
CI policy is documented in [CI](../ci.md). Existing hosted jobs and branch
protection remain unchanged; hosted runs are supplemental to local roadmap gates.
Assignment 02 delivered setup, executable GPU/validation fixtures and probe packages.
Native Mac 02c is retired by ADR 0009; the earlier
[deferral](../handoffs/02c-mac-deferred.md) remains a historical record.
The [02b handoff](../handoffs/02b-graphics-cases.md) separates local evidence from
the hardware matrix required at that time. The [graphics checkpoint plan](../roadmap/graphics-plan.md)
and [probe guide](../graphics-probe.md) distinguish setup diagnostics from the
current local hardware gate.
The [Windows 02d handoff](../handoffs/02d-windows-validation.md) and
[receipt](evidence/02d-windows-local.json) record the portable package, supplemental
NVIDIA full soak, Intel short cases and retained failures at their recorded input
hashes. Those receipts retain their original nine-row scope. The later six-row
Windows/Linux matrix and independent clean-machine results also remain NOT_RUN
in their original evidence; ADR 0010 removes those unavailable targets from the
current plan. The
[final local handoff](../handoffs/02d-final-verification.md) records the current
package/link changes, refreshed Windows evidence and isolated Ubuntu
CPU/package checks. The [hosted audit](evidence/02d-hosted-ci.json) verifies nine
jobs and 156 CTests at `39110d4`; it does not cover the subsequent 02d changes.
The [scope amendment](../handoffs/current-machine-scope-amendment.md) reassesses
the [Windows refresh](evidence/02d-windows-refresh.json) and
[Linux local](evidence/02d-linux-local.json) receipts at `2ef281b`. Both actual
Windows GPUs passed the complete 100 resize / 20 device / 600-second lifecycle
and readback/validation cases; local package isolation and failure checks passed.
This closes REQ-02 only for LOCAL-W-NV and LOCAL-W-INTEL plus the stated local
package/CPU evidence. It is not a new run, production renderer or general Windows
support claim. Future renderer work revalidates those devices as features land.

Future packaging, release and MMORPG gates use this PC, separate real local
processes, the actual local store, shaped network faults and existing WSL
userspaces. ADR 0007's provisional LOCAL-MMO-1 profile uses 16 active synthetic
players, 16 NPCs, two region processes, 32 visible animated actors, 100 crossings
and a two-hour active soak. The two real graphical-client semantic test runs
separately from synthetic load. These are planned tests, not achieved capacity;
external hosting, multihost failover, WAN testing, paid services and public signing
credentials are not prerequisites. Human/terminal parity and seamless authoritative
world behavior remain required. Multiplayer requirements remain NOT_RUN. See the
[CI verification handoff](../handoffs/01-ci-verification.md) for 01's tested
revision, run URLs and qualification limits.
