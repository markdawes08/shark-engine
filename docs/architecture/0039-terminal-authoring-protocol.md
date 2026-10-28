# ADR 0039: Bounded terminal authoring protocol

Date: 2026-10-28. Status: accepted implementation decision for assignment 15a.

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
