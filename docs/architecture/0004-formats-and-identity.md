# ADR 0004: Formats, identity and compatibility

Status: accepted contract; formats are implemented in their owning assignments.
Date: 2026-10-17.

MMORPG amendment: the seamless open world contract below is planned, not
implemented. It supplements the existing authoring and cooked formats under
[ADR 0007](0007-mmorpg-architecture.md) and the [MMORPG plan](../roadmap/mmorpg-plan.md).
Internal regions partition streaming and authority; they are not player-visible
zones or a requirement for loading screens between adjacent world locations.

## Source and derived data

Use readable UTF-8 JSON with explicit `format` and integer `schema_version` fields.
Reserve `.sharkproject`, `.sharkscene`, `.sharkprefab` and `.sharkmaterial` for
project, scene, prefab and material source documents. Their initial production
schemas are independently versioned at 1. This decision specifies their envelopes
and compatibility rules; field schemas and fixtures land in 08, 21 and 26.
Lua source is UTF-8 `.lua`; project settings declare the Lua API version separately.
Human formatting is not identity and is not a cache key by itself.

08a implements the project/scene version-1 fields and tested version-0 migration
in the [source-document contract](../source-documents.md). The CPU EngineTools
codec returns owned validated values and readable text. It does not activate a
Scene or access files; those gates follow the [08 checkpoint plan](../roadmap/persistence-plan.md).
Authored IDs and global-cell origins remain distinct from live player records,
region authority, session replicas and database versions.

08b connects those values through the EngineTools [authored Scene owner](../authored-scenes.md).
It captures one committed revision and prepares a complete inactive candidate
using Core's hierarchy/transform validation. Expected revisions, unsaved-state
policy and a single ownership swap govern replacement. Source metadata stays
separate from runtime handles, live authority and later filesystem durability.

08c's [project file service](../project-files.md) provides bounded native reads
and complete validation before publication. Exact-content revisions are bound to root/path, sibling stages
publish with native rename, and committed outcomes report durability separately.
External writers must coordinate around the final compare/rename interval; these
per-file source operations do not implement M03 durable player transactions.

| Layer | Contract | Owner |
| --- | --- | --- |
| Authoring source | IDs, relative source references, component schema versions; readable and durable | 08 |
| Asset metadata | Persistent asset ID, source origin/license, import settings and dependency IDs in versioned JSON sidecars | 11 |
| Derived development cache | Disposable engine-owned binary payloads and JSON index keyed by input/settings/tool/dependency hashes and target profile | 11/12/14 |
| Cooked assets | Validated binary payloads with magic, kind, independent version, bounded lengths/offsets and content digest; little-endian on disk | 31 |
| Cook manifest | Versioned JSON mapping IDs to payloads, sizes, digests, dependencies and target profile, startup scenes and explicit dynamic roots | 31 |
| Authoring protocol | JSON-RPC 2.0 over local stdio plus a separately versioned Shark method/schema contract, initially 1 | 15 |
| Lua API | Engine-owned surface, API version 1; not the Lua language version | 19 |
| Gameplay protocol | Independently versioned authenticated session, intent and replication messages; never the authoring JSON-RPC schema | M02 |
| Server durable state | Transactional database schema for characters, world state and durable operations, with explicit migration and recovery rules | M03 |

Never serialize raw C++ structs, pointers, EnTT indices, compiler ABI layouts or
padding. Encoders use explicit widths and byte order. Runtime validates cooked
headers, kind, versions, offsets, counts and dependency references before making
resources visible. Container and per-kind payload versions are independent from
source schema and engine release versions. Hashes detect inconsistent payloads;
they are not a signature or a hostile-content sandbox.

Assignment 14 stages only validated development payloads and runtime scene data
prepared by EngineTools. It is a temporary, explicitly versioned export mode.
Assignment 31 replaces it with cooked dependency closure, including script-selected
assets declared as explicit roots. Final Runtime excludes source project/scene
readers, external importers, cookers and offline source shader compilers such as
DXC. System driver compilation remains permitted and must not require an installed
developer SDK. [ADR 0009](0009-windows-linux-platform-scope.md) removes the former
Vulkan-on-Metal shipping path. Source Lua scripts may
ship as script payloads for portability; no native Lua bytecode compatibility is
promised between CPU architectures or Lua releases.

## Stable identity and references

Persistent project, scene, entity, asset and prefab-local IDs are randomly generated
128-bit UUIDv4 values from OS entropy, serialized as lowercase hyphenated strings.
Tests use an injectable seeded ID generator and fixed imported IDs so creation
and callback order can be replayed; production does not use that test seed.
Reject nil/invalid IDs
where a reference is required and duplicate IDs within their defining namespace.
IDs survive rename, reparent and save/load; paths and content hashes are not IDs.
References between scenes carry scene plus entity ID. Asset metadata moves with
its source; importing an unrecognized duplicate sidecar must diagnose a collision.

