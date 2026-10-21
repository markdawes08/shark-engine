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
Follow the [human and terminal-agent contract](../../../docs/architecture/0008-human-agent-authoring.md)
and its [implementation plan](../../../docs/roadmap/human-agent-plan.md) as each
authoring layer lands; the bootstrap does not yet provide these interfaces.

The implemented-surface manifest includes the mandatory Name and Transform C++
components and their validation/mutation tests. 07c adds real fixed schema and
bounded revisioned query coverage. Authoring transactions, Lua and FeatureWorld
arrive in 09, 19 and 10 respectively. Do not fabricate bindings for those layers or fill the
empty command/Lua arrays with placeholders. Connect each earlier component when
its missing layer is introduced.

07a now supplies [scene identity and lifetime](../../../docs/scene-contracts.md)
through EngineCore, with private EnTT, checked scoped references and an explicit
deferred boundary. 07b implements mandatory Name/Transform and hierarchy; follow
the [transform contract](../../../docs/transforms.md) and
[07 checkpoint plan](../../../docs/roadmap/scene-plan.md). Its
[current-machine qualification is complete](../../../docs/handoffs/07b-transform-hierarchy.md).
Follow the implemented [schema and inspection contract](../../../docs/scene-inspection.md)
and [07c handoff](../../../docs/handoffs/07c-schema-inspection.md).
07c and full REQ-07 are PASS under the current-machine scope; the
[07c receipt](../../../docs/coverage/evidence/07c-schema-inspection.json) records
376 passing CPU CTests and protected dependency/package checks. M02 is complete;
[M02b owned sessions](../../../docs/handoffs/M02b-owned-sessions.md) is **PASS**
under the current-machine scope. M02c [replication](../../../docs/network-replication.md)
is also locally qualified; follow [ADR 0013](../../../docs/architecture/0013-authoritative-replication.md).
Only explicitly selected validated affine poses enter its public schema; authoring
metadata never grants player visibility or mutation authority. The bounded client
cache is presentation-only. Preserve stream/session fences and atomic updates.
Do not introduce a second registry or expose
native entities. Include math through `shark/math.hpp`
and its SharkMath target so every consumer shares the central GLM configuration.
Cached scene world matrices use a local frame; global WorldPosition, future M05
origins and later physics constraints remain distinct.

Extend the fixed schema catalog through engine-owned IDs, explicit versions and
typed defaults/ranges/units/mutability. Name and Transform are version 1 and
mandatory/nonremovable; unknown IDs, versions and mismatched value types reject.
Reuse their existing validators rather than interpreting caller-edited metadata
as validation policy. Pure validation does not reserve a scene revision or prove
hierarchy/world feasibility. Query through the existing scene owner with bounded
fields/results, revision checks and owned immutable pages. These are CPU C++
contracts, not source files, terminal framing or gameplay permissions. Preserve
explicit xyzw quaternion metadata and signed scale's absolute magnitude floor.

Name and Transform cannot be removed. Reuse scene validation, checked parent
links and the existing deferred queue for component changes. Keep-local preserves
local TRS; keep-world rejects nonrepresentable local TRS before publishing links
or any descendant world matrix. Delete removes the subtree in one operation.
Actual changes advance the revision once; no-ops do not. Test stale/cyclic/foreign
parents, authority mismatch, depth/scale failures and complete rollback, alongside
owned state copies and deferred FIFO revalidation.

Follow [MMORPG authority and identity](../../../docs/architecture/0007-mmorpg-architecture.md):
classify state as server-authoritative, predicted, replicated or presentation-only.
Player requests are bounded authenticated intents, not authoring transactions or
arbitrary component setters. Separate durable identity from local/replica handles;
test stale ownership epochs, reconnect, region handoff and duplicate durable actions
when those layers exist. Render-frame Lua must not decide live gameplay outcomes.

For M02b, follow [ADR 0012](../../../docs/architecture/0012-owned-sessions.md) and
the [session contract](../../../docs/network-sessions.md). SessionAuthority takes
the actual ready server-side mutual TLS transport for each request; copied peer
identity values cannot establish provenance. AccountId, CharacterId and SessionId
are separate from scene and replica identities. Readiness changes session state
only; it grants no gameplay outcome, arbitrary component mutation or authoring
access. Preserve bounded enrollment, revocation, lifetime, rate limits and
reconnect fencing when extending admitted intents. Editor/terminal adapters and
durable ownership still require their assigned later services.

For an implemented surface, trace valid and invalid behavior through engine
validation, serialization/versioning, editor fields, typed commands, selected
Lua bindings and cooked export as applicable. Document intentional unsupported
operations. Every exposed API gets a stable manifest entry linked to real tests
and a FeatureWorld exercise or justified external fixture.

Human editor actions and terminal-agent commands must call the same validated
services, schemas and transactions. Publish machine-readable discovery for
supported fields, operations, limits and capability requirements alongside human
help. CPU authoring operations must run without an editor process, window or GPU;
preview/render operations declare and check their separate capabilities. Preserve
stable JSON diagnostics and request/job IDs, bounded output, cancellation status
and artifact references for long work. Test both adapters against the same
validation, revision-conflict, retry/idempotency and undo/redo behavior where
applicable. Explicitly identify irreversible operations; never silently overwrite
another actor's edits or imply that an unsupported operation can be undone.

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

M02d [spatial interest](../../../docs/network-interest.md) is locally qualified.
Preserve the complete trusted catalog, authenticated character observer binding,
unthrottled privacy maintenance and bounded managed admission. Its two new CTest
families are `shark.core.net-interest` (30 seconds, seed 1337) and
`shark.core.net-interest-process` (120 seconds); retain these families alongside
the earlier network tests. Two active recipients and controlled reliable-stream
delay do not establish the full LOCAL-MMO-1 workload or WAN behavior. The
[M02d handoff](../../../docs/handoffs/M02d-spatial-interest.md) retains that gate's
historical evidence; M02e subsequently closes integrated qualification.

M02e's additive `ReplicationStats`, `InterestStats` and `ReplicaClientStats`
report owner-thread logical counts and actual retained vector capacity bytes.
They are not wire fields or total process memory. Preserve cleanup semantics,
reserved capacity and nonmutating rejection checks when extending these APIs.
Retain all fourteen network CTest families, including `shark.core.net-adversity`
(60 seconds) and `shark.core.net-adversity-process` (360 seconds). The
[qualification contract](../../../docs/network-qualification.md) and
[M02e handoff](../../../docs/handoffs/M02e-integrated-qualification.md) record
full REQ-M02 PASS under the current-machine scope. Assignment 08 follows the
[persistence checkpoints](../../../docs/roadmap/persistence-plan.md).
08a [source documents](../../../docs/source-documents.md) reuse the shared
Name/Transform validators and explicit component version 1. Extend format fields,
strict rejection policy and migration fixtures together when components gain
persistence. The codec is CPU EngineTools only; Scene activation, file commits,
shared command history and terminal/editor adapters retain their owners.

08b's [authored Scene owner](../../../docs/authored-scenes.md) captures complete
committed state and replaces only a fully validated candidate. Preserve immutable
source metadata, incarnation-bound extras, expected revisions, dirty protection
and pending/visit/thread rejection. No ordinary Scene setter becomes an atomic
batch; shared history remains 09. Extend capture and construction together when
new persisted components land, through the existing Scene validators.
