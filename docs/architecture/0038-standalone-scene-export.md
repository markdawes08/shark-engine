# ADR 0038: Standalone scene export and package startup

Date: 2026-10-28. Status: accepted implementation decision for assignment 14b.

Extend the existing Runtime packager with a noninteractive `export` command.
Use a supervised CPU Tools worker to open the source project, capture its
declared startup scene and explicit camera through the
[14a preparation service](../client-export.md), and retain the ProjectFiles owner
until package publication finishes. Imports remain explicit prerequisites; the
command receives an explicit sidecar inventory and content target. It never
starts an editor or graphics device.

The worker writes an owned temporary content generation and keeps the original
scene/catalog/cache owners alive. Immediately before the package's final rename,
the parent requests source/project/shader checks and candidate revalidation.
The project lock remains held through that rename. Conflicting cooperating
authoring access rejects; external writers must honor the same coordination
contract. This is bounded local publication, not a hostile-filesystem sandbox or
a power-loss durability guarantee. Existing output is never overwritten.

Keep package assembly, Runtime/CRT closure inspection, shader provenance,
redistribution notices, inventory digests and staging in the existing package
tool. The new `runtime-scene-export` package kind uses its own strict content
inventory and includes dependency attribution without source paths, settings,
history or server-private payloads. Required binary target is `windows-x86_64`;
the cache's content-target token is a separate identity. Preparation and package
construction do not establish GPU or isolated deployment qualification.

Core owns a small independently versioned binary startup descriptor, `SHKSTRT1`
version 1. It records project/world/scene/camera identities, display name, target,
and bounded lengths/SHA-256 identities for the prepared scene, external meshes
and two precompiled scene shaders. Paths are fixed by the format. Both Tools and
Client use this Core codec; Runtime gains no source JSON, glTF, image decoder,
authoring or shader compiler dependency. Unsupported versions require a new
export, with no runtime migration or reinterpretation as the final cooked format.

Client resolves package paths through the existing canonical contained path
service, reads owned bounded snapshots, verifies inventory identity, and decodes
those same bytes into a complete scene and shader candidate. Runtime launches
that candidate through the existing frame/presentation owner. F5 checks a complete
replacement including descriptor, camera and shaders; failure retains the active
candidate. Successful replacement receives a fresh scene incarnation and follows
the existing GPU drain/retirement boundary. Explicit development `--scene` launch
keeps its existing behavior.

`Runtime --package PATH` selects the descriptor; no-argument Runtime discovers
`startup.sharkstart` beside its executable independently of the current working
directory. Without that file, its existing help behavior remains. Informational
arguments do not auto-launch a package. `--check-package PATH` provides portable
CPU validation with structured results and no renderer; actual package launch
is restricted to Windows x86-64. Follow the
[standalone export contract](../standalone-export.md) for formats and commands.

The [export plan](../roadmap/export-plan.md) retains 14c for relocation, isolated
environment/module inspection and actual package presentation on both current
Windows GPUs. A local early scene export is not a dedicated server, MMO release,
full material renderer or final cooked asset pipeline; those owners remain M08,
later graphics assignments and 31.
