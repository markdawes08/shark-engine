# Static glTF qualification

Assignment 12 qualifies the bounded CPU static profile in
[ADR 0006](architecture/0006-v1-profile.md) through the existing source owner,
converters, engine payload and AssetImports service. Follow the
[testing policy](testing-policy.md) when selecting future checks; this document
defines behavior, not a recurring full-matrix requirement.

## Acceptance paths

| Boundary | Observable acceptance |
| --- | --- |
| Source | Licensed offline `.gltf`/`.glb`, external/data-URI/buffer-view resources, sparse accessors, checked offsets and effective indices; explicit malformed, oversized, escaping and unsupported-content failures. |
| Geometry | Independent vertices/indices, strip/fan expansion, generated normals/tangents, UV/color fields, shared meshes, hierarchy, negative/nonuniform transforms and derived bounds. |
| Textures | PNG/JPEG pixels, complete NPOT mip chains, distinct color/data/normal semantics, precision, alpha and sampler fields; invalid/missing inputs and allocation failures reject. |
| Materials/payload | Core factors and five texture roles, OPAQUE/MASK/BLEND and culling fields survive the bounded deterministic engine codec; invalid versions/digests/layouts/references reject. |
| Owner/cache | Typed resource descriptors and original source digests, exact dependency/settings identity, privacy, source moves, reuse/reopen, stale completion, cancellation and last-valid output. |
| Process restart | Actual glTF jobs interrupted after conversion, before native publication and after publication reopen to the old or new complete closure; explicit recovery and retry reconcile the durable cache. |

The source/geometry/texture families retain their earlier 41/45/43 cases,
including allocation sweeps. Material and payload families retain 14/12 cases.
The owner integration family has 19 cases: its original fifteen plus saved
strip containers, repaired sparse indices, textured GLB semantic mips and
optional core fallback followed by required-extension/missing-image failures.
All existing workloads, seeds and timeouts remain unchanged.

`shark.tools.gltf-process` extends the existing AssetImportProcess executable and
asset-job supervisor with `--gltf`; it does not introduce another cache, worker
or recovery mechanism. Its 90-second family uses three actual abrupt-exit phases:
`ready`, `before_publish` and `published`. An external buffer edit changes both
dependency and root keys. Each restarted process decodes the real engine payload
and checks literal geometry, mirrored/nonuniform bounds and BLEND material values.
The supervisor independently checks binary source bytes, unchanged metadata,
atomic cache replacement, retained keys, orphan-stage handling, foreign job-ID
rejection and reproducible final cache bytes. Observation markers never authorize
replay. The original 59 synchronous and 113 asynchronous byte-import checks remain.

FeatureWorld retains thirteen native cases, its four saved entity IDs, 31 process
checks and 47 CPU surfaces plus one separately qualified GPU command. No public
surface is added by 12e. Pure glTF cases exercise actual converted values; the
existing owner/job cases and connected import fixtures exercise publication.
Camera/MeshRenderer scene integration and displayed imported assets belong to 13.

## Independent fixture oracle

The pinned [Khronos validator](dependencies/gltf-validator.md) checks twelve saved
licensed positive containers and three deliberately invalid controls offline.
Its reports are additional evidence, not substitutes for Shark's bounds, decoder,
payload or owner tests. Normal CTests do not download models or require Node.
The corrected texture containers include required POSITION bounds without
changing their geometry/image bytes or independent pixel expectations.

## Qualification scope

At assignment closure, use the applicable Windows/WSL Debug inventories,
glTF Release and sanitizer coverage, independent standalone Tools checks and
protected package deployment checks. Reuse unchanged public-example and earlier
component evidence explicitly; do not count it as fresh execution. Keep concise
JUnit, logs and the [12e handoff](handoffs/12e-gltf-qualification.md).

The source parsers and decoders remain private to Tools and absent from protected
Runtime/Server closures. Payload version 1 is a development representation, not
final cooked output. CPU import success does not qualify material rendering,
skeletal animation, streaming, a source-file runtime mode or production MMO
capacity; those retain their assigned later owners.
