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
