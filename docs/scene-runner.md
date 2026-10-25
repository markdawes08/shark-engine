# Deterministic scene runner

The [CPU/GPU test supervisor](scene-execution.md) adds explicit GPU fixture
execution in 10c while this native worker remains CPU-only. Its target dependency
boundary and native version-1 result/catalog contracts remain unchanged.

Checkpoint 10a implements the shared CPU foundation described in
[ADR 0024](architecture/0024-headless-scene-runner.md) and the
[four-checkpoint plan](roadmap/featureworld-plan.md). `SceneRunner` is built with
`SHARK_BUILD_TOOLS=ON`, including the display-free `tools-debug` configuration.
It uses no Client, renderer, editor, storage or test-framework library.

## Running a foundation case

From the repository root, using the existing Windows developer wrapper:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset tools-debug --target SceneRunner --parallel 2
build/tools-debug/bin/SceneRunner.exe --mode cpu --case foundation --seed 1337 --ticks 7 --hz 59 --input 0:offset:5 --input 3:offset:-7 --run-id sample --revision my-revision
```

Add `--output out/run-result.json` to publish a result file; its parent must exist
and the file must not exist. The published UTF-8 JSON equals stdout and includes
its artifact reference. A unique sibling stage and atomic no-replace hard link
prevent overwriting another file. Unsupported hard links fail explicitly.
Staging cleanup is scoped to the created file/directory. This does not establish
power-loss durability. Keep generated results in ignored `out/` or `build/`.

`--help` and `--version` are readable discovery paths. Normal execution emits
only one JSON record on stdout. There are no prompts or required stdin/TTY.
Flags may appear once, except repeated `--input`. Malformed options, numbers,
unknown cases/actions and unsupported modes fail with structured diagnostics.
The `foundation` case creates one entity through AuthoredScene commands, applies
seeded transforms and one-shot `offset` inputs, then checks actual scene values.
`--case intentional-failure` deliberately fails at tick zero with its entity ID,
case ID, phase and assertion text. It is an expected nonzero test fixture.

| Exit | Meaning |
| --- | --- |
| 0 | All selected cases passed and requested output was published |
| 1 | A case assertion or callback failed |
| 2 | Configuration, execution infrastructure, publication or stdout failed |

The public C++ service also reports cooperative `CANCELLED` and unexecuted
`NOT_RUN` cases. The current executable has no cancellation adapter. A failed case
dominates aggregate cancellation; a cancelled run cannot pass. The current GPU
request returns `capability_unavailable`, exit 2 and `mode: none`; there is no CPU
fallback. GPU execution/capture arrives in 10c. The [saved FeatureWorld](featureworld.md)
adds explicit project loading and five current-surface native cases in10b;
scene rendering retains assignment 13.

## Configuration and execution

| Value | Fixed bound/policy |
| --- | --- |
| Cases | 1..32 unique ASCII IDs, 1..64 bytes |
| Ticks per case | 1..10,000, zero-based indices |
| Rate | 1..1,000 integral Hz, default 60; exact timestep 1/rate seconds |
| Seed | Unsigned 64-bit integer, default 1337 |
| RNG | SplitMix64-v1, reset to the same seed for each case; at most 65,536 draws/case |
| Assertions | At most 16,384/case; a passed case requires at least one |
| Inputs | At most 4,096 ordered events, tick less than run ticks; action ID 1..64 ASCII bytes; signed 32-bit value |
| Run ID/revision | 1..64 ASCII letters, digits, dot, underscore or hyphen |
| Diagnostics | UTF-8; message/ordinary context at most 1,024 bytes; path at most 4,096 |
| Result | UTF-8 JSON with newline, at most 1 MiB |

The Core scheduler is advanced to `ceil((tick+1)*1e9/rate)` nanoseconds. Each pass
must execute exactly one tick with no discarded/pending whole ticks. Test tick
start is `tick/rate` seconds. No wall-clock sleeps or second scheduling policy are
introduced. Equal-tick input order is preserved and each event is exposed only
on that tick; cases explicitly own any desired held state. This is local test
input, not raw device input or a player message.

All source documents and case/configuration bounds validate before any callback.
Each case receives a fresh owner, while setup/tick/finish share one owner and its
command/history/retry behavior. Use `context.owner().execute` and check both Result
levels; public Core scene access stays const. The context borrow ends with the
callback/run. Context assertions and random access reject foreign threads.

`require` records the first failure permanently, even if its returned Result is
ignored. Subsequent ticks/finish are skipped for that case; later cases can run.
Successful callbacks acknowledge completed ticks. A callback that mutates then
fails is not retried and its tick is not counted as completed. Before/after local
revisions describe observed changes, not rollback. Failed outer Results can occur
after callbacks executed; do not blindly repeat external callback effects.

Cancellation is observed before cases and between callbacks; already completed
work remains recorded. Native callbacks are trusted engine code, must bound their
work and must join any workers before returning. An external supervisor owns
hard deadlines; cooperative stop does not preempt a hung callback.

## Result version 1

`format: shark.scene-run` and integer `schema_version: 1` identify this independent
format. Complete reports include `status`, `mode: cpu`,
`renderer_initialized: false`, engine/source versions, seed, rate, tick budget,
RNG identity, caller run ID/revision, project/scene IDs, ordered inputs, cases and
artifact references. Failure records contain kind, phase, optional tick and owned
Core error context. ERROR envelopes describe an unexecuted/unsuccessfully
completed request with `mode: none`. Always check both process exit and status.

Local revision instances intentionally vary as scenes are created. They are
diagnostic incarnation context, never durable IDs. Reproducibility comparisons
exclude these process-local values and retain the committed revision counters,
explicit inputs and semantic assertions. CPU mode describes this runner path;
it cannot qualify renderer initialization or MMO distributed ownership.
Valid activation can replace a case's scene incarnation while retaining its
AuthoredScene owner. Before/after revision counters are monotonic only within
the same incarnation; a replacement may reset the counter. Project/scene IDs
at report scope identify the initial source baseline.
Consumers must reject unsupported versions; no implicit migration is promised.

The public API example compiles with only public Shark/GLM headers:

```cpp
#include <shark/tools/scene_runner.hpp>
#include <array>

shark::Result<void> inspect_runner_tick(shark::tools::SceneRunContext& context,
                                      const shark::tools::SceneRunTick&) {
    auto capture = context.owner().capture();
    if (!capture) return capture.error();
    return context.require(capture.value().revision.scene_instance != 0,
                           "Scene has a live owner");
}

shark::Result<void> verify_scene(const shark::tools::ProjectDocument& project,
                                 const shark::tools::SceneDocument& scene) {
    using namespace shark;
    using namespace shark::tools;
    SceneRunConfig config;
    config.ticks = 2;
    config.revision = "my-revision";
    SceneRunCase test{.id = "scene-readable"};
    test.tick = inspect_runner_tick;
    const std::array cases{test};
    auto run = run_scene_cases(project, scene, config, cases);
    if (!run) return run.error();
    if (run.value().status != SceneRunStatus::passed)
        return Error{ErrorCode::execution_failed, "Scene run did not pass",
                     {.operation = "example.verify_scene"}};
    return {};
}
```

`shark.tools.scene-runner` uses Catch seed 1337 and a 60-second timeout.
`shark.tools.scene-runner-process` has a 60-second timeout and independently
parses real executable JSON, verifies nonzero intentional failure, deterministic
context, bounded invalid inputs and Unicode/create-only publication. Temporary
deployments/results are managed under the active build and removed on success.
