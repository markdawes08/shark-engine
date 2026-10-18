# Offline shader pipeline

Assignment 06a provides an offline developer tool. It compiles HLSL into validated
SPIR-V without a window, Vulkan device, Editor or game process. The
[checkpoint plan](roadmap/shader-resource-plan.md) records the limited sequencing
exception: 05c remains open and full assignments 05/06 remain NOT_RUN.

Use the repository Python environment from the repository root. Tool preparation
is explicit and separate; the build command never downloads tools or searches
PATH for a compiler. It reuses the exact DXC archive and standalone SPIRV-Tools
receipt already pinned by assignment 02. No dependency revisions or licenses
change. Existing probe shaders and receipts remain separate historical fixtures.

```powershell
.tools/venv/Scripts/python.exe tools/shader_pipeline.py build --recipe shaders/recipe.json --output out/shaders/development --validation-manifest out/05a-work/validation-tools/summary.json
.tools/venv/Scripts/python.exe tools/shader_pipeline.py verify --output out/shaders/development
```

The validation receipt path above identifies this machine's retained preparation.
On a fresh checkout, prepare the locked tools using the commands in the
[graphics probe guide](graphics-probe.md), then pass the resulting receipt
explicitly. The builder checks the lock and executable identity; a stale or
modified input is an error. The compiler does not need the validation-layer DLL
or a GPU merely to validate SPIR-V.

## Contract and scope

The version-1 [recipe](../shaders/recipe.json) names explicit entry points and
stages under the fixed `shark-vulkan-1` convention. Sources and literal include
dependencies are bounded and contained by the recipe root. Unsupported source
forms or recipe versions fail explicitly. Source content, include dependencies,
compiler options, tool identities and the dependency lock participate in
reproducibility. Compiler defaults and timestamps do not define artifact identity.

The recipe contains exactly `schema_version`, `convention` and `shaders`. Each
shader has `id`, `source`, `entry`, `stage` and `bindings`; each binding has `kind`
and `slot`. Stages are `vertex`, `pixel` and `compute`, using shader-model 6.0
profiles. IDs are unique lowercase identifiers, at most 64 characters; entry
points are C-style identifiers with the same length limit. Unknown fields,
duplicate JSON keys, nonfinite numbers and unsupported versions are rejected.

Compilation fixes `-spirv -fspv-target-env=vulkan1.3 -O3 -Ges -WX -Zpc` and the
declared stage/entry. There are no user-supplied compiler flags, include search
paths or defines. Version 1 permits only literal quoted `#include` directives;
macros, conditional directives, include guards, `#pragma`, line continuations,
BOMs and environment/path/time-dependent builtin macros are unsupported. Paths
use normalized relative `/` spelling without parent traversal, drive prefixes,
links/junctions, ambiguous Windows components or case-colliding names. Includes
resolve relative to their containing file and remain within the recipe directory.

Limits are 32 shaders, 64 source/include files, include depth 16, 1 MiB per source
and 4 MiB for the complete source closure. Each shader permits 64 declared bindings
and produces at most 4 MiB of SPIR-V. Source snapshots are compiled from owned
staging; changes to inputs detected during a build prevent publication. A changed
dependency rebuilds the entire recipe generation. Unreferenced files do not
invalidate it. This checkpoint does not provide a per-entry object cache.

Each completed build publishes `shader-manifest.json`, referring to a complete
immutable generation beneath `generations/`. Publication replaces the manifest
only after all outputs pass independent SPIR-V validation and integrity checks.
A failed build leaves the last published generation usable. Repeating unchanged
inputs verifies existing artifacts before returning a cached result. This is a
single-writer developer workflow; conflicting publication must fail explicitly.
Automatic garbage collection and hot reload are outside this checkpoint.

The `verify` command checks the artifact closure without requiring shader source,
DXC, spirv-val, or a graphics device. Copy the published manifest and its referenced
generation together for relocation. Hashes detect missing or modified bytes;
they are integrity metadata, not signatures or an authenticated content channel.
The final game package and EngineClient artifact loader belong to 06b/06c.

The builder's binding convention retains the pinned probe/NVRHI offsets: within
descriptor set 0, sampled/read-only resources start at 0, samplers at 128,
constant buffers at 256 and writable resources at 384. Matrix declarations use
explicit column-major layout and matrix-times-column-vector multiplication.
Each kind permits slots 0..127. The builder checks SPIR-V entry/stage and exact
descriptor set/binding numbers against the recipe, then runs independent
`spirv-val`. This is not full resource-type, member-offset/stride, vertex-input or
cross-stage reflection. Those checks belong with the GPU loader/pipeline in 06b.
The supplied vertex shader performs the one projection-Y inversion; 06b must
avoid also flipping the supplied matrix or viewport.
The convention is versioned; it does not select a material schema, bindless
resource system, streaming policy or world-coordinate representation.
The existing [space/depth/color contract](architecture/0005-simulation-and-rendering.md)
still governs the later rendering fixtures.

Commands emit structured JSON with status and diagnostics; failures return
nonzero. The existing bounded process runner owns compiler and validator children.
JSON results have `schema_version: 1`; success has `status: PASS`, `operation`,
`generation` and shader count. Builds also report `disposition: BUILT` or `CACHED`.
Failure has `status: FAIL` and an `error` with stable `code`, readable `message`
and relevant `path`. Exit 0 means success (or requested help); exit 1 means failure.
Diagnostic codes distinguish `shader.arguments`, `shader.schema`, `shader.version`,
`shader.path`, `shader.source`, `shader.toolchain`, `shader.layout`, `shader.artifact`,
`shader.busy`, `shader.changed`, `shader.compile`, `shader.validation` and `shader.io`.
Compiler/validator calls each have a 30-second deadline; the process runner's
bounded termination grace applies separately. A caller may bound the whole recipe
more tightly. This tool does not promise a 30-second deadline for an entire build.
Human contributors and terminal agents use these same commands. These are
development build tools, not the future public authoring commands from assignment
15. No provider SDK or model service is involved.

## Verification and boundaries

```powershell
.tools/venv/Scripts/python.exe tools/tests/test_shader_pipeline.py -v
```

CTest registers that device-free suite as `shark.tooling.shader-pipeline` with
labels `cpu;tooling` and a 30-second timeout. It uses controlled tool fixtures;
passing it alone is not evidence that the real compiler or a GPU worked. The
explicit real-tool fixture and its results are documented in the
[06a handoff](handoffs/06a-offline-shaders.md).

```powershell
.tools/venv/Scripts/python.exe tools/tests/check_shader_pipeline.py --validation-manifest out/05a-work/validation-tools/summary.json --recipe shaders/recipe.json --output out/shader-check-new
```

Use a fresh evidence directory. This fixture runs the actual offline compiler and
validator, checks independent cold-build equality, nested-include rebuilds,
failed-build preservation and source/tool-free artifact relocation. It is opt-in
and does not open a graphics device.

Runtime, Core and Server do not invoke or link this tool. Their CMake graph and
install/package closure remain unchanged. Normal test configurations register
CPU checks without preparing DXC or SPIRV-Tools; Runtime-only and Server-only
builds do not discover Python for this feature. Generated shader output and
evidence belong under ignored `out/`; the recipe, shader sources, tools and tests
stay versioned. Full GPU pipeline/layout, upload, offscreen/window readback and
compiler-free game-package acceptance remain in 06b/06c.
