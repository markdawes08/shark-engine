# Supervised CPU and GPU test execution

Checkpoint 10c adds `tools/scene_runner.py`, following
[ADR 0024](architecture/0024-headless-scene-runner.md). It drives existing native
cases through the shared bounded process supervisor. CPU work uses the
[SceneRunner worker](scene-runner.md); GPU work uses the current Client graphics
fixtures. Python uses the standard library and the repository's existing helpers.
No editor, provider SDK or MCP adapter is required.

From the repository root, with existing output parent `out`:

```text
python tools/scene_runner.py --mode cpu --list-cases --executable build/tools-debug/bin/SceneRunner.exe
python tools/scene_runner.py --mode gpu --list-cases
python tools/scene_runner.py --mode cpu --case featureworld --executable build/tools-debug/bin/SceneRunner.exe --project assets/FeatureWorld/FeatureWorld.sharkproject --workspace out --ticks 3 --hz 20 --seed 1337 --revision my-revision --output out/featureworld-execution
```

CPU discovery uses the real worker catalog; execution requires a matching case
or suite. All original native commands remain usable directly. CPU supervision
does not import graphics helpers or initialize a renderer. Existing
[FeatureWorld ownership/scratch rules](featureworld.md) remain required.

## Explicit GPU cases

| Case | Native fixture and required payload | Actual work |
| --- | --- | --- |
| `gpu.offscreen-cube` | `OffscreenSmoke`, `--shaders` verified offline generation root | Two textured-cube uploads/readbacks at 96x80 and 128x128, depth occlusion, reversed triangle order and changed texture |
| `gpu.window-cube` | `CubeSmoke`, `--bundle` precompiled `.sharkshader` | Three actual window captures and paired offscreen draws at 192x128, 256x192 and 192x192, resize/minimize/restore, completed retirement |

The current Windows builds enable native window/graphics and test fixtures.
Build the normal Debug, Release or ASan profile. GPU discovery describes required
capabilities and performs no device access; it does not claim those capabilities
are available. Actual execution requires an exact lowercase device UUID, its
built fixture, the precompiled payload and the locked validation receipt.
The supervisor has no validation-off or automatic-device option.

For example, reuse the existing tools and prepare an offline shader generation:

```text
python tools/shader_pipeline.py build --recipe shaders/recipe.json --output out/scene-shaders --validation-manifest out/graphics-tools/current-validation/summary.json
python tools/scene_runner.py --mode gpu --case gpu.offscreen-cube --executable build/windows-debug/bin/OffscreenSmoke.exe --device 863d6f59a51894ab8b4829aa9460c78d --shaders out/scene-shaders --validation-manifest out/graphics-tools/current-validation/summary.json --build-dir build/windows-debug --revision my-revision --output out/gpu-offscreen-nvidia
python tools/scene_runner.py --mode gpu --case gpu.window-cube --executable build/windows-debug/bin/CubeSmoke.exe --device 868088a7040000000002000000000000 --bundle build/windows-debug/bin/assets/cube.sharkshader --validation-manifest out/graphics-tools/current-validation/summary.json --build-dir build/windows-debug --revision my-revision --output out/gpu-window-intel
```

Use the existing executable-adjacent cube bundle prepared under the
[shader distribution contract](shader-pipeline.md#prepare-and-distribute-the-cube-sample).
The generation/bundle is checked before and after execution; no compiler is
discovered or invoked during a GPU run. GPU cases use fixed native work, so
seed/tick/input/project controls are rejected rather than silently ignored.

Use a normal terminal for validation. A high-integrity process returns explicit
NOT_RUN because Vulkan ignores its development layer paths. The existing
same-user helper can launch the entire command without prompting or modifying
host settings:

```text
python tools/windows_unelevated.py --timeout 110 -- python tools/scene_runner.py --mode gpu --case gpu.window-cube --executable build/windows-debug/bin/CubeSmoke.exe --device 863d6f59a51894ab8b4829aa9460c78d --bundle build/windows-debug/bin/assets/cube.sharkshader --validation-manifest out/graphics-tools/current-validation/summary.json --output out/gpu-window-standard-user
```

No unavailable device, platform, validation tool or shader payload can select a
different adapter, backend or CPU case. Native Linux graphics remains outside
the current scope; WSL exercises CPU execution and explicit GPU rejection.

## Results, limits and failures

Execution emits one UTF-8 `shark.scene-execution` record, integer
`schema_version: 1`, to stdout and `<output>/result.json`. GPU/CPU discovery uses
`shark.scene-execution-cases` version 1. Existing `shark.scene-run` and
`shark.scene-cases` version-1 worker formats are retained. Reject unsupported
versions; no implicit migration is promised.

| Exit | Status/meaning |
| --- | --- |
| 0 | PASS, actual selected work and requested evidence publication completed |
| 1 | FAIL, native work, validation, evidence or cleanup failed |
| 2 | ERROR, invalid request or result infrastructure/publication failure |
| 3 | NOT_RUN, required native capability unavailable; no fallback |
| 124 | FAIL, worker deadline expired; partial work may have executed |
| 130 | CANCELLED, cooperative process interruption; partial work may have executed |

Always inspect exit and status. `renderer_initialized` is false for CPU and
pre-dispatch rejection, true for verified GPU completion and null for incomplete
GPU execution whose initialization cannot be established. A failed outer request
can occur after native work or publication; never blindly repeat external effects.
Hard process/machine termination cannot guarantee a final result record.

The output parent must exist; the evidence directory must be absent and must not
traverse symlinks/junctions. Result publication uses an owned sibling stage and
atomic create-only hard link, with no overwrite fallback or power-loss durability
claim. Unexpected failures retain partial native files for inspection. Stdout
failure remains nonzero even after file publication.

Bounds are one MiB per JSON result/worker stream, 128 artifact files, 32 MiB of
retained artifact payload, eight path levels and 4096 UTF-8 bytes per artifact
path. IDs/revisions use the existing 1..64 ASCII token contract. CPU controls keep
their earlier limits. The worker deadline is 1..180 seconds, default 90; CPU case
discovery has a separate ten-second bound. Native GPU fixture deadlines remain
45/60 seconds. Local validation/hash/rasterizer work uses the existing bounded
fixed fixtures; callbacks/executables are trusted and must return. The existing
process-runner containment limits still apply to deliberately escaping Unix
descendants and hard termination.

GPU PASS rechecks fresh native identities, exact selected adapter/capabilities,
actual readback bytes, required clean validation and completed resource owners.
The fixed 1-byte color and 0.00001 depth tolerances, subpixel model, coverage and
occlusion minima are unchanged. The [GPU resource contract](gpu-resources.md)
defines the reference and its fixed edge/discontinuity exclusions. These cases
exercise current graphics diagnostics; assignment 13 owns authored scene rendering.

CPU CTest families `shark.tools.scene-execution` and
`shark.tooling.scene-execution-evidence` have explicit 90-second timeouts. They
test real CPU processes, UTF-8 paths, publication, nonzero failure, capability
rejection and synthetic evidence mutations. Synthetic fixtures qualify no GPU.
Hardware qualification is an explicit separate run on both current Windows GPUs.
Full REQ-10 is PASS in the [integrated handoff](handoffs/10d-integrated-qualification.md).
The [qualification contract](featureworld-qualification.md) connects manifest
audits, actual saved cases and intentional failure through these existing results.
