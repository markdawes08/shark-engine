# Core contracts introduced in 03a

These CPU-only C++20 contracts live in EngineCore and are shared by the
bootstrap executables. The [checkpoint plan](roadmap/core-plan.md) leaves owned
application lifecycle to 03b and scheduling/queues to 03c. Source project
schemas, runtime entity handles, networking and authoring protocol remain with
their existing owners.

## Results and diagnostics

`shark/error.hpp` defines `Result<T>`, `Result<void>`, `Error` and `ErrorContext`.
Values and all error strings are owned; callers can retain failures after input
buffers disappear. Test the result before accessing its value or error. Access
to the wrong alternative throws `std::bad_variant_access` in Debug and Release;
this is a programmer error, not an expected input failure. Standard assertions
are for internal invariants only. Validation must still return errors in Release.
Allocation failure is not promised to be recoverable by these allocating APIs.

Stable lowercase codes are distinct from human-readable messages:

| Category | Codes |
| --- | --- |
| `validation` | `invalid_argument`, `invalid_configuration`, `invalid_id`, `invalid_path`, `path_outside_root` |
| `unavailable` | `entropy_unavailable`, `capability_unavailable` |
| `conflict` | `conflict` |
| `execution` | `io_failure`, `execution_failed` |

Context contains operation, request ID, path, entity ID and asset ID strings.
Services preserve supplied correlation context; domain services will validate
their respective IDs when those domains exist. Neither request IDs nor durable
IDs grant authorization. Parsers provide a specific operation without echoing
unbounded command-line text.

`shark/log.hpp` provides an explicitly injected `Logger` borrowing an
`std::ostream`. Its owner keeps the sink alive and serializes all calls, including
other writers sharing the sink. A successful write emits one UTF-8 JSON object
followed by a newline, with `level`, `code`, `category`, `message`, `operation`
and nonempty optional context fields. Controls and quotes are escaped.
Each string is limited to 1024 UTF-8 bytes except `path`, which allows 4096;
records stay below 64 KiB. Malformed or oversized fields fail before output,
without truncation. A write/flush failure returns `io_failure`, can leave a
partial record, and is never recursively logged or retried. Callers handle that
failure separately. This diagnostic framing is not a versioned authoring protocol.

Logger, filesystem resolution and ID generation may allocate, block or perform
I/O. They run at ordinary application/service boundaries, never in signal
handlers, audio callbacks or other real-time/allocation-free callbacks. Current
apps log only on the main thread. The Server signal handler still only assigns
`sig_atomic_t`; no callback queue or cross-thread logger is claimed by 03a.

## Configuration and executable behavior

`parse_bootstrap_arguments` and `parse_server_arguments` now return owned results.
Their old `invalid` enum alternatives and borrowed Server error field are removed;
all in-repository consumers are migrated. No saved data format changes.
`validate_server_config` also validates a programmatically constructed candidate
before Server startup: `run_for` is absent or 1..600000 milliseconds. Text accepts
one to six ASCII digits (leading zeros allowed), with no signs or whitespace.
Future configuration sources must use the same candidate validation before
activation; 03a introduces no configuration file format or speculative flags.

Help/version and Server readiness/completion stdout remain unchanged. Invalid
arguments write a JSON diagnostic as the first stderr line, followed by readable
usage, and exit 2. Startup/output failures exit 1; failed diagnostic output also
exits 1 without recursion. Successful informational/timed commands exit 0.
There is still no graphical editor or game-authoring CLI.

## Stable identity

`shark/stable_id.hpp` defines a validated `StableId` primitive and explicit
`TypedId<Domain>` wrappers. Owning systems introduce their tags later; authored
and durable MMO identifiers must not become interchangeable aliases. IDs are
128-bit UUIDv4 values, serialized as exactly 36 lowercase hyphenated characters.
Parsing rejects nil, wrong version/variant, uppercase and alternate encodings.
No implicit default/nil identity is available.

Production generation uses Windows `BCryptGenRandom` or Linux `getrandom`, with
an owned failure if entropy is unavailable. `IdEntropy` injection provides
deterministic tests and failure fixtures without a production test seed or
hidden global generator. Duplicate detection belongs to the owning namespace;
a UUID generator is not a uniqueness database. IDs are not hashes, credentials,
session replicas or checked runtime handles.

## Project-relative paths

`shark/project_path.hpp` separates a portable `ProjectPath` from a native
`ProjectRoot`. References are valid UTF-8 with forward slashes, at most 4096
bytes overall and 255 per component. Spaces and Unicode are valid. Reject
absolute/drive/UNC paths, backslashes, empty/dot/traversal components, NUL/control
characters in the C0 range, Windows device/ADS names and trailing dots/spaces on every platform.
Do not normalize invalid input into a different accepted reference. Case and
Unicode spelling are retained; this is not a file-identity or normalization API.

The root must already be a directory and is stored as an absolute canonical
native path, independent of later working-directory changes. Native roots reject
embedded NUL and invalid text encoding before OS resolution. Resolving a
reference checks existing ancestors, rejects escaping symlinks/junctions,
dangling links and non-directory ancestors, and permits missing descendants for
later creation. Canonical component comparisons are exact: ambiguous case aliases
can fail conservatively on Windows. Resolution never creates files.

Automatically recorded diagnostic paths are bounded valid UTF-8; malformed or
oversized raw input is omitted, retaining the stable error code and message.
Caller-supplied context remains owned and must meet the logger's contract.
Resolution is a validation step, not a filesystem sandbox or a reserved handle:
concurrent replacement between checking and opening remains possible. Later
project services must coordinate writes and validate at their actual I/O boundary.

See the [03a handoff](handoffs/03a-core-contracts.md) for executed tests and limits.
