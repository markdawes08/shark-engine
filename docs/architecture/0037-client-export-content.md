# ADR 0037: CPU preparation of client export content

Date: 2026-10-28. Status: accepted implementation decision for assignment 14a.

Add one EngineTools service to prepare immutable export content through the
existing AuthoredScene, AssetCatalog and AssetImports owners. The caller selects
the startup camera and supplies exact scene/catalog/cache revisions and a client
content target. The active scene must be the project's declared startup scene.
No new source parser, importer path, Runtime dependency or file format is needed.

All MeshRenderer references, including hidden ones, participate. Built-ins use
their existing reserved IDs; other meshes must come from fresh typed glTF cache
artifacts. Explicit import is a prerequisite. Querying the existing cache proves
freshness of the complete dependency closure; reading an old retained key alone
does not. A narrow context check connects the import owner's project/catalog/cache
fences even when a built-in-only scene has no import roots.

The result owns prepared scene/mesh bytes, startup identity, content digests and
source attribution for the used closure. It omits source paths, settings/extras,
raw external resources, private server data and authoring history. Deterministic
asset ordering and explicit count/byte/work bounds make it suitable for later
human/terminal adapters. Dirty source state is reported and preserved.

Preparation rechecks inputs before returning. A separate validation operation
rechecks the original candidate immediately before later package publication.
These are owner-thread observation fences with the existing cooperating-writer
assumption, not a multi-file filesystem transaction or a perpetual freshness
guarantee. Assignment 14b owns staging/publication and the public export command;
14c owns actual isolated launch. See the [contract](../client-export.md).
