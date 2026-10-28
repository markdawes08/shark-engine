# Shark

Contributors and AI agents must follow [AGENTS.md](AGENTS.md) and the
[testing policy](docs/testing-policy.md). Select checks for affected behavior;
whole-project workflows are available for explicitly broad verification.

Shark is a C++20 game engine targeting a **seamless open-world MMORPG**, usable
by both human editor users and terminal-based LLM agents through shared services.
Development and acceptance target **this Windows PC**, its RTX 4070 Laptop and
Intel GPUs, and existing **WSL2 Ubuntu** for CPU/headless Server work.
[ADR 0010](docs/architecture/0010-current-machine-scope.md) limits the plan to
available resources; other hardware, native Linux graphics, cloud deployment
and Mac compatibility are outside current scope. The planned local workload is
16 active synthetic players across two region processes, plus a separate
two-client graphical fixture; this is a development target, not a production
capacity claim.

The [MMORPG plan](docs/roadmap/mmorpg-plan.md) defines the authoritative server
and persistent-world work. The [human/agent plan](docs/roadmap/human-agent-plan.md)
defines shared authoring, discoverable schemas, noninteractive results and safe
project handoff without requiring a model or provider integration.

**13a/13b/13c are complete under the current-machine scope.** The
[latest handoff](docs/handoffs/13c-scene-extraction.md) records CPU scene extraction,
camera math, conservative bounds and culling on the shared components and
immutable prepared assets. Full assignment 12 remains complete.
**Next: 13d, depth-tested offscreen rendering and viewport resize.** Completed
checkpoints and their original results are indexed in
[requirements and coverage](docs/coverage/README.md); historical counts are not
recurring verification requirements. The user's [nonblocking backlog decision](docs/backlog.md)
authorizes continued development under each increment's acceptance criteria.

Current capabilities include:

- [Core services](docs/core-contracts.md), [application ownership](docs/application-lifecycle.md)
  and [bounded timing/work](docs/time-and-work.md), with separate Core, Client,
  Tools and headless Server boundaries.
- [Input](docs/input-contracts.md), [Windows windows](docs/window-lifecycle.md)
  and an [offline frame/input loop](docs/frame-loop.md), with owned
  [graphics devices](docs/graphics-device.md), [clear presentation](docs/presentation-lifecycle.md),
  [GPU resources](docs/gpu-resources.md) and a fixed textured-cube sample using
  [precompiled shaders](docs/shader-pipeline.md). The opt-in
  [graphics probe](docs/graphics-probe.md) supplies analytic draw/compute/readback
  cases. CPU build success does not establish graphics or release-platform support.
- CPU [scene ownership](docs/scene-contracts.md), [Name/Transform and hierarchy](docs/transforms.md),
  [schemas and revisioned inspection](docs/scene-inspection.md),
  [source codecs](docs/source-documents.md), [capture/activation](docs/authored-scenes.md)
  and [project-file conflict handling](docs/project-files.md), plus
  [prepared content](docs/prepared-content.md) and CPU
  [render extraction/cameras/culling](docs/scene-rendering.md).
- Shared [authoring commands](docs/authoring-commands.md),
  [atomic batches and preview](docs/authoring-transactions.md),
  [undo/redo](docs/authoring-history.md) and [retained retries](docs/authoring-retries.md)
  through one owner. Terminal/editor adapters retain assignments 15/16.
- Bounded [TLS/wire transport](docs/network-transport.md),
  [mutually authenticated owned sessions](docs/network-sessions.md),
  [authoritative replication](docs/network-replication.md) and
  [spatial interest/privacy/budgets](docs/network-interest.md), qualified through
  [local processes and deterministic faults](docs/network-qualification.md).
- Server-only [durable storage](docs/durable-store.md),
  [owned characters](docs/durable-characters.md), [atomic transactions](docs/durable-transactions.md),
  [runtime recovery](docs/durable-recovery.md) and [migration/restore](docs/durable-maintenance.md),
  with [connected qualification](docs/durable-qualification.md).
