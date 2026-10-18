# Requirement and API coverage

[requirements.json](requirements.json) is the version-3 requirement/coverage manifest for
Shark. Each requirement has a unique ID, one owning roadmap assignment, observable
acceptance criteria, a planned FeatureWorld case or external fixture, and an
honest verification state. Related assignments can contribute without changing
which assignment owns the gate.

The user selected a seamless open-world MMORPG. The
[versioned execution plan](../roadmap/mmorpg-plan.md) and
[ADR 0007](../architecture/0007-mmorpg-architecture.md) expand the original 00-35
assignments with M01-M10. There are 49 requirements across 46 ordered assignments.
`assignment_order` records dependency order; M01 follows 01 and precedes 02, and
the validator checks order rather than comparing IDs lexically. The product
profile and plan path are explicit machine-readable fields.

`planned_fixture` describes future evidence, not an existing test. `NOT_RUN`
means precisely that; documentation validation cannot pass a future engine gate.
The `implemented_surface.features` entries map the bootstrap behavior and runtime
build boundary, server lifecycle and Server/Core closure to their actual tests
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
revision. The current assignment is M01; its implementation and pending hosted
qualification are recorded in the [M01 handoff](../handoffs/M01-headless-server.md).
Its [local receipt](evidence/M01-local.json) records Windows 45/45 CTests,
Ubuntu 26/26 restored CTests, the expected injected sentinel failure and package
closure/lifecycle evidence. The hosted gate remains open.
Graphics and multiplayer requirements remain NOT_RUN. See the
[CI verification handoff](../handoffs/01-ci-verification.md) for 01's tested
revision, run URLs and qualification limits.
