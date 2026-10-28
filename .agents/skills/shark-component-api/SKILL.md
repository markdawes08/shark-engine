---
name: shark-component-api
description: Add or change Shark components and public command or Lua APIs while connecting validation, persistence, authoring, lifetime tests and FeatureWorld coverage.
---

# Shark component and API integration

Apply the [testing policy](../../../docs/testing-policy.md). Read [AGENTS.md](../../../AGENTS.md)
and the assigned increment; use the [coverage guide](../../../docs/coverage/README.md)
for current mappings and historical handoffs, rather than copied case counts.

Before design, read [identity/version rules](../../../docs/architecture/0004-formats-and-identity.md),
[mutation phases](../../../docs/architecture/0005-simulation-and-rendering.md) and
[human/agent authoring](../../../docs/architecture/0008-human-agent-authoring.md).
Extend authoritative engine services, not a parallel UI/script scene model.

## Schema, scene and lifetime

Follow [scene ownership](../../../docs/scene-contracts.md),
[transforms](../../../docs/transforms.md) and [inspection](../../../docs/scene-inspection.md).
Name/Transform are mandatory version-1 components. Keep EnTT private; include math
through `shark/math.hpp` and SharkMath. Local world matrices and global cell
coordinates have distinct roles. Do not expose native entities or borrowed
component pointers across mutations.

Extend engine-owned schema IDs, versions, typed defaults/ranges/units/mutability
and validators together. Unknown IDs/versions/value types reject. Caller-edited
metadata never becomes validation policy. Pure validation neither reserves a
revision nor proves hierarchy feasibility; queries return bounded owned pages
with explicit revision/consistency checks. Preserve xyzw quaternion metadata and
signed scale's magnitude floor.

Reuse the scene owner/deferred queue. Keep-local preserves local TRS; keep-world
rejects nonrepresentable TRS before changing links or descendant matrices. Delete
removes a subtree atomically. Changes advance one revision; no-ops do not. Exercise
relevant stale/foreign/cyclic handles, authority mismatch, depth/scale failure,
rollback and deferred ordering without reducing earlier workloads.

## Authoring and persistence

Use [shared commands](../../../docs/authoring-commands.md),
[batches](../../../docs/authoring-transactions.md), [history](../../../docs/authoring-history.md)
and [retries](../../../docs/authoring-retries.md) on the same AuthoredScene owner.
Its public scene borrow is read-only. Ordinary edits preserve incarnation, live
handles, metadata and saved baseline; activation is not a command shortcut.
Never expose candidate handles or private invariant-fixture mutation ports.

Batches validate every intermediate state and allocate complete replies before
one publication. Preview publishes nothing and is not a commit token. Changed
edits/batches form one history group; preview/no-op/rejected work preserves stacks.
Undo/redo require the current revision and next ticket. Save retains history;
activation clears it; recreated entities receive fresh generations. Extend Core
candidate copying and metadata/history capture/restoration when components grow.

Retained mutations check outer access, reply outcome and original operation Result.
Keep scope/consecutive sequence/watermark semantics, bounded retention and exact
input comparison/charges. Foreign/expired identities and out-of-band writes need
explicit reconciliation, never blind replay. New command fields extend matching,
serialization and retained-memory charges together.

Follow [source documents](../../../docs/source-documents.md),
[activation](../../../docs/authored-scenes.md) and [project files](../../../docs/project-files.md)
for persisted fields. Extend capture and reconstruction together, retaining dirty,
revision, pending/visit/thread and incarnation-bound metadata checks. Native files
use root/path content revisions and explicit create-only writes. A committed
post-publication durability limitation must not appear as rollback. Extend actual
save/reopen/conflict cases when persisted behavior changes.

## Authority and adapters

Under the [MMORPG contract](../../../docs/architecture/0007-mmorpg-architecture.md),
classify state as authoritative, predicted, replicated or presentation-only.
Players submit bounded authenticated intents, not arbitrary component setters or
authoring transactions. Keep durable identity, ownership epochs, local handles
and replica/session identities distinct. Client/render Lua cannot grant outcomes.

When touching networking, read [sessions](../../../docs/network-sessions.md),
[replication](../../../docs/network-replication.md) and [interest](../../../docs/network-interest.md).
Only actual ready server-side mTLS establishes admission, not copied identity.
Only explicitly selected public fields replicate. Preserve owner/session/stream
fences, privacy maintenance, bounded resources and nonmutating rejection. Logical
retained-capacity statistics are not wire fields or total process memory.

Human and terminal adapters use identical validated services, schema discovery,
revision conflicts, retries and history. CPU operations need no editor/window/GPU;
preview/capture exposes separate capabilities. Keep structured diagnostics,
request/job IDs, cancellation and truthful artifact/commit outcomes. Workers
return bounded values and observed revisions to the owner, never mutable state.
Bindings use checked handles and retain edit/play isolation.

## Coverage and verification

For a new supported surface, connect validation, persistence/versioning, typed
commands, editor fields, Lua and export where those layers exist. Document current
unsupported operations; never fabricate future bindings or placeholder Lua entries.
Connect actual [FeatureWorld](../../../docs/featureworld.md) cases, independent
expected state and manifest IDs together. Preserve saved source/IDs and write only
to explicit scratch through ProjectFiles. Isolated system fixtures remain necessary.

Choose affected validation/lifetime/persistence/FeatureWorld checks under the
policy; compile changed public examples. Broaden to Core/Server/adapters only when
the change reaches them. Update contracts, migration/rejection rules, mappings and
handoff together. The contract validator checks structure, not API behavior.