- CPU [asset identity/catalog](docs/asset-identity.md),
  [dependency imports/cache](docs/asset-import-cache.md) and
  [bounded background jobs](docs/asset-import-jobs.md). The completed
  [glTF checkpoints](docs/roadmap/gltf-plan.md) add [source inspection](docs/gltf-source.md),
  [geometry/hierarchy](docs/gltf-geometry.md), [images/textures](docs/gltf-textures.md)
  and [materials/typed asset integration](docs/gltf-assets.md).
- The saved [FeatureWorld](docs/featureworld.md), CPU [SceneRunner](docs/scene-runner.md)
  and explicit [CPU/GPU supervisor](docs/scene-execution.md), with case discovery,
  seed/tick controls and versioned JSON results. These are test workflows;
  graphical scene authoring retains its later checkpoints.

Editor/Runtime currently expose version/help, `--cube`, `--graphics` and
`--window` diagnostics. Server exposes validated startup, timed execution and
shutdown. These bootstraps have no active player connections, world simulation,
scene renderer or game-authoring UI yet; implemented networking, persistence
and editing services are C++ APIs.

- [Architecture decisions](docs/architecture/README.md) define target boundaries,
  dependencies, formats, simulation conventions and the MMORPG profile.
- [Requirements and coverage](docs/coverage/README.md) retain assignment scope,
  acceptance criteria, surface mappings and original handoff/receipt navigation.
- [Build instructions](docs/building.md), [CI](docs/ci.md) and [agent guidance](AGENTS.md)
  describe tools, commands and ownership policies.
- The [01 CI follow-up](docs/handoffs/01-ci-verification.md) and
  [M01 CI follow-up](docs/handoffs/M01-ci-verification.md) preserve the completed
  hosted CPU, headless Docker and deliberate-failure/restored-success evidence.
  Hosted CI remains supplemental to current-machine roadmap acceptance.

For an explicit whole-project build/test run, use an x64 Visual Studio developer
shell with the documented tools installed. For incremental work, select affected
targets/tests under the [testing policy](docs/testing-policy.md):

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Prepare the fixed cube sample explicitly after the C++ build:

```powershell
.tools/venv/Scripts/python.exe tools/shader_pipeline.py prepare --recipe shaders/recipe.json --output out/shaders/development --validation-manifest out/05a-work/validation-tools/summary.json --bin-dir build/windows-debug/bin
.\build\windows-debug\bin\Editor.exe --cube
```

The receipt path identifies the retained locked tools on this machine; the
[shader guide](docs/shader-pipeline.md) describes preparation and compiler-free
export. `--cube` loads precompiled `assets/cube.sharkshader` beside the executable;
startup never compiles shaders. For a bounded run use
`build/windows-debug/bin/Runtime.exe --cube --graphics-ms 1000`.
The existing `--graphics` flag opens the clear-presentation sample.
F6 toggles offline pause and F7 advances one empty tick while paused; presentation
continues while visible. Diagnostic JSON is available in the terminal. No game
world or authoring UI runs. See the [presentation guide](docs/presentation-lifecycle.md)
for explicit device selection, validation and qualifying native fixtures.
The existing `--window` option retains the empty-window diagnostic described in
the [window guide](docs/window-lifecycle.md).

Use `linux-debug` in the existing WSL Linux environment. Sanitizer, release and isolated
Runtime configurations are documented in the build guide. All staging, commits
and pushes remain with the user; generated output and local tooling stay ignored.

After provisioning the locked Core dependency archives described in
[building](docs/building.md), use `cmake --preset server-only`
for a server without client/editor/test targets,
then `cmake --build --preset server-only`. Run
`build/server-only/bin/Server --run-for-ms 1000` (append `.exe` on Windows)
for a one-second headless smoke run.
With no arguments it waits for shutdown. This is an idle process foundation;
it has no player connections or world simulation yet.
