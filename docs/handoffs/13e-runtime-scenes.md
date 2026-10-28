# 13e: Connected Runtime scene acceptance

Date: 2026-10-28. Base: `5b68183`, initially clean `main`.
**13e and full REQ-13 are PASS under current-machine scope.** All
[five rendering checkpoints](../roadmap/scene-rendering-plan.md) are complete.
Next is assignment **14, early standalone game export**. Staging, commits and
pushing remain with the user.

## Delivered

Runtime now loads explicit prepared scene/mesh files, selects an authored camera
ID and presents depth-tested unlit geometry through the existing graphics owner.
The bounded CPU loader validates a complete candidate before returning a fresh
Scene incarnation. It accepts at most 126 external meshes, existing per-format
limits and 32 MiB combined encoded inputs. Built-ins remain automatic and source
JSON/glTF/image readers remain excluded from Runtime. Follow
[ADR 0036](../architecture/0036-runtime-scene-launch.md) and the
[Runtime scene contract](../runtime-scenes.md).

`--scene`, `--camera`, explicit scene shader files and repeated `--mesh` inputs
form the development launch path. Optional extent, frame/time bounds, adapter,
validation and a fresh capture directory support terminal use. The existing
offline event pump handles resize/minimize and F6/F7. F5 reload prepares a complete
candidate; read/camera validation failure retains current content. Successful
replacement publishes after GPU installation and invalidates old Scene handles.
Source projects are never written by Runtime. Keyboard F5 behavior is reviewed
and built; automated reload coverage exercises the shared owners directly.

`set_presentation_scene` reuses the offscreen geometry/pipeline implementation,
existing swapchain and bounded completion machinery. Immutable snapshots replace
only after drain; failed candidate creation preserves the previous view. Per-image
color/depth ownership survives unresolved work. Actual presented capture returns
sRGB RGB, linear alpha and D32 depth; Runtime writes raw RGBA8, little-endian depth
doubles and a completion report only after clean teardown. No second renderer,
device, scheduler or source compiler was introduced.

Materials remain temporary opaque unlit vertex color times base-color factor.
Textures, lighting/PBR, alpha testing/blending, animation, streaming and MMO
population/performance are not qualified by this increment. Runtime is a scene
viewer, not a game-authoring editor. Export staging, terminal authoring, editor and
final cook retain assignments 14, 15, 16 and 31.

The Tools-only preparer produces three fixtures through existing authoring/import
services: a view of saved FeatureWorld (four IDs and global anchor preserved),
a fresh project with stable cube/plane IDs, and the licensed embedded glTF triangle.
Original source fixtures are unchanged. Client-only lifecycle qualification reads
the same prepared FeatureWorld; no Client dependency enters Tools/SceneRunner.
Coverage retains sixteen native CPU cases, 55 CPU surfaces and two external GPU
commands. The existing external scene-render command now includes connected
Runtime loading, presentation/capture and lifetime checks.

## Selected verification

The [testing policy](../testing-policy.md) selected the complete affected rendering
subsystem and authoring/prepared-content consumers, portable loading/presentation
checks and actual GPU acceptance. Logs and existing-format reports are in
`out/13e-work/`; no source-freeze ledger or new receipt framework was added.

| Check | Result |
| --- | --- |
| Windows MSVC Debug | 24/24 initial subsystem CTests, 16.20s; four focused follow-ups after final parser/diagnostic/coverage changes, 3.59s; three Runtime CLI families, 0.57s |
| WSL GCC Debug | Five affected CTests pass, 1.15s: presentation, offscreen, scene viewport, content loader and launch |
| WSL Clang ASan/UBSan | Same five pass, 1.80s; leak detection and halt-on-error enabled |
| Python oracle controls | Three standalone cases pass, 0.815s, checking all three references and rejection of damaged images/reports/lifetime claims |
| Real Runtime | Three processes/captures on each GPU: FeatureWorld, fresh cube/plane and imported glTF; six independent color/depth comparisons pass |
| Connected lifetime | Seven actual presentation/capture phases per GPU; both 62-check children pass with failed replacement recovery and balanced teardown |
| Graphics regressions | Five existing offscreen scene frames on each GPU and four existing cube window/offscreen comparisons on RTX all pass |
| Protected/API | Native `runtime-only` Runtime build passes with Tools/tests OFF and protected closure; public Core/Client-only example compiles |
| Repository | Contract/links/manifest, discovery/ignore rules, C++ formatting and diff checks pass |

