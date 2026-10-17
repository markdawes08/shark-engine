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

Follow [MMORPG authority and identity](../../../docs/architecture/0007-mmorpg-architecture.md):
classify state as server-authoritative, predicted, replicated or presentation-only.
Player requests are bounded authenticated intents, not authoring transactions or
arbitrary component setters. Separate durable identity from local/replica handles;
test stale ownership epochs, reconnect, region handoff and duplicate durable actions
when those layers exist. Render-frame Lua must not decide live gameplay outcomes.

For an implemented surface, trace valid and invalid behavior through engine
validation, serialization/versioning, editor fields, typed commands, selected
Lua bindings and cooked export as applicable. Document intentional unsupported
operations. Every exposed API gets a stable manifest entry linked to real tests
and a FeatureWorld exercise or justified external fixture.

Test stale handles, missing components, duplicate IDs, rollback, callback ordering,
scene reload and deferred create/destroy where relevant. The local editor/client
controlling thread commits its scene changes; each authoritative server region
uses its designated single writer. Physics workers queue events and never call Lua.
Bindings use checked handles instead of borrowed component pointers. Preserve
edit/play isolation and the authored scene through failed operations.

Use the native preset and actual existing CTest cases from
[building](../../../docs/building.md), then run
`python tools/validate_contract.py`. Add meaningful feature tests in the owning
increment; the contract validator alone does not prove API behavior. Update
migration/rejection rules, coverage and handoff together when the public contract
changes, and follow AGENTS.md's user-owned commit policy.
