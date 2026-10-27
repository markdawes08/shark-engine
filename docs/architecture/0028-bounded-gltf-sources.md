# ADR 0028: bounded static glTF source inspection

Date: 2026-10-27. Status: accepted for 12a; later conversion gates remain planned.
Follow [ADR 0006](0006-v1-profile.md), the [five checkpoints](../roadmap/gltf-plan.md)
and the [source contract](../gltf-source.md).

## Decision

Use the exact reviewed cgltf dependency privately in EngineTools. Source
inspection takes one portable source path, immutable bytes and explicit local
resource snapshots carrying persistent AssetIds. It returns owned counts,
references, digests and diagnostics. No parser pointer or borrowed byte view
escapes, and inspection performs no file, network, cache or Scene operation.

Reuse the source codec's bounded SAX preflight before its DOM and cgltf parsing.
Existing project/scene limits stay unchanged; the private glTF entry permits
bounded long data-URI values and uses its own node budget. cgltf receives a
bounded custom allocator and never its file-loading helpers. Buffer/accessor
arithmetic and sparse storage/index ordering are validated before data walks.

The allocator tracks its blocks with bounded intrusive bookkeeping and aborts
through a private C++ exception on exhaustion; the Result boundary catches it
and the arena releases all blocks without walking incomplete parser structures.
Returning null exposed unsafe upstream partial-allocation cleanup in actual
failure injection. MSVC compiles only this translation unit with `/EHc-` after
the ordinary `/EHsc`, allowing the internal C-linkage parse call to unwind.
Core/other source files retain their existing exception settings. Debug, Release
and sanitizer sweeps must validate this path and zero outstanding allocations.
Checked private integer hooks also reject malformed/out-of-range conversions,
including attribute suffixes and sampler fields outside earlier size/index
checks. Numeric rejection uses the same private unwind/arena cleanup boundary;
the standard library's unchecked `atoi`/`atoll` conversions are not used.

Normalize percent-decoded relative URIs within the logical project root before
ProjectPath validation. An explicit parent reference can stay inside the root;
escaping it rejects. Unknown schemes, network paths, malformed encoding and
ambiguous portable paths reject. Native file/alias validation remains with the
existing ProjectFiles owner when it captures bytes. Inspection is not an OS
sandbox and cannot prove the caller's snapshot came from a native file.

Unsupported required extensions fail by name. Optional extensions produce
bounded diagnostics while 12a inspects retained core source fields; complete
material/texture fallback validation belongs to 12d/12e. Skins, animations
and morph targets reject until their declared owner implements them. Source
inspection success does not establish converted geometry or decoded pixels.
The full profile requires sparse expansion in 12b; it cannot be reduced to an
optional-rejection policy merely because a parser recognizes sparse storage.

## Subsequent conversion contracts

Mesh/node/material/texture references use engine-owned indices with explicit
counts, never cgltf pointers. Their final payload layout/version lands with
actual conversion, retaining checked bounds, little-endian encoding and explicit
unsupported-version behavior. Persistent AssetId remains separate from path,
content digest, source-array index and runtime handle.

Color space is a property of a texture usage. Base-color/emissive RGB is sRGB;
metallic-roughness, normal and occlusion data is linear, and alpha remains linear.
An image reused by color and data usages requires distinct semantic processing;
its filename/MIME or one global image flag cannot choose every usage's color
space. Samplers and UV selections remain per referenced texture/material use.

12d must connect external resources through explicit declared IDs/settings and
the existing captured-source/revision/audience fences. ByteAssetImporter has a
transformable body and cannot serve as implicit identity-only raw source merely
by stripping its envelope. Choose a validated identity-only resource payload or
an explicit owner capture extension before implementing that adapter.

## Qualification and limits

The [source contract](../gltf-source.md) fixes concrete budgets. Licensed offline
fixtures, hostile sizes/URIs, sparse storage, real allocator-failure sweeps,
FeatureWorld, Windows/WSL sanitizers and protected packages qualify 12a. CPU
authoring works without Client/Editor/display/GPU. Parser headers and targets
remain absent from Runtime/Server dependency closures. Retain original asset
limits and earlier qualification evidence; full REQ-12 remains NOT_RUN until 12e.
