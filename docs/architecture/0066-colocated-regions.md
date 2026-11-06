# ADR 0066: co-located regional ownership in one simulation island

Date: 2026-11-06. Status: accepted for M05d. Extends
[ADR 0065](0065-world-traversal.md), [ADR 0059](0059-authoritative-character-movement.md)
and [ADR 0062](0062-gameplay-lifecycles.md). Follow the
[regional contract](../region-world.md), [M05 checkpoints](../roadmap/seamless-world-plan.md)
and [testing policy](../testing-policy.md).

## Decision

Keep adjacent regions in one retained Scene, PhysicsWorld and traversal frame for
M05d. `RegionWorld` privately owns the movement, combat and interest adapters and
admits regional gameplay through a checked `RegionGrant`. A grant identifies its
process-local owner scope, WorldEntityId, child RegionId and generation. The
shared Scene's RegionSceneScope still identifies the common simulation island;
it is distinct from child regional routing authority.

Use two to eight bounded, nonoverlapping, edge-connected half-open XZ rectangles.
Their union constrains ordinary movement. Full segment coverage rejects paths
that cut through an unowned gap, even when both endpoints are owned. Cell
residency and traversal readiness remain separate from regional ownership.

At each admitted tick, execute existing gameplay and native physics exactly once.
Then prepare all regional membership candidates from committed poses before
publishing fresh grants for crossings. The same actual session, entity, native
body, queued movement, life and gameplay streams continue across the boundary.
Old grants reject before actual session command admission. Reconnect retains
M04's fresh actual session/transport checks and rotates the regional grant.

Retain at most 32 player records, including dead and detached records, and 16
separate NPC records. NPCs have checked native dynamic primitives and no invented
player/session/economic identity. Their bounded steering velocities run through
the same union/readiness gates and the same native tick as players. Navigation
progress remains a host observation adopted only after checking committed motion.

Publish public managed actors through the existing InterestServer and private
CharacterAuthority session adapter. Neighbor visibility uses one spatial
hysteresis selection with regional overlap at least its exit radius. Runtime
availability is distinct from privacy: dead/detached rows remain catalogued with
`present=false`, while privacy checks still apply. Crossings preserve replica
streams and handles. Required visible rows that exceed the stream limit fail
explicitly rather than silently hiding gameplay participants.

Retain pending durable ticket ownership by entity and character. A current grant
can reconcile its original operation after crossing; another actor or stale
grant cannot consume the receipt. Existing Store transactions and OperationIds
remain the durable authority. Regional grants add no persistence writer lease.

## Consequences and later boundaries

This profile gives enforced regional ingress and continuous cross-boundary
movement, collision, melee, projectile and native NPC traversal without copying
Scenes, reconnecting clients or introducing another simulation scheduler.
Borrowed host owners must outlive RegionWorld; trusted host/maintenance C++ APIs
remain privileged. Grant admission is not a sandbox against a malicious host.

Native work is not rollback. A tick reports its original combat/native prefix,
NPC velocity-write count and any later error. A faulted regional owner rejects
further gameplay and requires explicit teardown/reconciliation. Retrying a failed
tick cannot repair partial effects.

M05e must supply a real consistent transfer cut, outstanding-operation routing and
persistent writer fencing before independent authorities can take ownership.
M05f must qualify separate local processes, transport failure and recovery. These
requirements are not satisfied by child grants in a common Scene. Connected
M05g acceptance, final cooked closure in 31/M08 and later social/economic authority
remain separate gates. No distributed capacity or high-availability claim follows
from M05d.

No source/prepared/database format changes or new scheduler, importer or native
dependency are required by RegionWorld. Existing wire identities remain stable;
the region grant is a trusted process-local value, not a new client credential.