There are **41 CTest executions** across the selected profiles (31 Windows,
five WSL Debug and five sanitizer); three Windows families were deliberately
repeated after relevant fixes. These are not 41 unique tests. The primary Windows
selection covers Core render components/math/extraction/prepared assets/scenes,
Tools prepared content/source documents/commands/batches/history/retries/integration/
scene qualification, and Client input/window/frame/device/presentation/resources/
offscreen/viewport/content/launch/shader bundle. The later follow-up adds the
actual Tools preparation family. Existing cases, seeds and timeouts remain intact.
New loader/launch families have 60/30-second limits and seed 1337; the new tooling
and preparation families use 30/60 seconds.

| Device | Observed driver | Connected Runtime / lifetime |
| --- | --- | --- |
| RTX 4070 Laptop, `863d6f59a51894ab8b4829aa9460c78d` | NVIDIA 616.56 | 3/3 captures and 7/7 phases PASS |
| Intel RaptorLake-S Mobile, `868088a7040000000002000000000000` | Intel 101.6790 | 3/3 captures and 7/7 phases PASS |

Required Vulkan core/synchronization and NVRHI validation report zero warnings,
errors or dropped messages. The three 160x120 Runtime references check respectively
18,764 / 18,206 / 18,640 pixels at the existing one-byte color and 1e-5 depth
tolerances with one-pixel edge exclusion. Both GPUs match checked pixels exactly.
RTX fresh geometry and Intel imported geometry were visually inspected too.

Lifecycle phases are baseline, rejected partial native replacement with unchanged
old frame (without reinstall), visible entity destroyed/empty, fresh reload,
200x144 resize, 160x120 restoration and retained historical snapshot after all
Scene owners are destroyed. Reload/restoration/historical bytes exactly match the
same-device baseline. A failed missing-file candidate preserves the live revision;
reload rejects old camera/entity handles. Both runs present/complete seven frames,
retire three generations and finish with zero in-flight work. Private fault controls
exist only in the test fixture, not the Runtime launch API.

`rtx-runtime-final/summary.json` and `intel-runtime/summary.json` record actual
Runtime/lifecycle executable hashes, prepared inputs, shaders, commands and native
results. `rtx-offscreen`, `intel-offscreen` and `rtx-cube` retain regression reports
and raw readbacks. Their paired `*-token.json` files prove standard-user launches
through the existing `windows_unelevated.py` wrapper. The scene shader generation
is the unchanged 13d
`257c648de10734db42af482c80a863aacb60a001864cd0d0d30d71e24d2f2ae9`,
verified with `out/12c-work/validation-tools-pinned/summary.json`. Dependency lock
and shader sources are unchanged. Original 13a-13c component/codec/math proofs and
13d offscreen failure controls remain applicable historical evidence; new shared
draw behavior and connected lifetimes were refreshed here.

The initial RTX attempt in `rtx-runtime/` is preserved: Windows widened a decorated
96-pixel window to 120 pixels. Runtime correctly repacked the actual framebuffer
and all three processes exited successfully without GPU diagnostics, but the
fixture expected 96 pixels and its lifetime loop waited for that impossible extent.
The fixture was corrected to larger 160x120/200x144 sizes, preserving geometry,
all seven phases and every tolerance. No engine workaround or driver setting was
needed. An initial MSVC argument-test initializer error was fixed before the CPU
pass. A transient Python verifier-test setup error was also fixed before its pass.

No unrelated Server/storage/network matrix, Release matrix or package redeployment
ran. Core formats, dependencies, install rules and licenses are unchanged; package
staging is the next increment. WSL sanitizer evidence covers portable code, not
native Vulkan; both actual GPU runs cover the new native path. No host/WSL/driver
settings changed. Existing `.gitignore` rules cover prepared outputs, captures,
builds and the local roadmap. No files were staged, committed or pushed.

## Try the prepared viewer on this workspace

The checked inputs remain at `out/13e-work/prepared/`. From the repository root:

```powershell
$sceneShaderDir = 'out/13d-work/scene-shaders/generations/257c648de10734db42af482c80a863aacb60a001864cd0d0d30d71e24d2f2ae9'
& .\build\windows-debug\bin\Runtime.exe --scene out/13e-work/prepared/fresh/scene.bin --camera 13e00000-0000-4000-8000-000000000004 --vertex-shader "$sceneShaderDir/scene-unlit-vs.spv" --pixel-shader "$sceneShaderDir/scene-unlit-ps.spv" --extent 640x480
```

The default interactive validation setting is off; qualification above explicitly
used the pinned required layers. Close the window to exit. These generated test
inputs are local development examples, not an exported game or an authoring CLI.

Suggested commit: `feat(runtime): load and present prepared scenes with safe reload`.
