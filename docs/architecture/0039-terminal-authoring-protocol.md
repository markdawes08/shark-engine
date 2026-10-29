# ADR 0039: Bounded terminal authoring protocol

Date: 2026-10-28. Status: accepted. The original 15a decision follows;
the dated extensions below add editing/persistence, imports/jobs and run/artifact adapters.

Implement a CPU terminal adapter in EngineTools and a `SharkAuthor` executable.
The adapter uses the existing ProjectFiles and AuthoredScene services and Core
component/query metadata. It owns no second mutable scene model. Its public C++
header exposes engine-owned values and strings; nlohmann JSON and wire helpers
remain private. Runtime, Server and EngineCore gain no authoring dependency.

Use a bounded [JSON-RPC 2.0](https://www.jsonrpc.org/specification) profile with
independent Shark `protocol_version: 1`. The
[protocol contract](../authoring-protocol.md) defines exact framing, fields,
limits, errors and executable examples. Support one-shot EOF input and persistent
LF/CRLF-delimited stdio, with no TTY, HTTP listener, model SDK or MCP requirement.
Machine stdout carries only responses; startup failures and diagnostics use stderr.

Require named parameters, reject unknown fields and unsupported versions, and
reuse the bounded source JSON preflight for duplicate keys, UTF-8, nesting, nodes
and finite numbers. Bound requests to 64 KiB and responses to 1 MiB. Accept only
bounded string, nonnegative 53-bit integer or null request IDs. Valid notifications
execute normally without a response. RPC batch arrays are explicitly unsupported
and dispatch no members; this restriction does not change native atomic authoring
transaction semantics. JSON-RPC method framing is not a gameplay protocol or
an authorization boundary for remote administration.

Fix the project root when the service is created. Hold one ProjectFiles lock and
one AuthoredScene owner for an open project. Load and validate complete candidates,
prepare the complete reply and enforce its bound before changing active ownership.
Opening another project requires explicit close. Dirty or pending state prevents
explicit close, and conflicting cooperating writers reject. Destruction releases
the owner without silently saving. Later write adapters must use the same owner,
history and retained-operation services; project source state remains separate
from live MMO economic persistence.

Expose exactly eight implemented methods in 15a: `protocol.discover`,
`schema.components`, `project.open`, `project.status`, `project.close`,
`scene.query`, `scene.inspect` and `scene.continue`. Discovery includes parameter
and result descriptions, examples, native component metadata and bounded
capabilities. Advertise CPU authoring with mutations and device capture false.
Do not advertise future editing, jobs, captures or export methods before their
adapters exist.

Translate authored entity identities through the active scene, returning durable
UUIDs and scope rather than native handles. Preserve Core query selection, name
prefix validation, row and scan bounds, revision fences, owned page values and
explicit field selection. Serialize incarnation/revision counters as canonical
uint64 decimal strings; selected absent Camera/MeshRenderer values are null,
while unselected fields are omitted. Local rotations use xyzw and world matrices
state their column-major layout in the scene-local frame. Serialize the existing
four component schemas and twelve properties from the Core catalog, including
typed defaults, bounds, enum choices, units and mutability. Do not duplicate or
weaken component validation in the adapter.

Retain at most 32 outstanding native cursor/field-selection pairs under opaque
wire tokens. A successful continuation consumes its old token and publishes a
fresh token if another page remains. Encode and admit the next bounded response
and replacement cursor before consuming the old token; failures preserve it.
Do not evict another query silently. Close/reopen invalidates all tokens. This
single-use wire contract deliberately differs from native copyable cursor replay.
There is no durable cursor, acknowledgement of stdout delivery, or safe blind
replay after a lost response; callers restart a revision-fenced query.

The initial adapter is read-only with respect to authored source. Later checkpoints
attach mutations, history, retries, jobs and export to these shared services.
In particular, export must use the active authoring owner or reject conflicting
ownership; it may not bypass unsaved state through a separate disk-reloading worker.
The protocol/source/component/cooked/gameplay/database versions remain distinct
under [ADR 0004](0004-formats-and-identity.md).

Qualification uses focused native service tests and a real subprocess fixture
with redirected UTF-8 streams, invalid input, ownership conflict, bounded paging,
cursor expiry and clean shutdown. Existing component/source/query coverage remains
applicable. The [testing policy](../testing-policy.md) determines selected compiler,
platform and sanitizer checks; this CPU adapter does not require GPU qualification.

## 15b extension: editing and persistence, 2026-10-28

Extend the existing host and version-1 discovery to nineteen implemented methods,
with `mutations:true`. Add project creation, pure batch preview, retained command/
batch/undo/redo/clear operations, history and mutation lookup, scene save/reload and
scoped text read/write/release. Keep the same ProjectFiles/AuthoredScene ownership pair.
No new command executor, history model, retry ledger, listener or scheduler exists.
The protocol guide defines the exact fields, schemas and executable examples.

Require current revisions for scene writes and native scoped/sequenced mutation
IDs for every command/history mutation. Structural wire decoding retains the
original typed values; shared services validate components and ordered batches.
Return both the current enclosing revision and immutable original decision.
Preserve all three result levels: RPC/service access, retry admission/lookup, and
the original operation. A retained rejection is not successful editing. Preview
has no retention or publication; a successful preview reserves nothing.

Save preserves history and retry retention. Reload validates a complete candidate,
defaults to rejecting unsaved edits and supports explicit revision-bound discard.
Reload creates a fresh scene incarnation and clears history/cursors while keeping
the native retry scope. Close/open creates a new owner and expires all such state.
Close also supports explicit revision-bound discard; this permits deliberate
handoff when external project edits block saving. EOF/destruction never saves;
callers must keep the stdio session alive and explicitly publish intended changes.

Project creation uses caller-owned durable IDs and two absent destinations in
existing directories. Publish the empty scene first and then its project catalog.
Return each successful file publication separately; a failed second publication
does not roll back or delete the first. Saves use retained exact scene content
revisions and report publication separately from durability. Save, reload and text
access additionally check the retained project's exact bytes before proceeding,
so external catalog changes cannot silently redirect operations.

Text access is limited to `scripts/**/*.lua` (including direct `scripts/*.lua`)
through ProjectFiles, with exact lowercase directory/suffix spelling, 32 KiB UTF-8
content and no NUL. It does not parse or execute Lua. Exclude the active project
and every declared scene path, including case aliases and path-prefix overlaps;
typed source ownership cannot be bypassed by changing filename extensions.
Raise the protocol preflight decoded-string limit to 32 KiB while retaining the
64 KiB complete request bound and existing smaller field limits. Keep at most 32
owner/path-bound text revision tokens with no silent eviction. Null means
create-only; successful replacement returns a new token and consumes the old one.
Explicit token release recovers observation capacity without accessing files.

Scene mutation retry recovery uses the native ledger; file publications are not
retained mutations. Output failure can follow successful dispatch/publication.
Report reconciliation context on post-dispatch reply-preparation failure and
require retained-outcome lookup or file inspection before another intent. Neither
a broken pipe nor a missing response proves rollback or exactly-once delivery.
Retain the existing same-machine file race/durability limits. New acceptance
extends actual subprocess workflows and native publication fault fixtures under
the testing policy; unrelated GPU/Server matrices remain outside this CPU change.

## 15c extension: imports and jobs, 2026-10-28

Extend version-1 discovery to forty implemented methods with `imports:true` and
`background_jobs:true`. Add native metadata read/write/release, explicit catalog
refresh/query/resolve, importer discovery, cache inspection/maintenance and bounded
job submit/list/cancel/drain/take/close. Keep one retained ProjectFiles/AuthoredScene
owner; lazily attach its AssetCatalog/AssetImports services and exclusively owned
byte/glTF importers. Reuse the native worker, capture/publication fences, completion
queue and cache path. There is no second scheduler, independent writer, watcher,
source decoder, durable job journal or change to Runtime/Server dependencies.

Decode the full native `shark.asset` envelope through the existing metadata codec.
Importer settings remain validated by the native import service. Keep 32 separate
owner/path-bound metadata file tokens, with exact-content write conflicts, explicit
release and truthful publication/durability receipts. Exclude active project and
all declared scene paths from both the sidecar and derived source path, including
case aliases and prefix overlap. Metadata must belong to the retained project.
Metadata edits do not modify scene history or implicitly refresh the catalog.
The complete caller-supplied sidecar inventory validates atomically; catalog paging
uses bounded offsets and expected revisions, distinct from scene cursor tokens.

Encode catalog/cache scopes as UUIDs and counters/job sequences as canonical uint64
decimal strings. Preserve fresh/stale/missing cache state, nullable current-input
diagnostics and last-valid keys. Artifact inspection returns validated descriptors,
dependency keys, digests and payload byte count. It does not return binary payloads:
native payloads can exceed the unchanged 1 MiB protocol response bound. A retained
artifact descriptor is not proof of freshness. Transport request/string/node limits
continue to constrain metadata operations and the native catalog bounds.

Preserve the native four retained jobs and single worker. Submit captures immutable
inputs and can deduplicate matching pending work; RPC correlation IDs are not job
retry identities. List observes only. Drain processes at most one ready completion
through the active owner and reports both its count and all retained statuses.
Return state, completed/total nodes and complete nullable summary/commit/error
fields. A successful outer RPC or drain is not a successful import. Preserve
directory-flush warnings as published outcomes and failed/stale/cancelled imports
as terminal states without replacing last-valid output.

Take acknowledges a terminal outcome and releases its slot only after a bounded
reply is prepared. Cancellation cannot undo a published success and dispatched
work remains retained until drained or closed. Close permanently closes admission,
requests stop and joins before releasing dependencies, retaining outcomes while
the project session remains open. Project close/EOF/destruction use the same
join-before-owner-release order; they never publish pending jobs or save scene
edits. Native callbacks must return/cooperate; no preemption is promised. New
sessions use fresh scopes and require cache/input reconciliation after lost work.

Check the retained project file before asset file/catalog/cache work and job
publication. Keep list/cancel/take/close and metadata token release available when
an external project edit blocks publication, so users can reconcile and clean up.
Metadata tokens, job take and release prepare their bounded response before
consuming observations; writes retain the existing post-dispatch reconciliation
error context. A missing response is not proof of rollback or a durable receipt.

Qualify the adapter through real redirected subprocess workflows over scratch
projects, native metadata/import/cache/jobs and licensed offline glTF fixtures.
Check preservation of unsaved scene state/history, last-valid output, stale and
foreign identities, cancellation/take/closure and owner handoff. Reuse native job
fault and decoder coverage; the testing policy selects affected Debug, applicable
sanitizer and portable WSL checks without repeating unrelated device matrices.
Full REQ-15 remains open until connected 15e acceptance.

## 15d extension: run, export and capture, 2026-10-28

Extend the same version-1 protocol with `scene.run` and capability-gated
`scene.export`/`scene.capture`. Discovery reports actual installed adapters:
41 methods in the CPU base, an additional configured Windows export method, and
an additional configured Client capture method. Unavailable artifact methods
reject explicitly rather than requiring a GUI, silently saving or replacing the
active project. Existing protocol/source/cooked versions remain unchanged.

Capture the exact committed revision through AuthoredScene, including dirty
values. Feed the existing deterministic scene runner an owned isolated snapshot
for the static `scene-integrity` case, with bounded seed/ticks/Hz, entity-count
expectation and native assertion/status report. No gameplay, Lua, arbitrary
callback execution or write-back is implied. Preserve authoring history, retry
state, source files and dirty baseline. A failed assertion remains FAIL even when
its enclosing RPC successfully returns the report. This synchronous adapter adds
no scheduler, job retention or concurrent cancel command.

For artifacts, reuse the retained ProjectFiles/AuthoredScene/catalog/import owners
and `prepare_client_export`/`validate_client_export`. Require startup-scene camera,
current scene/catalog/cache revisions and fresh client-permitted dependencies,
including hidden references. Observe the exact saved project and scene files;
external source edits reject publication without discarding current unsaved work.
Both adapters capture immutable content and revalidate through the still-live
owner immediately before publishing a fresh output outside the project root.
Do not construct another source writer or reload saved state behind the session.

Configure executable export helpers only at trusted host startup. Extend the
existing package tool to consume staged immutable content and request final
validation from the live owner. Keep the project lock through helper completion,
bounded child I/O/lifetime and final rename. Preserve existing package checks,
exact Runtime CRT closure, full content notices and Core-only startup data.
The existing export report separates success and publication; lost acknowledgement
after publication requires output reconciliation. Package creation is not
isolated deployment or native rendering qualification.

Keep EngineTools and `SharkAuthor` independent of Client. A separate
`SharkAuthorClient` composition installs an application-owned capture provider
using engine-owned request/result types. The provider consumes immutable prepared
content, reconstructs a private read-only Core scene and invokes the existing
offscreen scene renderer. It reports original source identity separately from the
fresh rendered Core incarnation. Preserve the global world anchor independently
of the bounded local offset. Configuration retains validated scene shaders but
initializes no device; actual capture creates only a hidden native window/device.

Capture requires bounded dimensions/pixels, GPU completion, clean diagnostics and
safe resource close before preparing its raw linear RGBA8/depth artifact. Stage
all files, revalidate the retained owner and publish by final rename. Return
bounded artifact metadata/digests instead of inline pixels. Its publication flag
does not promise disk-flush durability, actual window presentation or final PBR
quality. Client/device failures preserve the existing bounded retention and fatal
shutdown rules; no second renderer or completion system is added.

Qualify public redirected CPU run/rejection flows, dirty/history/source fences,
configured package publication and real optional captures on both available GPUs.
Use the testing policy's affected Debug/sanitizer/WSL selection and existing
independent image/depth references and package tooling. Full REQ-15 remains open
until 15e connects the complete public authoring workflow.

## 15e qualification, 2026-10-29

The [connected handoff](../handoffs/15e-terminal-acceptance.md) closes REQ-15
under current-machine scope. Its public standard-library example creates typed
project/scene/metadata through the protocol, imports and modifies a licensed
glTF dependency closure, saves/reopens, runs and optionally captures/exports the
retained unsaved scene before an explicit final save and owner handoff. Independent
checks bind saved fields, import keys and actual readbacks/packages to that intent.
No protocol method, source format, engine service or dependency changes in 15e.
The same CPU workflow runs on Windows and WSL; native capture/deployment stays
Windows-only. Human editor consumption and parity remain assignments 16/17.