Runtime entity handles contain a scene-instance generation, entity slot and slot
generation. Each dereference checks all three. Scene unload invalidates its
instance generation; slot reuse increments generation. Retire a slot or scene
token rather than wrapping a generation into a valid old handle. Authored scene
IDs resolve to handles only within their active scene instance. Lua, physics user
data and GPU selection records use checked IDs/handles, never borrowed component pointers.
07a implements these process-local lifetime checks and distinct authored/world
identity domains in the [scene contract](../scene-contracts.md). Region and replica
scope carry immutable authority/session metadata; actual network authentication,
replica mapping, distributed fencing and durable storage remain M02/M03/M05.
07b adds mandatory Name/Transform and checked parent/child links through the same
owner. Names are nonunique display text; rename and reparent preserve persistent
identity and the live handle. Parent links stay within an active scene instance
and matching authority incarnation. Subtree deletion invalidates all descendant
handles before their slots can be reused; see the [transform contract](../transforms.md).
07c implements [schema metadata and bounded revisioned inspection](../scene-inspection.md).
The fixed catalog identifies Name as `shark.name` / numeric ID 1 and Transform as
`shark.transform` / numeric ID 2, each at schema version 1. Property IDs 1..4
identify Name value and Transform translation, rotation and scale. Unknown IDs,
unsupported versions and mismatched value types reject; there is no implicit
migration. These are explicit C++ discovery contracts, not raw enum/struct bytes
or a scene/gameplay format. Assignment 08 owns source persistence and its
migration/rejection fixtures. Schema version 1 does not set the source envelope,
authoring protocol or gameplay-wire version. Do not serialize GLM object memory
or infer a scene-file version from the in-memory Transform representation.

Query pages carry a scene-incarnation/revision pair and owned values. Continuation
rejects after committed changes rather than silently combining revisions. Local
handles and continuation cursors remain process-local; a returned persistent ID
does not acquire authority or make a stale handle valid. The inspection contract
defines bounded filtering, field selection and visible continuation separately
from later source encoding and terminal protocol framing.
Prefab instances remap every local entity ID and internal reference atomically;
references outside the prefab remain asset or explicitly scoped scene references.

## MMORPG identity, ownership and replication

Use distinct engine-owned types, retaining UUIDv4 for new durable identifiers.
Account IDs identify authenticated principals; character IDs identify durable
player characters; world-entity IDs identify server-created gameplay objects.
They are not interchangeable with authored entity or asset IDs. A character's
association with a live entity is explicit and can change on respawn or recovery.
Character and persistent world-entity IDs survive reconnect, server restart and
region transfer. Ephemeral entities have live identities without an implied
database record. IDs are references, never credentials or proof of ownership.

A world ID and stable spatial cell/region IDs identify content and partition
locations. Region instances identify running authorities over that content;
an authority epoch/fencing token distinguishes successive owners. Spatial cell
identity does not change merely because a process or authority assignment changes.
Account, character, world and region references are validated within their declared
scope. Authentication/session secrets have separate representations and lifetimes.

Live server handles include the owning region-instance incarnation and checked
slot generations. Network replica IDs are compact protocol identifiers scoped
to a connection/session incarnation and replica generation, with an explicit
mapping to authoritative entity identity. They are not EnTT indices, Jolt body
IDs or persistent database keys. Reuse must never make a stale packet or handle
refer to a different live object. Restart, reconnect and handoff invalidate old
session/authority mappings; a stable durable ID alone cannot authorize a mutation.

The server enforces character control permissions and the chosen duplicate-login
policy. Every gameplay intent carries the necessary session, sequence and
authority context, but the server derives permission from authenticated state,
not a client-supplied owner field. Message size, queue depth, sequence windows,
retry retention and rate limits are bounded and tested in M02. State replication
uses explicit field schemas and recipient visibility rules: persistent,
replicated, owner-private, server-only and presentation-only fields are classified
deliberately. Interest filtering does not replace authorization.

M05 introduces a fenced transfer between internal regions. There is at most one
committing authority for an entity at a time. Transfer records bind entity IDs,
source/destination incarnations, an ownership token and an operation ID; retries
and in-flight intents are deduplicated across the transfer. The destination
prepares validated state before activation, and the source cannot resume writes
after losing its fence. Transfer uses a consistent authoritative tick/input-sequence
cut, a final source snapshot and pending durable-operation context; an earlier
preparation snapshot cannot overwrite newer state at activation. The source
quiesces at the cut. Post-cut intents and late worker/storage completions route to
the committed owner with deduplication/reconciliation, so acknowledged outcomes
cannot be lost between preparation and ownership commit. Cut/commit fault tests
include in-flight mutations and late durable success, with bounded buffering and
handoff pause. Failure before ownership commit aborts safely; recovery
after commit follows the durable owner record rather than allowing both owners
to roll back independently. Client replica continuity is explicit even when the
local runtime handle changes. Start with co-located region authorities; claiming
distributed handoff requires the same failure tests across processes.

