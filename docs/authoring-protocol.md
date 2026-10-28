# Terminal authoring protocol

Assignment 15a adds `SharkAuthor`, a CPU terminal adapter over the existing
[ProjectFiles](project-files.md), [AuthoredScene](authored-scenes.md),
[component schemas and bounded queries](scene-inspection.md). It opens an existing
project and inspects its active authored scene without an editor, display, GPU,
network listener, model provider or MCP client. It does not edit or save source,
run imports/jobs, render captures, or export through this protocol yet.

[ADR 0039](architecture/0039-terminal-authoring-protocol.md) defines this bounded
profile. The public C++ owner is
[authoring_protocol.hpp](../engine_tools/include/shark/tools/authoring_protocol.hpp);
JSON parsing and wire conversion remain private to EngineTools. Runtime and Server
gain no Tools or JSON dependency. These are local development operations, separate
from gameplay requests and live-world administration.

## Launch and framing

```text
SharkAuthor --stdio|--request [--root DIRECTORY] [--project RELATIVE_PATH]
SharkAuthor --help|--version
```

Select exactly one transport mode. `--root` fixes an existing project root for the
process; a request cannot change it. Without a root, discovery/schema requests and
closed-project status are available, but `project.open` fails. `--project` requires
`--root` and opens that relative source project before reading requests. Startup
open selects the project's declared startup scene. A failed startup open writes
its structured error to stderr and exits 2; success produces no startup response.
Use `project.open` to select another declared scene explicitly.

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
| Decoded JSON string | 4,096 bytes, with smaller field-specific limits below |
| Outstanding query cursors | 32 per open project |

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
still returns 1.

## Request and response profile

Discovery identifies each method's `execution_context: "cpu_owner_thread"`,
`requires_open_project` and `requires_project_root` prerequisites, as well as its
parameter/result schemas and example request. `scene.*` methods require the open
owner; `project.open` requires the root fixed at host creation.

Shark uses the [JSON-RPC 2.0 specification](https://www.jsonrpc.org/specification)
request/response and notification conventions with an explicit bounded profile:
named parameters only, one request object per frame, and no RPC batch arrays.
This batch restriction is about RPC envelopes, not the existing native
[atomic authoring batches](authoring-transactions.md); those mutations are not
exposed by 15a.

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
flags. It advertises exactly these eight methods:

| Method | Parameters | Result |
| --- | --- | --- |
| `protocol.discover` | `{}` | Version, transport, limits, capabilities, methods and components |
| `schema.components` | `{}` | `{ "components": [...] }` |
| `project.open` | Required `path`; optional `scene_id` | Open-project status |
| `project.status` | `{}` | Current status, or `{ "open": false }` |
| `project.close` | `{}` | `{ "open": false }` after successful close; idempotent when closed |
| `scene.query` | Selection/filter/fields/bounds/revision below | Bounded query page |
| `scene.inspect` | Required `entity_id`; optional `fields`, `expected_revision` | One-row page with no continuation |
| `scene.continue` | Required `cursor` returned by an earlier page | Next bounded page, with the earlier selection and revision |

The capabilities are `cpu_authoring: true`, `device_capture: false` and
`mutations: false`. There is no advertised editor, mutation, export or GPU
operation whose implementation is deferred.

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

Explicit close rejects dirty state or pending mutations. Successful close releases
the scene and all cursor state before the project lock. Process destruction also
releases ownership; it does not save source. This read-only increment creates no
unsaved edits. Later mutations must retain the existing dirty/history/retry
semantics and define their own exit/reconciliation behavior.

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

Use the relevant binary path on WSL. Production supervisors should retain stderr,
bound their waits and terminate/drain a failed child. The executable processes
requests synchronously; this initial contract introduces no asynchronous job API.

Later 15 checkpoints will adapt shared edits/history/retries and existing tools.
An export adapter must use the active owner or reject a conflicting open project;
it cannot launch an independent source-reloading export worker behind an active
authoring session and claim to preserve unsaved changes.
