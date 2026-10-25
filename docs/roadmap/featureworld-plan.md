# 10: FeatureWorld and scene-runner checkpoints

Date: 2026-10-25. Base: `7f5353a` on initially clean `main`.
Owner: REQ-10. Full REQ-09 is complete. The user authorized 10a after reviewing
this four-checkpoint split. Full REQ-10 remains NOT_RUN until 10d.

**10a is PASS under the current-machine scope.** The
[handoff](../handoffs/10a-headless-runner.md) and
[receipt](../coverage/evidence/10a-headless-runner.json) record the current Tools,
regression, public API, sanitizer and protected package evidence, including the
explicit Linux repository-environment retry. **10b is also PASS under the
current-machine scope:** its [handoff](../handoffs/10b-featureworld.md) and
[receipt](../coverage/evidence/10b-featureworld.json) record saved source, five
native cases, all current component/command mappings and 311 passing profile
results. **Next: 10c.**

Follow [ADR 0007](../architecture/0007-mmorpg-architecture.md),
[ADR 0008](../architecture/0008-human-agent-authoring.md), the
[MMORPG plan](mmorpg-plan.md), the [human/agent plan](human-agent-plan.md), and
[ADR 0024](../architecture/0024-headless-scene-runner.md).

## 10a - Deterministic CPU runner foundation

Implement an engine-owned, noninteractive CPU executable and shared EngineTools
service. Reuse AuthoredScene, validated commands and Core's fixed-rate scheduler.
Provide bounded seed/rate/tick/input configuration, trusted native case callbacks,
sticky assertions, explicit callback errors/cancellation, scene/revision context,
versioned result JSON and create-only result publication. Include a real small
in-memory foundation case and an intentional failing scene assertion. They are
runner fixtures; the saved FeatureWorld and coverage catalog belong to 10b.

Gate: deterministic rational ticks/input order and RNG vectors, fresh case
ownership, shared edits within a case, useful failure context and nonzero CLI
failure, malformed/bounded inputs, callback faults, cancellation, result
consistency, Unicode paths, no overwrites, and CPU-only target/package boundaries.
Preserve `shark.tools.scene-runner` and `shark.tools.scene-runner-process`, both
60 seconds; the Catch family uses seed 1337. The [runner contract](../scene-runner.md)
records fixed limits. CPU results explicitly report no renderer initialization.

## 10b - Saved FeatureWorld and current surface coverage

Completed under the current-machine scope; retain its source IDs, five native
cases, 23 process checks, both 60-second families and Catch seed 1337.

Create the small versioned saved project/scene and real cases for the currently
supported Name/Transform/hierarchy, source persistence and authoring commands,
batches, history and retained retries. Reuse the 10a runner and owners. Bind
manifest entries to executable case IDs and existing behavior tests. Keep the
Lua list empty until real bindings land. Preserve justified isolated fixtures for
network, storage, process and platform behavior that cannot be proven by a scene.

Gate: all current component/command mappings resolve to real passing exercises
or explicit external-system fixtures; source IDs, revisions and expected state
survive repeat runs. No future feature is represented as implemented coverage.

## 10c - Explicit GPU execution

Connect existing graphical fixtures through an explicit GPU mode with device,
capability and artifact context. Qualify the current Windows NVIDIA and Intel
GPUs. CPU/WSL operation stays independent of Client/graphics initialization.
Unavailable capabilities remain explicit and cannot silently select CPU or a
different backend. Scene rendering remains assignment 13; existing cube/device
fixtures are identified by their actual scope.

## 10d - Coverage audit and integrated qualification

Audit missing/stale coverage mappings and prove intentional assertion failure
through the complete executable/result/artifact path. Refresh the relevant
Windows/WSL, sanitizer and protected package gates against recorded inputs.
Preserve earlier workloads and useful failures. Close full REQ-10 only after all
four checkpoints pass under the current-machine scope.

Reuse current build profiles and canonical dependency inputs. Keep final logs,
receipts and manifests compact; archive historical raw evidence with verified
hashes instead of retaining copied build environments. Do not change host/WSL
settings. MMOWorld, distributed authority and capacity retain M09/M10; asset jobs,
scene rendering, full terminal authoring and Lua retain 11-14, 15 and 19.
