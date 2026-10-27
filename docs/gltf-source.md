# Bounded glTF source inspection

Owner: 12a / REQ-12. Follow [ADR 0028](architecture/0028-bounded-gltf-sources.md),
the [declared static profile](architecture/0006-v1-profile.md) and
[five checkpoints](roadmap/gltf-plan.md). `inspect_gltf_source` is a pure CPU
EngineTools function. Geometry conversion, image decoding and material/cache
publication follow in 12b/12c/12d; full REQ-12 remains NOT_RUN until 12e.

## Inputs, ownership and results

`GltfSourceInput` contains the portable `.gltf`/`.glb` path, an immutable source
span, explicit `GltfExternalSource` snapshots and a cooperative stop token.
External snapshots carry portable paths, nonnil persistent AssetIds and immutable
bytes. Snapshot mappings have unique paths/IDs, reject ambiguous ASCII-case
collisions and must all be used. Multiple URI uses can share the same mapping.
No current directory, filesystem callback, network resolver or implicit fallback
is consulted. Every borrowed span must remain valid and immutable for the call.

Results own container/count/default-scene information, buffer/image references,
resource lengths/digests and diagnostics. External references retain the supplied
AssetId/path; embedded references have neither invented path nor invented ID.
Content digests describe bytes, not persistent identity or authenticity. No third
party object or borrowed pointer escapes. Results publish nothing and need no
scene owner, editor, device or asset cache. Failures are owned `Error` values with
operation `asset.gltf.inspect` and source/field context.

Native capture belongs to the existing project/import owner: validate actual
paths through ProjectFiles and retain original content revisions. This pure API
checks semantic paths and supplied bytes; it cannot establish native alias or
snapshot provenance. Worker callbacks must not acquire raw file access.

## Bounded source support

Accept static glTF 2.0 JSON and GLB version 2 with checked container lengths,
ordered/aligned chunks and bounded resource bytes. Local external buffers/images,
embedded buffer views and base64 data URIs are resolved from supplied data.
PNG/JPEG MIME declarations describe source image bytes; they do not imply decoded
pixels. Validate buffer views, accessor component packing/strides/ranges and
sparse storage/range/ordered-index checks before cgltf validation/data walks.

Percent-decode URI escapes, reject malformed UTF-8/NUL/control/backslash/scheme/
network/query/fragment forms, normalize relative dot segments and then validate
ProjectPath. `models/static.gltf` may reference `../textures/a.png`; walking
beyond the logical project root rejects. No percent-encoded escape bypasses that
policy. Required extensions reject by name; optional extensions produce bounded
diagnostics. 12a checks core source fields; full usable material/texture fallback
and extension-only resource reachability remain 12d/12e. Unsupported skins/animations/morph
content rejects. Camera/light content has explicit diagnostics, retaining later
engine component ownership.

| Bound | Fixed limit |
| --- | ---: |
| Encoded root or each external snapshot | 1 MiB |
| Supplied encoded aggregate | 16 MiB |
| External snapshots / combined buffer and image references | 127 |
| Resolved resource aggregate | 16 MiB |
| JSON depth / nodes (including keys) | 32 / 65536 |
| Ordinary URI / JSON key | 4096 bytes |
| JSON data-URI value | At most the 1 MiB source bound |
| Each source table / combined table entries | 4096 / 16384 |
| Accessor elements | 262144 |
| Sum of accessor element counts | 4194304 |
| Retained sparse-index storage | At most 16 MiB under the aggregate element bound |
| Live cgltf allocator bytes | 32 MiB |
| Diagnostics | 32, each at most 1024 bytes |

Charge inputs before parsing/copying, use checked arithmetic, and reject excess
before allocation or iteration. The bounded source SAX pass rejects duplicate
decoded JSON keys, nonfinite/overflowing values and excessive nesting. Existing
project/scene parser limits stay unchanged. Allocation failures and cancellation
return owned failures and release all cgltf allocations; they cannot publish a
partial artifact. Concrete source-conversion bounds may be added in later
checkpoints without silently weakening these limits.

The aggregate accessor bound charges logical counts before sparse-index storage
or iteration; reusing a buffer cannot multiply unbounded private allocations.
Effective primitive-index maxima are computed at most once per accessor, so many
primitives cannot repeatedly walk the same large index stream. Resolved-byte
charges include every reported reference, including repeated image/view uses;
the parser arena limit is separate from these bounded working buffers and the
JSON DOM, and does not claim a 32 MiB total-process memory limit.

Private cgltf integer hooks use checked conversion rather than `atoi`/`atoll`.
Out-of-range sampler fields and standard-attribute suffixes reject through the
same controlled C++ unwind/arena boundary, including fields whose final material
semantics belong to later conversion. No unchecked vendor integer conversion
is permitted merely because source bytes and JSON tokens fit their budgets.

## Public CPU example

The caller supplies captured bytes; this function opens no paths:

```cpp
#include <shark/tools/gltf_source.hpp>

shark::Result<shark::tools::GltfSourceInspection>
inspect_static_source(const shark::ProjectPath& source_path,
                      std::span<const std::byte> source,
                      std::span<const shark::tools::GltfExternalSource> resources) {
    return shark::tools::inspect_gltf_source({source_path, source, resources});
}
```

Use the same function for later human/terminal adapters. Image usage roles,
reference semantics and the existing owner/publication policy are specified by
ADR 0028; no terminal protocol or editor UI is introduced by this checkpoint.
