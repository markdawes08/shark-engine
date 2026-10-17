# Requirement and API coverage

[requirements.json](requirements.json) is the version-2 requirement/coverage manifest for
Shark. Each requirement has a unique ID, one owning roadmap assignment, observable
acceptance criteria, a planned FeatureWorld case or external fixture, and an
honest verification state. Related assignments can contribute without changing
which assignment owns the gate.

`planned_fixture` describes future evidence, not an existing test. `NOT_RUN`
means precisely that; documentation validation cannot pass a future engine gate.
The `implemented_surface.features` entries map the bootstrap behavior and runtime
build boundary to their actual tests and external fixtures. Component, authoring
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

Schema 2 adds `current_assignment` and structured implemented-surface entries to
schema 1. This repository's planning manifest was migrated directly; the checker
explicitly rejects earlier/future schema versions. No project/scene/runtime format
changed. The ignored original roadmap is optional local context; it is not a CI
input. REQ-01 remains NOT_RUN while its full cross-platform/hosted gate is open,
even when the handoff records passing local checks.
