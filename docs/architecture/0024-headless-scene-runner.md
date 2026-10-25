# ADR 0024: Deterministic headless scene cases

Date: 2026-10-25. Status: accepted for checkpoint 10a.

The runner is a CPU EngineTools service with a separate `SceneRunner` executable.
It links Core and the existing private source codec, with no Client, storage,
editor or Catch2 dependency. Runtime/Server do not acquire it. Extend the existing
dependency checker to audit this executable as an authoring root.

One trusted native case runs setup, fixed ticks and finish against one
AuthoredScene on the calling thread. The next case starts from the same validated
source values in a fresh owner. All edits use the existing command service;
there is no mutable registry port or second scheduler. Core's ServerScheduler
admits exactly one tick at each synthetic rational deadline. Its use here grants
no live-server authority; the default test rate is offline 60 Hz.

Case assertions are sticky and bounded. Returning success after a failed
assertion still fails the case. Callback failures/exceptions stop that case,
preserve diagnostic progress, and are never retried. Stop tokens are cooperative
between callbacks. Trusted callbacks must return in bounded time; the enclosing
process supervisor owns a hard deadline. This is not an arbitrary script sandbox
or a general job framework.

The explicit seed drives SplitMix64-v1 with per-case reset and fixed test vectors.
It supplies test data only; production IDs/credentials retain OS entropy. Input
events have ordered integer ticks, bounded action IDs and integer values. No
wall-clock timing, focus, OS events or rendering controls enter a CPU run.

Use the independently versioned `shark.scene-run` result format. Include the
engine/source format versions, caller revision/run ID, seed, rate, inputs, case
results, assertion counts, scene IDs and before/after local revisions. Local scene
incarnations are diagnostic values rather than persistent identity or a promise
of bit-identical results across processes. Compare semantic results explicitly.
This does not claim cross-platform physics determinism.

CPU result paths are create-only. A complete bounded JSON record is closed in a
unique sibling stage, then a same-filesystem hard link publishes it without
replacing an existing path. Stage cleanup uses only the owned file/directory.
Unsupported filesystems and I/O failures report errors. This is result publication,
not source saving or M03 database durability. Stdout write failure remains nonzero
even if the result artifact was already published.

10b adds the saved FeatureWorld and executable coverage catalog. 10c adds actual
GPU execution; 10a rejects GPU requests explicitly. The runner is a local testing
tool, not the assignment-15 authoring CLI, player protocol or operational console.

## 10b amendment: saved current-surface cases

FeatureWorld is a versioned source project under `assets/FeatureWorld`, using the
existing source schema 1. Five trusted native CPU cases are shared EngineTools
code, discovered through `feature_world_case_ids` and the independently versioned
`shark.scene-cases` catalog (version 1). The CLI accepts an explicit project file,
reads its startup scene through ProjectFiles and holds that cooperating owner
for the run. It never infers a workspace from an ignored roadmap or build path.

Each case checks the independently specified saved baseline before exercising
current components/services. Ordinary edits, batches, history and retries use
the same AuthoredScene supplied by the runner. Persistence saves/reopens/reloads
that owner through a unique child of an explicit scratch parent. Input files are
never save destinations. Cleanup removes only owned, known scratch names; any
unexpected remnant remains visible. A source drift is a real scene assertion
failure; malformed/unsupported source rejects before case dispatch.

The coverage manifest adds catalog paths, executable case IDs and per-surface
case references. Independent process tests compare discovery to that catalog,
run all cases, check semantic revisions and preserve original source bytes.
Existing isolated fixtures continue to qualify bounds, faults, actual network,
durable storage and platform behavior. A saved scene cannot replace those tests.
No source/result schema change or migration is needed. Catalog consumers reject
unsupported versions; there is no implicit catalog migration. FeatureWorld has
no input actions in10b; the foundation case retains its explicit offset inputs.

## 10c amendment: supervised GPU fixtures

`tools/scene_runner.py` is the developer test entry point for explicit CPU/GPU
execution. The native SceneRunner remains a CPU worker; its target dependencies
and version-1 result/catalog contracts remain unchanged. GPU requests use the
existing OffscreenSmoke/CubeSmoke executables and their existing supervisors,
source identities, validation-tool checks and independent readback validators.
This reuses the owned Client renderer and process supervisor without adding
Client dependencies to EngineTools or a second renderer/process framework.

The new, independently versioned `shark.scene-execution` envelope (version 1)
identifies requested mode/case, caller context, actual input hashes, worker
completion and bounded artifact references. CPU retains the complete native
result. GPU PASS requires an exact UUID, required Vulkan core/synchronization and
NVRHI validation, current native source/executable/shader identities and verified
raw readbacks at unchanged tolerances. A wrapper's PASS label is insufficient:
the supervisor rechecks the native stream and actual files. No mode, adapter,
validation or shader fallback is permitted. GPU controls describe fixed native
workloads; CPU seed/tick/input/project arguments are rejected in that mode.

`renderer_initialized` is false for CPU and pre-dispatch capability rejection,
true only after verified GPU completion, and null when failed/interrupted GPU
work may have initialized a device. Incomplete evidence cannot assert that no
device was touched. Unavailable capabilities remain NOT_RUN with nonzero exit.
Cancellation is cooperative at supervised process boundaries; work may already
have executed. Native callbacks remain trusted; this is not a hostile executable
sandbox or a guarantee against machine loss/hard termination.

Evidence roots are explicit, fresh directories with existing parents. Complete
UTF-8 result publication is atomic and create-only; artifact traversal rejects
links/junctions and enforces fixed file/path/byte bounds. Failed evidence remains
inspectable. Consumers reject unsupported envelope/catalog versions; no implicit
migration is provided. Existing source, native CPU result and gameplay protocols
do not change. GPU cases qualify the fixed graphics fixtures, while authored
scene rendering, general terminal authoring and the full REQ-10 audit retain
their later owners.
