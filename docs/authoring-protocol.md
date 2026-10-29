# Terminal authoring protocol

`SharkAuthor` is a CPU terminal adapter over the existing
[ProjectFiles](project-files.md), [AuthoredScene](authored-scenes.md),
[component schemas and bounded queries](scene-inspection.md). 15a supplies discovery
and inspection; 15b adds project creation, shared edits/preview/history, retained
mutation outcomes, save/reload and scoped script text. These operations need no
editor, display, GPU, network listener, model provider or MCP client. 15c adds
typed asset metadata, explicit catalog inventory, cache inspection/maintenance
and the existing bounded import jobs. 15d adds deterministic static-scene runs,
configured Windows standalone export and optional GPU capture through the same
retained owner. [15e](handoffs/15e-terminal-acceptance.md) completes connected
acceptance through the [runnable example](#complete-terminal-example).

[ADR 0039](architecture/0039-terminal-authoring-protocol.md) defines this bounded
profile. The public C++ owner is
[authoring_protocol.hpp](../engine_tools/include/shark/tools/authoring_protocol.hpp);
JSON parsing and wire conversion remain private to EngineTools. Runtime and Server
gain no Tools or JSON dependency. These are local development operations, separate
from gameplay requests and live-world administration.

## Launch and framing

```text
SharkAuthor --stdio|--request [--root DIRECTORY] [--project RELATIVE_PATH]
            [--export-python PATH --export-tool PATH]
SharkAuthor --help|--version
SharkAuthorClient --stdio|--request [same ownership/export options]
                  [--capture-vertex-shader PATH --capture-pixel-shader PATH
                   --capture-device UUID --capture-validation required|off]
```

Select exactly one transport mode. `--root` fixes an existing project root for the
process; a request cannot change it. Without a root, discovery/schema requests and
closed-project status are available, but `project.open`/`project.create` fail. `--project` requires
`--root` and opens that relative source project before reading requests. Startup
open selects the project's declared startup scene. A failed startup open writes
its structured error to stderr and exits 2; success produces no startup response.
Use `project.open` to select another declared scene explicitly.

The optional export pair configures trusted host-startup programs, not executables
selected by an RPC. Use the local Python interpreter and
[`tools/package_runtime.py`](../tools/package_runtime.py); both must be existing
regular files. Actual standalone export is Windows x64 only. Without that pair,
`scene.export` is absent from discovery and rejects with `capability_unavailable`.

`SharkAuthorClient` is the separate Tools + Client composition; `SharkAuthor` and
EngineTools remain CPU-only. All four capture options are required together.
The shader inputs must be complete precompiled scene-unlit artifacts from the
[offline pipeline](shader-pipeline.md), the device must be an exact lowercase
32-hex UUID, and validation is explicitly `required` or `off`. Configuration
validates/retains shaders without creating a window or GPU. The device is created
only for an actual capture, using a hidden window. Unconfigured discovery/help
remain device-free, and no foreground editor window is opened. Qualification
uses required validation; validation-off execution is diagnostic only.

Both modes accept UTF-8 through redirected stdin with no TTY or prompt:

| Mode | Input and completion |
| --- | --- |
| `--request` | One JSON-RPC object, read through EOF. JSON whitespace/newlines are allowed within its total byte limit. No final newline is required. Emit its response and exit. |
| `--stdio` | One complete JSON object per LF-terminated line; CRLF is also accepted. The LF and optional preceding CR are framing, outside the request-byte limit. Process requests sequentially, flushing each response. Empty EOF ends normally; any nonempty unterminated final frame is fatal. |

This is newline framing, not HTTP or `Content-Length` framing. In machine modes,
stdout contains only compact JSON response objects, each followed by LF. Valid
notifications have no response line. Ordinary diagnostics and startup/internal
host failures use stderr. The executable initializes binary stdin/stdout on Windows
so byte limits and newline framing do not depend on text-mode translation.

| Bound | Limit |
| --- | --- |
| One request | 65,536 UTF-8 bytes; `--request` counts all stdin bytes |
| One response | 1,048,576 UTF-8 bytes, excluding the output LF |
| JSON nesting | 32 container levels |
| JSON nodes | 8,192, counting containers, values and object keys |
| Decoded JSON string | 32,768 bytes, with smaller field-specific limits below |
| Outstanding query cursors | 32 per open project |
| Retained text-revision tokens | 32 per open project |
| Retained asset-metadata revision tokens | 32 per open project, separate from text tokens |
| Catalog inventory / page | 4,096 sidecars / 1..128 records; complete requests and responses must still fit |
| Retained import jobs / running jobs | 4 / 1; terminal outcomes occupy slots until taken |
| Script text | 32,768 UTF-8 bytes; the full encoded request must also fit 65,536 bytes |

The shared source JSON preflight rejects duplicate decoded keys, malformed UTF-8,
nonfinite numbers and out-of-range integer representations before creating a DOM.
Unknown envelope/parameter fields, unsupported versions and positional parameter
arrays reject explicitly. No source or protocol version is inferred from another
format's version.

Exit codes are 0 for normal completion, 1 for a failed one-shot protocol request,
and 2 for startup, framing, I/O or host failures. In `--stdio`, ordinary request
errors produce error responses and the next complete frame can proceed; reaching
normal EOF returns 0 even if earlier requests failed. A blank line is a malformed
JSON request and produces a parse error. Oversized frames and unterminated final
frames emit a transport error and exit 2; the process does not dispatch their
partial content. An error notification in `--request` has no response body but
still returns 1. Typed operation failures inside a JSON-RPC `result` also return
1 in one-shot mode; see the nested outcome rules below.

## Request and response profile

Discovery identifies each method's `execution_context: "cpu_owner_thread"`,
`requires_open_project` and `requires_project_root` prerequisites, as well as its
parameter/result schemas and example request. `scene.*`, `text.*` and `asset.*` methods require
the open owner; `project.open`/`project.create` require the root fixed at host creation.

Shark uses the [JSON-RPC 2.0 specification](https://www.jsonrpc.org/specification)
request/response and notification conventions with an explicit bounded profile:
named parameters only, one request object per frame, and no RPC batch arrays.
This batch restriction is about RPC envelopes. Use `scene.preview` or `scene.mutate`
for the existing native [atomic authoring batches](authoring-transactions.md).

```json
{"jsonrpc":"2.0","protocol_version":1,"id":"discover-1","method":"protocol.discover","params":{}}
```

`jsonrpc`, integer `protocol_version: 1`, and `method` are required. Method names
are nonempty valid UTF-8 without controls, at most 128 bytes. `params` may be
omitted, meaning `{}`; when present it must be an object. The only envelope keys
are `jsonrpc`, `protocol_version`, `id`, `method` and `params`.

An `id` is a nonnegative integer at most 9,007,199,254,740,991, a nonempty UTF-8
string of at most 128 bytes without C0/C1/DEL controls, or JSON null. Floating-point
IDs, including `1.0`, reject. Prefer a string or integer; null is a request ID and
still receives a reply. Omitting `id` makes a notification: a valid notification
dispatches normally and suppresses its response, including method/parameter or
engine errors. It is not an acknowledgement. Use IDs when callers need returned
state, cursor tokens or confirmation that a project operation succeeded.

Ordinary service replies include `jsonrpc: "2.0"`, `protocol_version: 1`, the same
`id`, and exactly one of `result` or `error`. Framing/startup errors are separate
host failures with null ID; their error data identifies the transport/startup
operation rather than a successfully decoded request.

| Error code | Meaning |
| --- | --- |
| `-32700` | JSON/preflight parse failure |
| `-32600` | Invalid envelope, unsupported RPC batch, or transport framing failure |
| `-32601` | Method not implemented |
| `-32602` | Invalid method parameters |
| `-32603` | Host/internal failure |
| `-32001` | Unsupported Shark protocol version |
| `-32000` | Shared engine operation failed, including ownership/revision/cursor conflicts |

Service error `data` contains the stable engine `code`, `protocol_version`, and
`context` with `operation`, `request_id`, `path`, `entity_id` and `asset_id`.
When an active owner's status is readable it also contains `current_revision`.
Readable messages explain the failure; clients should branch on codes and context.
A version rejection includes `supported_versions: [1]`. Invalid envelopes whose
IDs were not accepted use null. Rejected requests do not silently restart queries,
close another owner, or switch the active project.

## Implemented methods

`protocol.discover` is the executable source of available method names, parameter
and result descriptions, examples, component metadata, limits and capability
flags. The CPU base advertises 41 methods. Configured Windows export adds one;
configured Client capture adds one, for at most 43. Query the actual host rather
than assuming a method count or device capability:

| Method | Parameters | Result |
| --- | --- | --- |
| `protocol.discover` | `{}` | Version, transport, limits, capabilities, methods and components |
| `schema.components` | `{}` | `{ "components": [...] }` |
| `project.open` | Required `path`; optional `scene_id` | Open-project status |
| `project.create` | `path`, `scene_path`, `name`, `project_id`, `world_id`, `scene_id` | Per-file publication outcomes and open-project status on success |
| `project.status` | `{}` | Current status, or `{ "open": false }` |
| `project.close` | Optional `unsaved` and `expected_revision`; discard requires the current revision | `{ "open": false }` after successful close; bare `{}` is idempotent when closed |
| `scene.query` | Selection/filter/fields/bounds/revision below | Bounded query page |
| `scene.inspect` | Required `entity_id`; optional `fields`, `expected_revision` | One-row page with no continuation |
| `scene.continue` | Required `cursor` returned by an earlier page | Next bounded page, with the earlier selection and revision |
| `scene.preview` | `batch`, `expected_revision` | Pure batch validation with nested outcome |
| `scene.mutate` | `mutation_id`, `expected_revision`, `operation` | Retained decision, current revision and original nested outcome |
| `scene.history` | Optional `expected_revision` | Common undo/redo counts, tickets and limits |
| `scene.mutation_status` | Optional `expected_revision` | Retry scope, next identity, retained range and limits |
| `scene.mutation_outcome` | `mutation_id` | Retained original outcome or explicit lookup failure |
| `scene.save` | `expected_revision` | Publication, saved revision and clean-baseline acknowledgement |
| `scene.reload` | `expected_revision`; optional `unsaved` | Open-project status after explicit source activation |
| `text.read` | `path` | Script text and opaque content-revision token |
| `text.write` | `path`, `text`, `expected_revision` token or null | Publication and replacement content-revision token |
| `text.release` | `revision` token | `{ "released": true }` after releasing a retained text observation |
| `asset.importers` | `{}` | `{importers:[...]}` with actual native descriptors and settings schemas |
| `asset.metadata.read` | `path` | `{path,metadata,revision}` with the complete native metadata envelope and opaque file token |
| `asset.metadata.write` | `path`, `metadata`, `expected_revision` token or null | `{path,revision,publication}` |
| `asset.metadata.release` | `revision` token | `{released:true}` |
| `asset.catalog.status` | `{}` | `{revision}` |
| `asset.catalog.refresh` | Complete `sidecars` inventory, `expected_revision` | `{revision}` after complete candidate validation |
| `asset.catalog.query` | `expected_revision`; optional `offset`, `page_size` | `{revision,total,records,next_offset}` |
| `asset.catalog.resolve` | `asset_id`, `expected_revision` | `{revision,asset_id,availability,record}` |
| `asset.cache.status` | `{}` | Cache revision, counts, bytes and unpublished-stage observation |
| `asset.cache.query` | `asset_id`, `target`, `expected_catalog`, `expected_cache` | Fresh/stale/missing state, required/last-valid keys and nullable input failure |
| `asset.cache.inspect` | `key`, `expected_revision` | Validated artifact descriptor, digests and payload byte count |
| `asset.cache.reload` | `expected_revision` | `{revision}` after adopting the validated disk cache |
| `asset.cache.prune` / `asset.cache.clear` | `expected_revision` | `{revision,changed,publication}` |
| `asset.cache.recover` | `{}` | `{removed}` for the reserved unpublished cache stage |
| `asset.jobs.submit` | `asset_id`, `target`, `expected_catalog`, `expected_cache` | `{job_id,deduplicated}` |
| `asset.jobs.list` | `{}` | `{jobs:[...]}`; observation does not publish |
| `asset.jobs.cancel` | `job_id` | Current job status after requesting cancellation |
| `asset.jobs.drain` | `{}` | `{drained,jobs:[...]}`; publishes at most one ready completion |
| `asset.jobs.take` | `job_id` | Terminal status, acknowledging and releasing its retained slot |
| `asset.jobs.close` | `{}` | `{closed:true,jobs:[...]}` after stopping and joining the worker |
| `scene.run` | `expected_revision`; optional `seed`, `ticks`, `hz`, `run_id`, `revision`, `expected_entities` | Captured revision/dirty state, entity count, source digest and native scene-run report |
| `scene.export` (configured Windows host) | Common content fields below plus `build_dir`, `crt_dir`, `shaders`; optional `notices` | Existing standalone export report with status and publication outcome |
| `scene.capture` (configured Client host) | Common content fields below; optional `width`, `height`, `local_origin` | Capture artifact report with source identity, raw-file digests and actual GPU diagnostics |

The fixed capabilities are `cpu_authoring:true`, `mutations:true`, `imports:true`,
`background_jobs:true` and `scene_runs:true`. `standalone_export` reflects the
configured Windows helper, and `device_capture` reflects the installed Client
adapter. These flags advertise supported paths, not a claim that arbitrary inputs
or hardware will succeed. Unavailable export/capture requests fail explicitly even
though their names are absent from the current method inventory.

`schema.components` returns all four native schemas and twelve properties in
stable catalog order. Components include numeric `id`, `stable_id`, `display_name`,
`version`, `required`, `removable` and `properties`. Properties include numeric
`id`, `key`, `display_name`, stable-text `type`, `unit`, `mutability`, native JSON
`default` and `constraints`. Enumeration constraints include numeric `value` and
text `key` choices. MeshRenderer's required asset has a null default and
`constraints.required: true`; no invented asset is supplied. Scalar/vector bounds,
quaternion normalization/sign policy, units and mutability come directly from
Core. The complete shared component validators still govern cross-field rules
such as Camera's projection-dependent clipping relationship.

## Project ownership

`project.open.path` is a contained project-relative source path, at most 4,096 UTF-8
bytes. It must identify a valid source project. Optional `scene_id` is a lowercase
canonical UUIDv4 declared in that project; otherwise its startup scene is used.
Paths and source documents pass through existing ProjectFiles validation.

Opening acquires the existing exclusive project lock and constructs the scene
through shared source activation. A complete candidate, status reply and bounded
encoded response must succeed before the new session becomes active. The service
retains one ProjectFiles/AuthoredScene ownership pair until close or destruction.
A second cooperating writer fails; external writers must follow the same
coordination contract. Opening while any project is already active rejects, even
for the same path. Close explicitly first.

Open status reports `project_id`, `world_id`, `project_name`, relative
`project_path`, `scene_id`, `revision`, `dirty`, `pending_mutations`, original
project/scene `source_schema_version`, `file_revisions` byte counts, and
`cursors.retained`/`cursors.maximum`. The byte counts are observations, not exposed
content-revision tokens or permission to overwrite files. Status reads the owned
scene; it does not silently reload changed source files.

Close defaults to `unsaved:"reject"` and rejects dirty state or pending mutations.
`unsaved:"discard"` requires the complete current `expected_revision` and explicitly
abandons unsaved state; pending work still rejects. An optional expected revision
is checked even for a clean close. Bare `{}` is idempotent when already closed;
a closed owner cannot satisfy a supplied revision. Successful close releases
the scene, history, mutation ledger and cursor/text/metadata tokens before the
project lock. Asset jobs stop and their worker joins before their catalog/files
owners are released; uncommitted imports are cancelled, never published on close.
EOF, broken output and process destruction release ownership without saving;
unsaved state and retained evidence are lost when the process exits. Keep one
`--stdio` process alive for editing and explicitly save before close/EOF. A new
process cannot resume an earlier owner's revision, history or retry scope.

`project.create` requires existing root/parent directories, two distinct contained
paths and caller-generated canonical UUIDv4 project/world/scene IDs. It creates an
empty startup scene with the supplied project name. Both destinations must be
absent; existing files are never overwritten. It publishes the scene first and
then the project through ProjectFiles. This is two file publications, not an atomic
multi-file transaction. The result is:

```json
{"success":true,"published":{"scene":{"published":true,"bytes":100,"durability":"file_flushed"},"project":{"published":true,"bytes":200,"durability":"file_flushed"}},"project":{"open":true},"error":null}
```

Byte counts above are illustrative and `project` contains the full status. A
failure result has `success:false`, an owned `error`, and null for each file that
was not published. If project publication fails after scene publication, the scene
remains, its receipt is returned, and no project owner is activated. Preserve that
file and reconcile deliberately; creation is not a retained mutation to replay.

`scene.save` requires the current complete scene revision. It uses the scene file
revision retained at open/save/reload and rejects external content changes before
publication. Its result contains `success:true`, `saved_revision`,
`current_scene_clean` and `publication:{published:true,bytes,durability}`. Save
preserves scene incarnation, history and the mutation ledger. The clean baseline
is acknowledged only for the captured revision. Publication durability is
`file_flushed`, `file_and_directory_flushed` or `directory_flush_failed`; the last
still describes a published file, not rollback. See the [file contract](project-files.md).

`scene.reload` validates the entire saved scene before replacing the active
incarnation. `unsaved` defaults to `"reject"`; `"discard"` explicitly authorizes
replacing unsaved edits, still subject to the expected revision and normal owner
guards. Reload clears history/cursors, retains the same mutation ledger/scope,
and returns a fresh scene incarnation. Old retained outcomes remain historical.
Close/open instead creates a fresh owner, including a fresh retry scope.

Save, reload and text access also compare the retained project's exact on-disk
bytes. External project catalog edits reject rather than redirecting writes or
silently adopting new ownership. Preserve the unsaved state before reconciling
that edit and explicitly close/open to adopt the changed catalog. A revision-bound
discard close is available when the external catalog change prevents saving.
Queries and
in-memory edits continue against the retained project snapshot. File operations
do not form a transaction against an uncooperative external writer racing the
final comparison and native rename.

## Shared edits, preview and retained outcomes

All scene writes call the same [command](authoring-commands.md),
[batch](authoring-transactions.md), [history](authoring-history.md) and
[retry](authoring-retries.md) services. `scene.mutate` requires a `mutation_id`
obtained from `scene.mutation_status.next`, the observed `expected_revision`, and
one operation:

| `operation.type` | Additional fields |
| --- | --- |
| `command` | `command`: one typed command below |
| `batch` | `batch:{id,commands}`: UUIDv4 correlation ID and up to 256 ordered commands |
| `undo` / `redo` | `ticket`: canonical decimal string from the current history direction |
| `clear_history` | None; explicitly clears common history without changing scene values |

`scene.preview` takes `{batch:{id,commands},expected_revision}`. It validates the
same ordered candidate and history budget, publishes nothing, consumes no mutation
sequence and reserves no revision. The result is
`{batch_id,observed_revision,outcome:{ok:true,value:{changed,affected_entities}}}`
or `outcome:{ok:false,error:{code,message,context}}`. Even after successful preview,
commit must use the original expected revision and can reject intervening edits.

Every command has `type` and `entity_id`, a durable authored UUIDv4:

| `command.type` | Additional fields |
| --- | --- |
| `create_entity` | Optional `name`, `transform`, `parent_id`, `camera`, `mesh_renderer`; native Name/Transform defaults and absent optional components apply |
| `delete_entity` | None; deletes the complete subtree |
| `set_name` | Required `name` |
| `set_transform` | Required `transform` |
| `reparent_entity` | Required `parent_id` (UUIDv4 or null), `mode` (`keep_local` or `keep_world`) |
| `set_camera` | Required `camera`, or null to remove |
| `set_mesh_renderer` | Required `mesh_renderer`, or null to remove |

Transform input is `{translation:[x,y,z],rotation:[x,y,z,w],scale:[x,y,z]}`;
all three fields are required when supplied. Camera input requires `projection`,
`vertical_fov_radians`, `orthographic_height`, `near_clip` and `far_clip`.
MeshRenderer requires `asset_id`, uint32 `mesh_index` and boolean `visible`.
These are values to validate, not a mutable scene borrow. Wire decoding rejects
unknown fields, wrong types, nonfinite numbers and invalid UUIDs. Bounded typed
component inputs then reach native validation without prior normalization, so
invalid component values can have retained terminal outcomes. Names are admitted
up to 4,096 bytes; the native component limit remains 256 bytes with its UTF-8 and
control-character rules. A batch ID correlates diagnostics; it is not a retry ID.

Mutation IDs are `{scope:"UUIDv4",sequence:"uint64-decimal"}`. Request IDs only
correlate JSON-RPC messages. The complete mutation result preserves three distinct
decisions: JSON-RPC access/decoding, ledger admission/lookup, and the original
operation's success or rejection:

```text
result.mutation_id
result.state
result.observed_revision                      # current owner revision
result.outcome.ok                             # original outcome available?
result.outcome.value.mutation_id
result.outcome.value.observed_revision        # original decision's revision
result.outcome.value.result.ok                # original operation succeeded?
result.outcome.value.result.value             # {changed, affected_entities}
result.outcome.value.history_after            # history object only for clear, else null
```

At either nested failure, `ok:false` has `error:{code,message,context}` instead of
`value`. Inspect every level; a JSON-RPC `result` or a retained decision alone is
not edit success. `affected_entities` is a sorted refresh set, including deleted
IDs and affected parents/descendants, not a list of surviving entities.

`state` is `recorded` for a new admitted decision, `replayed` for an identical
retained submission, or `retained` for a successful lookup. Other states are
`not_recorded`, `unknown_outcome`, `payload_mismatch`, `out_of_order` and
`not_admitted`. Both successful edits and admitted native rejections consume a
sequence. A changed payload under an existing identity does not run again. Keep
the complete typed input and expected revision to recover a lost reply; retry the
same identity or call `scene.mutation_outcome` while that owner retains it.

`scene.mutation_status` reports the scope, current revision, decimal
`high_watermark`, nullable `next`/`oldest_retained`, retained entries/bytes and
limits. Default retention is 32 decisions and an 8 MiB charge. Evicted IDs and IDs
from another owner return `unknown_outcome`; inspect current state and reconcile
before choosing a new intent. There is no durable retry journal across restart.
Save, reload and history clear preserve the ledger; close/EOF/process death do not.

`scene.history` reports revision, synchronization, undo/redo counts, retained byte
charge, limits and nullable `next_undo`/`next_redo` groups with decimal `ticket`
and `command_count`. Defaults are 32 groups and a 32 MiB charge. A changed command
or batch is one shared group; previews, no-ops and rejected operations leave the
stacks intact. Undo/redo require both the current revision and next ticket and
travel through `scene.mutate`, making lost-reply recovery use the same ledger.
Save keeps history; reload clears it. Neither history nor retry charges measure
process RSS.

If response preparation fails after dispatch, an internal error with context
operation `authoring.protocol.reconcile` means the operation may have taken effect.
A broken pipe is likewise not proof of rollback. Query retained outcomes for
scene mutations and inspect files for file operations. Creation, save, reload,
text/metadata writes and cache maintenance are not covered by scene mutation
deduplication. For imports, inspect retained job outcomes and current cache state.

## Scoped text access

`text.read` and `text.write` access UTF-8 files only under exact lowercase
`scripts/` with a lowercase `.lua` suffix, including nested paths. They store
text; they do not parse or execute Lua or advertise a scripting runtime. Root and
parent directories must exist. Empty text, tabs and newlines are valid; malformed
UTF-8, embedded NUL, paths outside the scope and more than 32,768 bytes reject.
JSON escaping still counts toward the 65,536-byte complete request limit.

The active project and every declared scene path are excluded, including
case-folded aliases and file/directory prefix overlaps. Source document formats
are extension-independent, so renaming a scene to `scripts/scene.lua` cannot
bypass typed source ownership. Catalog edits are detected through the retained
project file revision before text access. Project paths, native no-follow checks,
atomic publication and exact-content conflicts reuse ProjectFiles.

`text.read {path}` returns `{path,text,revision}`. The revision is an opaque
owner/path-bound token, not a timestamp, digest or scene revision. Pass it as
`text.write.expected_revision` to replace that exact observed content. Explicit
null means create-only and requires an absent file. Writes return
`{path,revision,publication}`; successful replacement consumes the old token and
returns a new one, while rejection preserves the old token and external content.
Text writes are immediate file publications and do not change scene dirty state,
undo history or mutation sequences.

`text.release {revision}` releases an unused token after its response is admitted,
without reading/writing files or changing scene/history state. Unknown, consumed
or foreign tokens reject. This works even when external project edits block file
operations; it changes only the active owner's observations.

At most 32 text tokens are retained without silent eviction. Each read adds one;
a replacement write swaps its token without adding to the count. At the limit,
existing tokens remain usable for writes, while new reads/create-only writes
reject until `text.release` frees a slot. Close releases all tokens. They cannot be replayed after consumption or
reopen. After uncertain delivery, reread the actual file and compare intent before
writing; a new token does not prove whether an earlier write ran.

## Asset metadata, inventory and cache

The adapter reuses [native metadata/catalog](asset-identity.md),
[cache](asset-import-cache.md) and [job](asset-import-jobs.md) services under the
retained project owner. It owns the registered byte, glTF resource and glTF mesh
importers; `asset.importers` returns their actual `id`, integer `version`, `kind`,
integer `payload_version` and `settings_schema`. Declaring an importer in metadata
does not install it. These CPU operations do not modify the active scene, dirty
state, undo history or mutation ledger. External changes to the retained project
file reject asset file/catalog/cache operations instead of silently changing
ownership. Job list/cancel/take/close and metadata token release remain available
for reconciliation and cleanup; drain still checks the project fence.

Metadata access uses contained adjacent `source.ext.sharkasset` paths and the
complete `shark.asset` schema-1 object from the native contract. Required fields
are `format`, integer `schema_version`, `project_id`, `asset_id`, `kind`, `origin`,
`license`, `importer`, `audience`, `settings` and `dependencies`; `extras` is optional.
The native codec validates it, including settings/extras object bounds, UUIDs,
identifiers, dependency uniqueness and unknown fields. Importer-specific settings
validation remains in the import service. Both sidecar and derived source paths
must avoid the active project and all declared scenes, including case aliases
and file/directory prefix overlap. Metadata must belong to the active project.

Read returns the canonical metadata object and an opaque owner/path-bound token
for the exact file bytes. A write requires that token, or explicit null for an
absent destination. Successful replacement consumes the old token, returns its
replacement and reports `{published:true,bytes,durability}`; the durability values
are the same as scene/text publication. Rejection preserves the old token.
These immediate file writes are outside scene undo and do not refresh the catalog
automatically. There are 32 metadata tokens, separate from text tokens; release
unused observations with `asset.metadata.release`. Never pass a text token as a
metadata revision. Reopen expires both kinds of token. A lost write response
requires reading and comparing the sidecar before another write.

Catalog and cache revisions are distinct scoped values:
`{scope:"UUIDv4",value:"uint64-decimal"}`. Canonical decimal strings follow the
scene counter rules below; do not convert them through floating point. Each
owner gets a fresh scope, so a revision from another project session rejects even
when its counter matches. The catalog begins empty. Supply the complete explicit
sidecar inventory to `asset.catalog.refresh`; there is no recursive scan or watcher.
The full candidate must validate before replacement. A malformed, foreign or
duplicate sidecar leaves the old snapshot intact. Omitting a sidecar removes its
derived record without deleting files. A metadata write or outside source edit
requires an explicit refresh to update that snapshot.

Catalog query defaults to offset 0 and page size 64, bounded to 128 records per
page. Every page requires the original catalog revision; `next_offset` is null
when complete. This integer offset is separate from the single-use scene cursor
tokens. Records are ordered by asset ID and contain `metadata`, `source`,
`sidecar`, `metadata_bytes` and `source_present`. Resolve reports `availability`
as `unknown_id`, `source_missing` or `source_present`, and a nullable `record`.
Source presence does not establish import success. Large metadata pages can exceed
the 1 MiB response bound; retry a smaller page at the same observed revision.
The protocol's request/string/node bounds also apply to native metadata inputs;
the native 64 KiB sidecar limit does not enlarge the transport envelope.

An import target is `{id:"windows-client",side:"client"}` or another explicit
lowercase target identifier and `client`/`server` side. The target ID is an import
key input, not platform autodetection. `asset.cache.query` checks the current
captured inputs without importing and returns `revision`, `asset_id`, `target`,
`state`, nullable `required_key`, nullable `last_valid_key` and nullable `blocked_by`.
The last field is a native `{code,message,context}` error. Failed/missing inputs
can retain last-valid output while `state` is `stale`; it is not fresh content
authorization. Callback-bearing queries/inspection can return a transient conflict
while the one worker uses an importer. Job status/control remain available.

`asset.cache.inspect` validates a specific retained key and returns `asset_id`,
`key`, `kind`, `audience`, `target`, `importer`, integer `importer_version` and
`payload_version`, `source_digest`, `settings_digest`, dependency `{asset_id,key}`
pairs, `payload_digest` and `payload_bytes`. Digests/keys are 64 lowercase hex
digits. It returns no inline binary payload, whose native 2 MiB bound exceeds
the protocol response limit. Inspecting an old key does not certify freshness;
use `cache.query` for that decision.

Cache status reports `revision`, `artifacts`, `bindings`, `payload_bytes`,
`file_bytes` and boolean `unpublished_stage`. Reload explicitly adopts the valid
on-disk cache; prune retains all last-valid binding closures; clear discards the
disposable cache. Prune/clear return `{revision,changed,publication}`, where
`publication` is null for no file publication or the actual durability receipt.
Recover removes only the reserved unpublished cache stage, never source-save
stages, and does not revive a job. Cache maintenance preserves authored files;
revision-changing maintenance can make admitted jobs stale. After an uncertain
publication, inspect the cache/file state and reconcile instead of blind replay.

## Import jobs and terminal outcomes

`asset.jobs.submit` captures bounded immutable inputs through the current owner
and requires both catalog/cache revisions. It returns a native scoped
`job_id:{scope:"UUIDv4",sequence:"uint64-decimal"}` and `deduplicated`. This is
separate from JSON-RPC correlation and scene mutation identity. Identical pending
captures deduplicate; terminal or cancellation-requested jobs do not. The existing
service has four retained slots, one lazy worker and 128 nodes per closure, with
1 MiB/source, 2 MiB/payload and the native aggregate byte bounds. It introduces
no second scheduler, cache owner, disk spool or durable job journal.

Each status contains `job_id`, `state`, `completed_nodes`, `total_nodes`, and
explicitly nullable `summary`, `commit` and `error`. States are `queued`, `running`,
`ready`, `succeeded`, `failed` and `cancelled`. Progress counts completed dependency
nodes, including reuse; it is not a percent or estimated duration. Ready means
worker computation has ended, with owner publication still pending. List only
observes. Drain never waits for computation and processes at most one ready
completion, then dispatches the next queued job. `drained:0` means none was ready.

A successful status includes:

```text
summary: {cache_revision,catalog_revision,root,target,root_key,nodes,imported,reused}
commit:  {revision,changed,publication:null|{published:true,bytes,durability}}
error:   null
```

The summary preserves original input revisions; the commit reports the actual
result revision. A failed terminal status carries `{code,message,context}` even
when the enclosing RPC/drain succeeded. Inspect state, summary, commit and
durability; an admitted job or `result` object alone is not import success.
A published directory-flush warning remains a successful publication. Publication
rechecks native owner/catalog/cache scopes, exact source/sidecar bytes and payload
validity. An edit, another commit, reload or clear can reject stale work without
replacing last-valid output. Reconcile and submit new intent explicitly.

Cancel requests cooperative stop. Queued work can become terminal immediately;
dispatched/running/ready work remains retained until drain or close. The owner
fences publication after cancellation even if a trusted callback ignores stop.
Cancelling a succeeded job returns its original success and does not undo it.
Take requires a terminal job, returns its outcome and releases its slot; it is
acknowledgement, not rollback. Taken or foreign identities reject. Lost take
delivery can lose the last retained status, so inspect current cache state before
new work. Terminal jobs are never silently evicted.

`asset.jobs.close` permanently closes admission for this project session, requests
stop, joins the worker and returns retained terminal statuses. It is idempotent;
cache inspection/maintenance remains available. Project close, EOF and destruction
also stop/join before releasing project ownership, losing untaken statuses on
exit. They do not drain/publish uncommitted jobs or save unsaved scenes. Trusted
native callbacks must return/cooperate; no preemption or real-time shutdown
latency is promised. Keep a stdio owner alive, explicitly drain/take outcomes and
save intended scene changes before close. One-shot submit cannot retain a worker
across process exit.

## Deterministic runs and generated artifacts

`scene.run` captures the active committed revision, including unsaved edits, and
passes those owned values to the existing [scene runner](scene-runner.md).
Its single `scene-integrity` case checks the captured values and entity count,
then observes an unchanged isolated scene at each deterministic tick. It does
not execute gameplay, Lua, imported scripts, physics or arbitrary user callbacks.
The isolated run is never activated back into the authoring session. Source
files, dirty baseline, history, retry ledger and query cursors remain unchanged.
The run intentionally uses the retained scene even when an external source file
has changed; producing an export or capture instead checks those files below.

Only `expected_revision` is required. Defaults are seed `"1337"`, 60 ticks,
60 Hz, `run_id:"local"` and diagnostic `revision:"unspecified"`. Seed is a
canonical uint64 decimal string. Tick count is 1..10,000; Hz is 1..1,000. Run ID
and diagnostic revision must match `[A-Za-z0-9._-]{1,64}`. `expected_entities` is an
optional integer 0..4,096; a mismatch produces a truthful failed assertion in the
report. That diagnostic `revision` label is distinct from `expected_revision`.
Runs use rational ticks through the existing scheduler, not elapsed wall time.

The result has `observed_revision`, `dirty`, `entity_count`, `source_digest` and
`report`. `source_digest` identifies the canonical captured source-scene encoding.
The report retains `format:"shark.scene-run"`, schema version 1, cases, ticks,
assertions and native failure details. Inspect `report.status` (`PASS`, `FAIL`,
`CANCELLED` or `NOT_RUN`); an outer JSON-RPC result does not imply a passing run.
A failed run makes one-shot exit 1. Stdio remains serial: there is no concurrent
run-cancel RPC or added job scheduler. Supervisors still bound process waits.

Export and capture share these required content fields:

| Field | Meaning |
| --- | --- |
| `expected_revision` | Exact active scene incarnation/revision |
| `expected_catalog` / `expected_cache` | Current UUID-scoped native catalog/cache revisions |
| `camera_id` | Explicit authored Camera UUID in the startup scene |
| `target` | `{id,side:"client"}` for the existing fresh import closure |
| `output` | New artifact directory outside the project root, with an existing parent |

Both operations use [prepare/validate client export](client-export.md) through the
retained project, scene, catalog and import owners. They require the declared
startup scene, resolve hidden references as well as visible ones, reject stale or
server-only content, and never import implicitly. Built-in geometry needs no
sidecars. Observe the current catalog/cache revisions even for built-in content.
The active project and scene source files must still match their exact retained
file revisions; external edits cannot redirect publication. Unsaved in-memory
edits are included without being marked saved or removing undo/retry history.

Output is create-only, outside the source project. Existing destinations, links,
reparse points and unsafe topology reject. A private staged artifact is validated
again against the same source files, owner revisions and selected asset keys
immediately before final publication. The project lock remains held throughout.
This is cooperating-writer coordination, not a hostile-filesystem transaction;
uncooperative source/topology changes must not race final validation and rename.

`scene.export` adds `build_dir` (the configured protected Runtime-only Release
build), `crt_dir` (its exact redistributable CRT directory), and `shaders` (the
validated scene shader artifact directory). Optional `notices` is a bounded array
of `{license,path}` entries; licenses must be unique and every used import license
must have its complete notice. It reuses the existing
[standalone exporter](standalone-export.md), dependency checks and package format.
Its private helper receives immutable staged content and requests validation from
the still-live owner before publication. It never opens a second source writer or
reloads the saved scene behind unsaved authoring state. Return fields remain the
`shark.client-export` version-1 report, including `status`, `published`, artifact
metadata and `qualification:"NOT_RUN"`. Package creation alone is not GPU or
isolated deployment qualification.

`scene.capture` defaults to 160x120. Each dimension is 1..2,048 and the combined
limit is 1,048,576 pixels. Optional `local_origin` is three finite numbers within
the existing +/-65,536 meter local-coordinate range; generation is 1 for this
standalone snapshot. The original world anchor is preserved separately and is
never added to a matrix. The Client adapter reconstructs a private Core scene
from the immutable prepared bytes, extracts a bounded snapshot and uses the
existing [offscreen viewport](scene-viewport.md). The result distinguishes the
original `source_revision` from the fresh `rendered_revision`; reconstruction
does not become another editable project owner.

Capture writes `color.rgba` (tightly packed top-left linear RGBA8), `depth.f32`
(little-endian normalized depth floats) and `report.json`. The returned
`shark.authoring-capture` schema-1 object equals that report and includes
`published`, `succeeded`, output path, project/scene/camera IDs, `source_dirty`,
prepared scene digest, both revisions, origin, dimensions, draw/extraction counts,
pixel-file sizes/digests, selected adapter/driver, validation, diagnostics and
closed resource counters. It writes no inline image bytes. GPU completion and
clean teardown are required before staging; final validation precedes publication.
`durability:"not_confirmed"` makes no file/directory flush guarantee. This is an
offscreen unlit capture, not an actually presented window screenshot or a claim
of texture/PBR/gameplay completeness.

Neither artifact operation is a retained mutation retry. Inspect `published`
separately from success/status. A failed or lost response after dispatch can leave
a published artifact: inspect/reconcile the original destination and report before
retrying with another path. Request IDs cannot roll back or deduplicate file
publication. Failed unpublished staging is cleaned when possible; process death
can leave a reserved stage for deliberate inspection/cleanup.

## Queries and durable identity

`scene.query` accepts only these keys:

| Key | Meaning |
| --- | --- |
| `selection` | `all` (default), `roots`, or direct `children` |
| `parent_id` | Authored entity UUIDv4; required for `children`, forbidden otherwise |
| `name_prefix` | Exact UTF-8 byte prefix, default empty; at most 256 bytes, no C0/C1/DEL controls, no case folding |
| `fields` | Unique array drawn from `name`, `transform`, `hierarchy`, `camera`, `mesh_renderer`; default first three; `[]` is identity-only |
| `page_size` | Integer 1..256, default 64 |
| `scan_budget` | Integer 1..4096, default 256 |
| `expected_revision` | Optional `{ "scene_instance": "...", "revision": "..." }` |

Both revision members are canonical unsigned decimal strings: `0` or a nonzero
first digit followed by digits, no leading zeros, and at most uint64 maximum
18,446,744,073,709,551,615. The pair distinguishes scene incarnation from revision;
a saved scene ID alone cannot fence a read after reopen. `scene.inspect` uses the
same fields and revision rules, but requires `entity_id` and accepts no query
filter or paging keys. Numeric bounds require JSON integers, not booleans or
floating-point representations.

Each page contains `revision`, authored `scope` (`kind: "authored"`, `scene_id`),
`entities`, `scanned_slots`, `complete` and `next_cursor` (opaque string or null).
An entity always includes durable `entity_id`, the authored `scope`, and null
`authority`. No runtime slot, generation, pointer or native EntityHandle is sent.
Additional keys appear only for requested fields:

- `name`: display string.
- `transform`: `local` with three-double `translation`, quaternion `rotation`
  `[x,y,z,w]`, and three-double `scale`; `world_matrix` with
  `layout: "column_major"` and sixteen `values` in column-major order. This is a
  scene-local matrix using column vectors, not a global MMO coordinate.
- `hierarchy`: durable `parent_id` or null, direct `child_count`, and `depth`.
- `camera`: null when absent; otherwise named `projection` (`perspective` or
  `orthographic`), `vertical_fov_radians`, `orthographic_height`, `near_clip` and
  `far_clip`.
- `mesh_renderer`: null when absent; otherwise `asset_id`, `mesh_index`, `visible`.

Core controls page order and bounded work. Scans count vacant/nonmatching slots;
an empty `entities` array can still have a continuation. Completion is determined
by `complete`/null `next_cursor`, not row count. Missing/stale entities, foreign
revisions and changed scenes return shared engine failures rather than partial
unfenced results.

The service retains the native cursor and selected fields under a fresh opaque
UUID token. `scene.continue` consumes that token only after the next result has
been successfully encoded, checked against the response limit, and any replacement
cursor admitted. Failure preserves the prior token. Success yields a fresh token
when more work remains; otherwise it frees the slot. Reusing a consumed token,
using one from another owner, or using one after close/reopen returns conflict.
There is no silent eviction. At 32 outstanding cursors, new `scene.query` calls
reject until a query finishes or the project closes; continuation can replace its
existing slot. Inspection remains available.

A changed mutation or successful reload clears retained query cursors. Preview,
no-op and rejected edits preserve them. A previous page remains an owned snapshot,
but its cursor cannot continue after a changed scene.

Consumption precedes delivery to the caller's stdout pipe. A broken pipe or lost
reply is not an acknowledgement and does not restore the token. Start a fresh
query, optionally with the last observed revision, when delivery is uncertain.
These single-use wire tokens intentionally differ from the native copyable
`SceneQueryCursor` replay contract. Neither is a durable cursor across restart.

## Terminal examples

From the repository root after building `SharkAuthor`, PowerShell can discover
the contract through a normal pipe. Set UTF-8 explicitly when piping non-ASCII
names; Windows PowerShell's default external-process encoding may differ.

```powershell
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = $OutputEncoding
$author = '.\build\windows-debug\bin\SharkAuthor.exe'
'{"jsonrpc":"2.0","protocol_version":1,"id":"d1","method":"protocol.discover","params":{}}' |
    & $author --request
```

The retained FeatureWorld project can be queried without a visible editor:

```powershell
'{"jsonrpc":"2.0","protocol_version":1,"id":"q1","method":"scene.query","params":{"selection":"roots","fields":["name","camera","mesh_renderer"],"page_size":16}}' |
    & $author --request --root '.\assets\FeatureWorld' --project 'FeatureWorld.sharkproject'
```

For multiple requests and cursor continuation, keep one `--stdio` process alive.
This Python example uses ordinary UTF-8 subprocess pipes, preserves returned IDs,
and follows every page, including empty intermediate pages:

```python
import json
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

with subprocess.Popen(
    [r"build/windows-debug/bin/SharkAuthor.exe", "--stdio",
     "--root", "assets/FeatureWorld", "--project", "FeatureWorld.sharkproject"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    text=True, encoding="utf-8",
) as process:
    def call(request_id, method, params):
        request = {"jsonrpc": "2.0", "protocol_version": 1,
                   "id": request_id, "method": method, "params": params}
        process.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
        process.stdin.flush()
        line = process.stdout.readline()
        if not line:
            raise RuntimeError("SharkAuthor exited before its response")
        reply = json.loads(line)
        assert reply["id"] == request_id
        if "error" in reply:
            raise RuntimeError(reply["error"])
        return reply["result"]

    page = call(1, "scene.query", {"fields": ["name"], "page_size": 16})
    request_id = 2
    while True:
        for entity in page["entities"]:
            print(entity["entity_id"], entity["name"])
        if page["complete"]:
            break
        page = call(request_id, "scene.continue", {"cursor": page["next_cursor"]})
        request_id += 1
    call(request_id, "project.close", {})
    process.stdin.close()
    assert process.wait(timeout=10) == 0
```

This independent example creates a temporary project under `out/`, previews and
commits a new entity, replays the retained intent, runs the unsaved scene, then
saves and reopens it. It checks each nested result; a different JSON-RPC request
ID does not execute the edit again:

```python
import json
from pathlib import Path
import subprocess
import tempfile
import uuid

author = Path("build/windows-debug/bin/SharkAuthor.exe").resolve()
Path("out").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="terminal-example-", dir="out") as temporary:
    root = Path(temporary).resolve()
    (root / "scenes").mkdir()
    with subprocess.Popen(
        [str(author), "--stdio", "--root", str(root)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        text=True, encoding="utf-8",
    ) as process:
        sequence = 0

        def call(method, params):
            global sequence
            sequence += 1
            request = {"jsonrpc": "2.0", "protocol_version": 1,
                       "id": sequence, "method": method, "params": params}
            process.stdin.write(json.dumps(request) + "\n")
            process.stdin.flush()
            line = process.stdout.readline()
            if not line:
                raise RuntimeError("SharkAuthor exited before its response")
            reply = json.loads(line)
            assert reply["id"] == sequence
            if "error" in reply:
                raise RuntimeError(reply["error"])
            return reply["result"]

        def value(outcome):
            if not outcome["ok"]:
                raise RuntimeError(outcome["error"])
            return outcome["value"]

        created = call("project.create", {
            "path": "Example.sharkproject", "scene_path": "scenes/Main.sharkscene",
            "name": "Terminal example", "project_id": str(uuid.uuid4()),
            "world_id": str(uuid.uuid4()), "scene_id": str(uuid.uuid4()),
        })
        assert created["success"], created
        entity_id = str(uuid.uuid4())
        batch = {"id": str(uuid.uuid4()), "commands": [
            {"type": "create_entity", "entity_id": entity_id, "name": "Spawn point"}
        ]}
        observed = call("scene.mutation_status", {})
        preview = call("scene.preview", {
            "batch": batch, "expected_revision": observed["revision"],
        })
        assert value(preview["outcome"])["changed"]
        intent = {"mutation_id": observed["next"],
                  "expected_revision": observed["revision"],
                  "operation": {"type": "batch", "batch": batch}}
        committed = call("scene.mutate", intent)
        original = value(committed["outcome"])
        assert value(original["result"])["changed"]
        replayed = call("scene.mutate", intent)
        assert replayed["state"] == "replayed"
        assert value(replayed["outcome"]) == original
        history = call("scene.history", {})
        run = call("scene.run", {
            "expected_revision": committed["observed_revision"],
            "ticks": 3, "seed": "1337", "expected_entities": 1,
        })
        assert run["report"]["status"] == "PASS" and run["dirty"]
        assert run["entity_count"] == 1
        assert run["observed_revision"] == committed["observed_revision"]
        assert call("scene.history", {}) == history
        saved = call("scene.save", {"expected_revision": committed["observed_revision"]})
        assert saved["publication"]["published"] and saved["current_scene_clean"]
        call("project.close", {})
        call("project.open", {"path": "Example.sharkproject"})
        row = call("scene.inspect", {"entity_id": entity_id, "fields": ["name"]})
        assert row["entities"][0]["name"] == "Spawn point"
        call("project.close", {})
        process.stdin.close()
        assert process.wait(timeout=10) == 0
print("Created, previewed, edited, replayed, ran, saved and reopened the project.")
```

This independent import example writes a new temporary byte source, registers its
native metadata, explicitly refreshes the inventory, drains one job and inspects
its validated cache descriptor. It leaves saved project assets untouched:

```python
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time
import uuid

author = Path("build/windows-debug/bin/SharkAuthor.exe").resolve()
Path("out").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="terminal-import-", dir="out") as temporary:
    root = Path(temporary).resolve()
    (root / "scenes").mkdir()
    (root / "assets").mkdir()
    source = b"Shark terminal import example\n"
    with (root / "assets/example.bin").open("xb") as output:
        output.write(source)
    project_id, asset_id = str(uuid.uuid4()), str(uuid.uuid4())
    with subprocess.Popen(
        [str(author), "--stdio", "--root", str(root)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        text=True, encoding="utf-8",
    ) as process:
        sequence = 0

        def call(method, params):
            global sequence
            sequence += 1
            request = {"jsonrpc": "2.0", "protocol_version": 1,
                       "id": sequence, "method": method, "params": params}
            process.stdin.write(json.dumps(request) + "\n")
            process.stdin.flush()
            line = process.stdout.readline()
            if not line:
                raise RuntimeError("SharkAuthor exited before its response")
            reply = json.loads(line)
            assert reply["id"] == sequence
            if "error" in reply:
                raise RuntimeError(reply["error"])
            return reply["result"]

        created = call("project.create", {
            "path": "Example.sharkproject", "scene_path": "scenes/Main.sharkscene",
            "name": "Terminal import", "project_id": project_id,
            "world_id": str(uuid.uuid4()), "scene_id": str(uuid.uuid4()),
        })
        assert created["success"], created
        descriptors = call("asset.importers", {})["importers"]
        assert any(item["id"] == "shark.bytes" for item in descriptors)
        metadata = call("asset.metadata.write", {
            "path": "assets/example.bin.sharkasset", "expected_revision": None,
            "metadata": {
                "format": "shark.asset", "schema_version": 1,
                "project_id": project_id, "asset_id": asset_id, "kind": "bytes",
                "origin": "Original temporary example bytes", "license": "MIT",
                "importer": "shark.bytes", "audience": "shared",
                "settings": {"transform": "identity"}, "dependencies": [],
            },
        })
        assert metadata["publication"]["published"]
        call("asset.metadata.release", {"revision": metadata["revision"]})
        catalog = call("asset.catalog.status", {})["revision"]
        catalog = call("asset.catalog.refresh", {
            "sidecars": ["assets/example.bin.sharkasset"],
            "expected_revision": catalog,
        })["revision"]
        cache = call("asset.cache.status", {})["revision"]
        target = {"id": "example-client", "side": "client"}
        admitted = call("asset.jobs.submit", {
            "asset_id": asset_id, "target": target,
            "expected_catalog": catalog, "expected_cache": cache,
        })
        deadline = time.monotonic() + 10
        while True:
            statuses = call("asset.jobs.drain", {})["jobs"]
            job = next(item for item in statuses if item["job_id"] == admitted["job_id"])
            if job["state"] in ("succeeded", "failed", "cancelled"):
                break
            if time.monotonic() >= deadline:
                call("asset.jobs.cancel", {"job_id": admitted["job_id"]})
                call("asset.jobs.close", {})
                raise TimeoutError("Import did not reach a terminal outcome")
        assert job["state"] == "succeeded", job
        assert job["error"] is None and job["summary"]["root"] == asset_id
        assert job["summary"]["imported"] == 1
        assert job["commit"]["publication"]["published"]
        cache = job["commit"]["revision"]
        fresh = call("asset.cache.query", {
            "asset_id": asset_id, "target": target,
            "expected_catalog": catalog, "expected_cache": cache,
        })
        assert fresh["state"] == "fresh"
        artifact = call("asset.cache.inspect", {
            "key": fresh["last_valid_key"], "expected_revision": cache,
        })
        assert artifact["source_digest"] == hashlib.sha256(source).hexdigest()
        assert artifact["payload_bytes"] == len(source) + 36
        assert artifact["key"] == job["summary"]["root_key"]
        assert call("asset.jobs.take", {"job_id": admitted["job_id"]}) == job
        assert call("asset.jobs.close", {})["jobs"] == []
        assert not call("project.status", {})["dirty"]
        call("project.close", {})
        process.stdin.close()
        assert process.wait(timeout=10) == 0
print("Imported temporary source, inspected its cache and acknowledged the job.")
```

Use the relevant binary path on WSL. Production supervisors should retain stderr,
bound each pipe wait and terminate/drain a failed child. Requests dispatch serially
on the owner thread while import computation uses the existing private worker.

For artifact work, start the configured host with the same ownership options.
These example argument paths name your existing build/tool inputs; replace the
shader files and GPU UUID with the validated local values:

```text
build/windows-debug/bin/SharkAuthorClient.exe --stdio --root PROJECT_ROOT --project Project.sharkproject
  --export-python .tools/venv/Scripts/python.exe --export-tool tools/package_runtime.py
  --capture-vertex-shader SCENE_VERTEX_SPV --capture-pixel-shader SCENE_PIXEL_SPV
  --capture-device EXACT_GPU_UUID --capture-validation required
```

With that host open, a request builder can reuse the editing example's
`call(method, params)` helper. This fragment assumes an existing startup Camera
and already-imported fresh client content; the uppercase values are caller inputs:

```python
# Fragment for an already configured live host, not an independent script.
common = {
    "expected_revision": call("project.status", {})["revision"],
    "expected_catalog": call("asset.catalog.status", {})["revision"],
    "expected_cache": call("asset.cache.status", {})["revision"],
    "camera_id": CAMERA_UUID,
    "target": {"id": CONTENT_TARGET, "side": "client"},
}
capture = call("scene.capture", {
    **common, "output": NEW_CAPTURE_DIRECTORY, "width": 160, "height": 120,
})
assert capture["published"] and capture["succeeded"]
export = call("scene.export", {
    **common, "output": NEW_PACKAGE_DIRECTORY,
    "build_dir": RUNTIME_ONLY_RELEASE_BUILD, "crt_dir": EXACT_RELEASE_CRT_DIRECTORY,
    "shaders": SCENE_SHADER_ARTIFACT_DIRECTORY, "notices": CONTENT_NOTICES,
})
assert export["published"] and export["status"] == "PASS"
```

Do not run this fragment against the saved FeatureWorld root with guessed inputs.
Use deliberate scratch projects and fresh output directories, preserve complete
asset notices, and inspect a failed/lost operation's original output before retry.
The complete example below connects these operations to a new authored project.

## Complete terminal example

[examples/terminal_authoring.py](../examples/terminal_authoring.py) is a runnable
Python standard-library example. It uses the public redirected stdio protocol;
it needs no engine Python binding, test module, editor, model provider or MCP
client. From the repository root, after building the CPU host:

```powershell
New-Item -ItemType Directory -Force out | Out-Null
& ./tools/windows_dev.ps1 -CommandArguments @('cmake','--build','--preset','tools-debug','--target','SharkAuthor','--parallel','2')
.tools/venv/Scripts/python.exe examples/terminal_authoring.py --author build/tools-debug/bin/SharkAuthor.exe --output out/terminal-example
```

Use a fresh output name for each invocation. On WSL, use the built `SharkAuthor`
path, `python3` and a writable output parent. All argument paths are resolved
before launching the host, so an invocation can also start outside the checkout.
Fixed UUIDs belong to this repeatable sample; real new content should generate
fresh durable IDs.

The example creates `project/Terminal.sharkproject` and its startup scene, copies
the repository's licensed offline glTF triangle and external buffer, and writes
typed sidecars through the protocol. It imports the dependency closure, edits the
copied glTF's generator and import settings, then explicitly refreshes/reimports.
Direct filesystem writes ingest/edit external glTF, buffer and license inputs;
typed project, scene and metadata writes all use the retained owner. Original
fixtures are unchanged.

It previews/commits a shared Camera/MeshRenderer batch, saves and reopens in a new
process, adds an unsaved Unicode name edit, runs three static integrity ticks with
seed 1337, and optionally captures/exports that dirty state. It checks unchanged
history, retry state and source bytes before explicitly saving the last edit.
A third process reopens the clean saved project and releases ownership. A human
or another terminal owner can continue that project; assignment 16 supplies the
human editor UI.

`summary.json` retains the import outcomes, source identities, save/reopen state,
run report, optional artifact results and final handoff. Standard output contains
one JSON completion, and failure returns nonzero with a diagnostic. Pipe reads,
writes and process waits are bounded. A failed invocation retains its directory
for inspection; it never silently resumes or replaces an existing output.

To add a CPU-only Windows package export, pass these flags to the same example
with a new output directory:

```text
--export-python .tools/venv/Scripts/python.exe
--export-tool tools/package_runtime.py
--runtime-build build/runtime-only
--crt-dir "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Redist/MSVC/14.51.36231/x64/Microsoft.VC145.CRT"
--shaders out/14b-work/scene-shaders
```

Run export through `tools/windows_dev.ps1` to supply its MSVC build-inspection
environment. Those paths name this PC's retained inputs; use the current protected
build, exact CRT and verified shader generation described by the
[export guide](standalone-export.md). The example supplies the complete copied MIT
notice and publishes `package/`; the source importer and terminal authoring host
are absent from that package.

For optional capture, select `SharkAuthorClient` and supply all three flags
`--capture-vertex-shader`, `--capture-pixel-shader` and `--capture-device` with
explicit validated scene SPIR-V files and GPU UUID. `--capture-validation required`
is the default and needs the configured validation layer environment. An explicit
`--capture-validation off` is available for ordinary preview; it is not qualifying
render evidence. Capture writes `capture/color.rgba`, `capture/depth.f32` and the
artifact report. The existing artifact fixture's `--connected` mode invokes this
same example with the locked validation environment on both selected GPUs and
compares against the independent imported-triangle reference. It does not replace
the public example or introduce another editing API.