## MMORPG durable state and trust boundaries

Authored scene/project files define content. They do not serve as the live
character database, an inventory ledger or a save file for a running server.
M03a selects and pins SQLite privately in EngineStorage under
[ADR 0015](0015-durable-store.md), with a separately versioned database schema,
explicit create/open and immutable store/world identity. M03b supplies owned character records and M03c supplies
[atomic economic operations](../durable-transactions.md); runtime recovery and migration/restore retain M03d/M03e. Durable commands use operation IDs and atomic transactions
covering both the state change and the retained deduplication result. Retrying a
committed operation after a lost acknowledgement must not duplicate a reward,
item or currency transfer. Deduplication retention and request expiry must agree:
an old request cannot become a new valid operation merely because its cached
result expired. Acknowledging durable success requires the declared
commit boundary; uncommitted state must not be presented as durable success.

Database schema versions, gameplay wire versions, content/cooked versions and
Lua API versions evolve independently. M02 specifies negotiation and rejection
of incompatible peers before joining a world. M03 specifies tested database
migrations, backup/restore, interrupted migration recovery, stale-write rejection
and compatibility with the deployed server build. Source JSON migration rules
remain for authoring documents; they do not implicitly migrate server databases.

Only the owning simulation thread commits live region state. Database workers
return bounded results tagged with operation, entity and authority incarnation;
the owner revalidates them before applying a completion. Storage outage and
timeout policies bound pending work and reject affected durable operations
without granting an uncommitted benefit. Recovery reconciles committed results
before resuming entity admission. Never block the simulation indefinitely on
storage, or let a late completion mutate a replacement entity/region.

Gameplay clients submit bounded intents, not arbitrary component writes, Lua
source, filesystem paths, authoring transactions or server save/load commands.
Local CLI/JSON-RPC authoring stays a separate trusted interface. Shared engine
validation can be reused, but does not grant gameplay clients editor privileges.
M08 defines separately authenticated and authorized administrative operations.
No public server protocol or hostile-client security claim exists yet.

[ADR 0008](0008-human-agent-authoring.md) supplements the local authoring format:
versioned UTF-8 machine results/discovery, stable semantic IDs and revisions,
bounded queries and job artifacts, clean protocol stdout and conflict-aware text
writes. Source/schema, authoring protocol and result envelope versions remain
explicit; changing one does not silently change the others. Exact wire framing,
schemas and compatibility fixtures arrive in 15. Human and agent edits share one
owner/history; a lost reply never authorizes an unchecked duplicate mutation.

M02 gates malformed/oversized messages, stale sessions/replicas, duplicate and
out-of-order intents, unauthorized character control and private-field leakage.
M03 gates crash/retry around each durable commit boundary, a lost acknowledgement,
restore/migration failure and rejected stale completions. M05 gates transfer
retry, source/destination failure and abort/recovery with exactly one owner,
preserved durable identity and no duplicated committed operation. Planned
fixtures are specified in the MMORPG plan; none is an executed result.

## Validation and version policy

Unknown future source versions, unknown components and unknown source fields
are errors, not silently omitted data. Explicit `extras` metadata may be preserved
opaquely with size limits. Optional absent fields receive schema-defined defaults.
Reject duplicate JSON keys, NaN/infinity, invalid UTF-8, invalid references,
excessive nesting and out-of-range values. Assignment 08 sets and tests concrete
limits before exposing a reader.

Every source version upgrade provides a tested, sequential migration or explicitly
rejects that old version with an actionable diagnostic. Assignment 08 includes a
real version-0 fixture and 0-to-1 migration; version 0 is a test/legacy contract,
not a claim that files exist now. Migrate in memory; save only on an explicit
successful operation. Preserve the original on migration failure. Cooked versions
are exact-match only: incompatible cooked data requires recooking, not runtime
migration. Protocol and Lua API major mismatch rejects initialization/discovery
with supported version details. Public changes update the ADR and manifests.

Resolve project paths relative to a canonical project root, with forward slashes
in documents. Reject absolute/drive/UNC paths, traversal and symlink/reparse-point
escapes for project-scoped operations. Detect case-folded path collisions for
portable packages; preserve Unicode names. Normal loading never fetches network
URIs. Never join untrusted relative strings without validating the resolved path.

Validate a complete candidate scene/project before replacing the active one.
Save to a temporary sibling, flush, and atomically replace with platform-specific
durability/error handling; a failed write preserves the prior valid file. Cache,
cook and export publication follow the same staging/commit principle. These
failure paths are exercised in 08, 11, 30 and 31.
