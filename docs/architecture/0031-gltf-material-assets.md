# ADR 0031: Static glTF materials and asset integration

Date: 2026-10-27. Owner: 12d / REQ-12. Status: accepted.

## Decision

Extend the existing bounded source owner with core metallic-roughness material
conversion. Preserve factors, texture/UV links, normal scale, occlusion strength,
emissive, alpha mode/cutoff and double-sided state as CPU values. Selected explicit
materials retain their source indices; a primitive without a material uses core
defaults without inventing a source identity. Optional texture-source extensions
may be ignored when a usable core fallback exists. Unsupported extension-only
images must not invalidate that fallback; images referenced by core remain subject
to the existing source and decoder checks.

Serialize converted geometry, hierarchy, materials, semantic mip chains and
source correspondence in a separately versioned, bounded engine-owned payload.
This is a development asset format, not final cooked distribution or a GPU layout.
The existing 2 MiB per-artifact and 16 MiB closure limits remain unchanged. Larger
pure CPU conversions may succeed yet fail to fit a development cache artifact;
report that failure and preserve the last-valid cache output.

Register `shark.gltf` and the identity-only `shark.gltf-resource` through the
existing AssetImports registry. Add child artifact descriptor/source-digest views
to AssetImportDependency. glTF requires that typed resource contract and verifies
its asset identity, version, raw-byte length and digest. The earlier `shark.bytes`
format is insufficient: its body may be transformed, so stripping its header
cannot establish original source bytes.

Importer settings explicitly map logical resource paths to persistent AssetIds
and declare a logical root path and optional scene. These paths interpret glTF
URIs; they are not instructions to open files. AssetImports still captures actual
catalog sources through ProjectFiles. Moving those sources with their sidecars
retains identity and does not invalidate a key merely because a physical path
changed. Changing a logical mapping is a settings change and changes the key.

## Consequences

Human and terminal adapters can use the same pure converters and import owner.
No additional scene owner, worker pool, file resolver or cache publication path
is introduced. The existing owner performs native reads, revision checks, audience
checks and atomic publication; callbacks receive immutable captured values only.
Dependency keys, settings, cancellation and stale completion retain their original
meaning. Runtime, Core and Server do not acquire source decoders or EngineTools.

Rendering remains 13/26, adapters 15/16, cooked distribution 31 and animation M06.
The [contract](../gltf-assets.md) defines the public boundary. Verification follows
the [testing policy](../testing-policy.md); 12e closes integrated REQ-12 acceptance.
