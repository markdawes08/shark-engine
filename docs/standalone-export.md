# Early standalone scene export

Assignment 14b connects [client content preparation](client-export.md) to the
existing Runtime packager and package-relative launch. Follow
[ADR 0038](architecture/0038-standalone-scene-export.md) and the
[export checkpoints](roadmap/export-plan.md). Construction, CPU inspection and
interactive launch are distinct from [14c's completed isolated deployment gate](handoffs/14c-isolated-export-acceptance.md).

## Export command and inputs

The public noninteractive command is `tools/package_runtime.py export`. It runs
on native Windows x64 and requires `--target windows-x86_64`. The separate
`--content-target TOKEN` selects the client import-cache profile used to prepare
mesh content; it does not authorize a different binary platform.

Supply a current protected `runtime-only` Release build, the exact locked
app-local Release CRT directory, verified offline scene-shader generation, CPU
`ExportContent` executable, saved source project, explicit authored camera UUID
and fresh output directory. Export output must be outside the source project
root. The default build/test/source boundary remains described in
[building](building.md); only the development command needs Python, the worker
and build inspection tools. They do not ship or run with the exported client.

For example, after building the named targets and preparing the scene shaders:

```powershell
& ./tools/windows_dev.ps1 -CommandArguments @('cmake','--build','--preset','tools-debug','--target','ExportContent')
& ./tools/windows_dev.ps1 -CommandArguments @('cmake','--build','--preset','runtime-only','--target','Runtime')
.tools/venv/Scripts/python.exe tools/shader_pipeline.py build --recipe shaders/scene_recipe.json --output out/shaders/scene-development --validation-manifest out/graphics-tools/current-validation/summary.json
& ./tools/windows_dev.ps1 -CommandArguments @('python','tools/package_runtime.py','export','--build-dir','build/runtime-only','--crt-dir','C:/Program Files/Microsoft Visual Studio/18/Community/VC/Redist/MSVC/14.51.36231/x64/Microsoft.VC145.CRT','--exporter','build/tools-debug/bin/ExportContent.exe','--project','assets/FeatureWorld/FeatureWorld.sharkproject','--camera','20000000-0000-4000-8000-000000000004','--target','windows-x86_64','--content-target','development','--shaders','out/shaders/scene-development','--output','out/exports/FeatureWorld')
```

The CRT and validator paths above identify this machine's locked inputs; follow
the build/shader guides if their retained locations differ. Shader preparation is
an explicit offline step. Export verifies its published generation and does not
compile shaders, initialize a GPU or discover an editor. The FeatureWorld example
uses only built-in geometry and therefore needs no imported asset sidecars.

For imported meshes, repeat `--sidecar PROJECT_RELATIVE_PATH` for the required
asset and dependency metadata inventory. The paths use the normal contained
portable ProjectPath rules. Missing/stale/private/corrupt content rejects. The
selected assets must already have fresh typed glTF artifacts for the exact
client content target through the shared import services. There is no implicit
import, recursive source-directory scan or use of stale last-valid output. The
explicit sidecar inventory admits at most 4096 entries; the used export closure
keeps 14a's narrower 128-source, 126-external-mesh and repeated-work limits.

Supply `--content-notice LICENSE=PATH` for every distinct license identifier in
the used asset closure, for example
`--content-notice MIT=tests/fixtures/gltf_source/LICENSE`. The command includes
the full UTF-8 notice text, checks exact license coverage and rejects unused or
duplicate declarations. Each notice is at most 64 KiB, with at most 1 MiB total.
Notices are copied and checked again before publication. Built-in-only exports
need no asset notice arguments; engine/library notices are always included.

The source project and declared startup scene are loaded through ProjectFiles.
Another cooperating project owner rejects conflicting access. This command
exports saved state; it cannot reach an unsaved editor session. The underlying
14a C++ service can capture an existing owner's unsaved state without saving it.
The [terminal adapter](authoring-protocol.md) now uses that existing owner through
`scene.export`; the human editor remains assignment 16. Configured terminal hosts
use the same staging and package assembly, with an inverted bounded child exchange:
the Python helper prepares package inputs, the retained native owner stages its
current candidate and grants publication only after final source/candidate/shader
validation. The helper never opens a second ProjectFiles owner. Its process tree
is contained and bounded to 60 seconds; a missing completion after the grant
reports uncertainty and requires destination inspection before retrying.

## Publication and structured results

`ExportContent` is an internal supervised CPU worker, not a second public
authoring protocol. It retains the ProjectFiles lock and the same scene/catalog/
cache owners while the parent assembles the candidate. Its bounded private
READY/VALIDATE/RELEASE exchange has a 60-second parent deadline. Closing the
exchange or a timeout fails the operation and releases the worker. The full
packaging command also performs bounded toolchain/binary inspection before
acquiring that project owner.

Before publication, the worker rereads project/startup source revisions,
revalidates the 14a candidate and checks the same shader input bytes. The parent
keeps the worker alive across the final package rename. Cooperating source and
metadata writers must honor the project lock; uncoordinated filesystem changes
between validation and use remain outside this local coordination guarantee.

Package files are assembled in a fresh sibling stage, checked against their
inventory and renamed into a fresh destination only after final validation.
An existing output is preserved. Failure before publication leaves no completed
package. This atomic visibility is not an fsync/power-loss durability guarantee.
If publication succeeded but later worker release or cleanup fails, the result
retains `published: true`; callers must inspect that outcome instead of blindly
retrying or assuming rollback. Successful temporary stages are cleaned up.

The command emits UTF-8 JSON `shark.client-export`, `schema_version: 1`, with
status, publication state, `renderer_initialized: false`, package path, manifest
digest, content identity and artifact metadata on success. Errors carry a stable
code and useful context. Exit 0 is completed publication; exit 2 is rejection or
failure before publication; exit 1 reports failure after publication. Every
construction result retains `qualification: NOT_RUN`.

## Package inventory

`package-manifest.json` retains package schema 1 and adds the explicit kind
`runtime-scene-export`. The descriptor and content are beside Runtime under
`bin/`; all descriptor paths are relative to that directory:

| File | Purpose |
| --- | --- |
| `bin/Runtime.exe` and declared DLLs | Protected Release client, app-local Vulkan loader and locked Release CRT |
| `bin/startup.sharkstart` | Engine-owned startup descriptor |
| `bin/content/startup.sharkscene` | Prepared startup scene, not source JSON |
| `bin/content/<asset-uuid>.sharkmesh` | One prepared payload per referenced external asset; built-ins need no file |
| `bin/assets/scene-vs.spv`, `bin/assets/scene-ps.spv` | Verified scene-unlit vertex and pixel shaders |
| `licenses/Content/attribution.json` | Used import-closure asset/key/origin/license attribution |
| `licenses/Content/notices.json` | Full declared asset notices, sorted by license identifier with byte counts and hashes |
| `licenses/`, `provenance/` | Existing locked redistribution notices, actual binary closure, toolchain/CRT records and scene-shader provenance |

The content record is `shark.client-export-content`, version 1: project, world,
scene, camera, name, content-target identity, dirty status, fixed file paths with
sizes/digests, and used source attributions. The packager independently validates
it against the startup descriptor and payload inventory. Duplicate paths,
case-colliding names, missing/extra files, unsupported fields/versions and invalid
digests reject. Source files, import caches, worker binaries, authoring libraries,
compilers, server-private data and unrelated dependency resources do not belong
in this package.

Existing package bounds remain 256 files, 128 MiB per file and 512 MiB total.
Runtime's exact Release CRT closure includes `msvcp140_2.dll` for the scene
math's three-argument hypot functions. Its bytes, version and signed source are
pinned alongside the existing three CRT DLLs; the graphics-probe package keeps
its original CRT selection.
Content has smaller independent bounds: 32 MiB combined prepared scene/meshes,
4 MiB per scene, 2 MiB per mesh and 126 external meshes. Each shader is 20 bytes
through 4 MiB and a multiple of four bytes. Existing decoded scene/asset budgets
also apply. Driver/display and supported Windows system prerequisites remain
explicit; locally bundled files do not replace the installed GPU driver.

## Startup descriptor

Core's [prepared_startup.hpp](../engine/core/include/shark/prepared_startup.hpp)
defines the 64 KiB maximum `SHKSTRT1` version-1 descriptor. Integers are explicit
little-endian values; no native structs or handles are serialized. Its envelope
is eight magic bytes, uint32 version and 32-byte SHA-256 of the remaining body.

The body contains project/world/scene/camera UUIDs as 16 bytes each in text byte
order; uint32-length UTF-8 name and target strings; scene, vertex-shader and
pixel-shader file identities; then a uint32 mesh count and mesh records. A file
identity is uint32 byte length plus 32-byte SHA-256. Each mesh record adds its
16-byte asset UUID. Name uses the existing 1..256-byte Name rules, target is
exactly `windows-x86_64`, and mesh IDs must be strictly ascending, unique and
different from the reserved cube/plane IDs. Unknown versions, trailing bytes,
malformed identities and exceeded bounds reject. Re-export incompatible data;
this format is separate from final cooking in assignment 31.

The descriptor contains no caller-controlled path strings. Client derives the
fixed paths above and resolves them through the existing canonical contained
ProjectRoot service. Missing files and symlink/junction escapes reject. It reads
bounded owned snapshots, checks each declared length/digest, and decodes those
same bytes. The complete prepared scene, hidden references, scene/world identity,
selected Camera and both typed shader interfaces must validate before success.
SHA-256 identifies consistent content; it is not a signature or authorization.

## Runtime launch and CPU inspection

Launching packaged `bin/Runtime.exe` without arguments finds `startup.sharkstart`
beside the executable independently of the current working directory or
`argv[0]`. If no descriptor exists, Runtime keeps its original help behavior.
`--help`, `--version` and explicit diagnostic commands do not auto-launch a game.

Explicit launch accepts the same bounded presentation controls as the
[scene viewer](runtime-scenes.md):

```text
Runtime.exe --package C:/Exports/Example/bin/startup.sharkstart --frames 2 --extent 640x480 --device auto --validation off
Runtime.exe --check-package C:/Exports/Example/bin/startup.sharkstart
```

`--package` permits `--extent`, `--frames`, `--scene-ms`, `--capture-directory`,
`--device` and `--validation`. It rejects overriding package camera, scene,
mesh or shader paths. Actual launch requires Windows x86-64. The default is
interactive until window close; bounded frame/time controls retain their
existing limits. Runtime's selected camera and content come entirely from the
descriptor. Temporary materials remain opaque unlit, with no texture/PBR claim.

F5 validates a complete package replacement, including descriptor and shaders,
before installing it through the existing graphics owner. Failed validation
retains the previous scene and shaders; successful replacement uses fresh scene
handles and bounded GPU drain. Explicit development `--scene` remains available
with its existing reload behavior.

`--check-package` is portable CPU inspection and creates no window or GPU. It
returns `shark.package-inspection`, version 1, with the validated identities,
startup digest, external mesh count and `renderer_initialized: false`. A failure
has a nonzero exit and structured diagnostics. Inspection does not present a
frame, qualify the GPU, audit all installed system prerequisites or establish
same-machine isolation; its `qualification` remains `NOT_RUN`.

## Isolated package checking

`tools/package_runtime.py check` now accepts scene exports as well as the earlier
diagnostic packages. Supply the trusted manifest digest printed by `export`, an
exact package copy relocated outside the checkout through a Unicode/space path,
both explicit local GPU UUIDs, and a fresh evidence directory outside the package:

```text
python tools/package_runtime.py check --package "C:/Exports/Shark café" --manifest-sha256 DIGEST --device 863d6f59a51894ab8b4829aa9460c78d --device 868088a7040000000002000000000000 --output out/export-check
```

Run the checker as a standard user; the current-machine workflow can use
`tools/windows_unelevated.py`. Python, PowerShell and inspection helpers supervise
verification; they are not shipped or required to open Runtime. The checker first
audits the exact package inventory/notices and runs packaged CPU inspection to
bind the project, scene, camera and content digest. It then requests two 160x120
frames per GPU, captures the result, checks finite depth and complete GPU teardown,
and audits observed app-local Runtime/loader/CRT and Windows/driver modules.
Unsupported authoring/compiler modules and changed package bytes reject.

The window contract applies per-monitor scaling to requested content dimensions.
The launcher independently samples the child window's DPI and physical client
rectangle. Completion dimensions and raw capture byte lengths must match that
exact DPI-derived extent, within the existing 2048 scene extent and readback
bounds. The requested 160x120 size and two-frame workload stay fixed; no host
display setting or rendering tolerance changes to obtain a pass.

Each child starts in a fresh, initially empty directory outside both the package
and checkout. Its environment has a system-only PATH and no inherited development
overrides. Installed GPU drivers may create output there; the checker records a
bounded directory/file footprint rather than treating output as a launch input.
Passing working directories are cleaned; unexpected failures retain their paths.

The scene checker writes checkpoint `14c` results with bounded capture identities
and per-device launch evidence. Its `qualification` stays `NOT_RUN`: it proves
local isolation and completed presentation, while fixture-specific image oracles
and required-validation native tests establish rendering acceptance separately.
Deployment uses validation off to avoid a validation SDK/layer prerequisite.
The [14c handoff](handoffs/14c-isolated-export-acceptance.md) records both image
oracles and validated native reload acceptance. System module polling does not
capture transient unload history, and development tools remain installed on this
PC; this is not an independent clean-machine or production MMO qualification.
