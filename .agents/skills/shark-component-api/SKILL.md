---
name: shark-component-api
description: Add or change Shark components and public command or Lua APIs while connecting validation, persistence, authoring, lifetime tests and FeatureWorld coverage.
---

# Shark component and API integration

Read [AGENTS.md](../../../AGENTS.md),
[identity/version rules](../../../docs/architecture/0004-formats-and-identity.md),
[mutation phases](../../../docs/architecture/0005-simulation-and-rendering.md) and
[coverage policy](../../../docs/coverage/README.md). Extend the authoritative
engine schema/services rather than adding a parallel UI or script scene model.

Assignment 01 has no ECS, component schema, command service, Lua VM or
FeatureWorld. They arrive in 07, 09, 19 and 10 respectively. Do not fabricate
bindings or schema fixtures to fill the empty component, command and Lua entries
in the implemented-surface manifest. Its foundation features already map the
bootstrap behavior and runtime build boundary to tests.
Connect an earlier component when its missing layer is introduced.

For an implemented surface, trace valid and invalid behavior through engine
validation, serialization/versioning, editor fields, typed commands, selected
Lua bindings and cooked export as applicable. Document intentional unsupported
operations. Every exposed API gets a stable manifest entry linked to real tests
and a FeatureWorld exercise or justified external fixture.

Test stale handles, missing components, duplicate IDs, rollback, callback ordering,
scene reload and deferred create/destroy where relevant. Only the process main
thread commits scene changes; physics workers queue events and never call Lua.
Bindings use checked handles instead of borrowed component pointers. Preserve
edit/play isolation and the authored scene through failed operations.

Use the native preset and actual existing CTest cases from
[building](../../../docs/building.md), then run
`python tools/validate_contract.py`. Add meaningful feature tests in the owning
increment; the contract validator alone does not prove API behavior. Update
migration/rejection rules, coverage and handoff together when the public contract
changes, and follow AGENTS.md's user-owned commit policy.
