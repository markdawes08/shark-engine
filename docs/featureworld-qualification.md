# FeatureWorld qualification

Assignment 10 closes through the [four checkpoints](roadmap/featureworld-plan.md).
The CPU [runner](scene-runner.md), saved [FeatureWorld](featureworld.md) and
explicit [CPU/GPU supervisor](scene-execution.md) keep their existing contracts.
This gate adds a development test fixture, not a new authoring API or renderer.

`shark.tools.scene-qualification` is a CPU-only integration family with a
90-second timeout. Its standard-library Python supervisor obtains the actual
native case catalog, device-free GPU catalog and current CTest inventory. It
compares all manifest mappings with real test registrations or explicitly
identified declarations for another profile. A declaration is not a passing
execution. Component/command mappings must have registered tests in the current
Tools profile; their independently reviewed exercise inventory contains twenty-four
scene surfaces and one external GPU command after 11a. 11b extends the independent
inventory to 34 scene surfaces and one external GPU command, adding ten actual
import/cache operations in the seventh native FeatureWorld case. 11c extends it
to 40 CPU scene surfaces and one external GPU command with six job operations in
the eighth native case. It exercises real private-worker imports, pending-work
deduplication, progress, owner-side publication, cancellation before/after commit,
retained outcomes, explicit retry and closed admission. Lua remains empty until
real bindings land. New surfaces extend their actual cases, independent inventory
and manifest together.

11d adds a ninth connected asset-pipeline case while retaining those forty CPU
surfaces. It carries one saved scene reference through asynchronous imports,
coordinated moves, a real worker gate with owner source/settings edits, stale
completion rejection, retry, cancellation, missing/private dependencies and
reopen/maintenance. It adds no new authoring API or decoder.

The fixture runs all nine saved-world cases through the public supervisor, then
the intentional failing assertion through the same executable/result path. It
compares stdout with create-only published results, native results and every
artifact's bytes/hash. Failure remains exit 1 with the original case, phase,
tick and entity context; CPU results report no renderer initialization.
Negative controls reject missing surfaces/tests/fixtures, stale registrations,
catalog drift, unavailable profile tests, substituted statuses and corrupt
artifact bytes. Original workloads, bounds, seeds and timeouts remain intact.
The versioned source stays untouched: execution uses a writable temporary copy
for ProjectFiles ownership. CTest discovery reads the actual generated test tree
through a private metadata directory so it cannot overwrite the enclosing run's
logs or checkpoint files.

Run from the repository root in the documented developer environment:

```text
ctest --preset windows-debug --show-only
ctest --preset windows-debug -R ^shark.tools.scene-qualification$ --output-on-failure
```

The CTest receipt is `tests/scene-qualification.json` under that build directory.
It embeds successful and failing execution snapshots before managed temporary
cleanup. It qualifies no GPU. Both actual Windows GPUs must separately pass the
existing explicit offscreen/window cases with required validation and unchanged
numeric oracles. Optional assignment-02 probe CLI declarations retain their
separate historical qualification; they are not silently counted as current
CTest executions.

The final gate refreshes all seven full Windows/WSL CPU/sanitizer/Server profiles,
standalone Tools, public examples and protected packages against frozen inputs.
Record inventories, complete JUnit, injected failure context, exact helper and
source hashes, actual sanitizer objects and native GPU receipts. Validate all
current component/command test IDs against the union of passing executions.
Preserve incomplete attempts and report metadata-only closure changes separately
from implementation inputs. Reuse current build/cache/userspace directories and
remove successful temporary deployments. This establishes local development
coverage; MMORPG capacity, MMOWorld and authored scene rendering retain their
later owners.
