# glTF images, texture uses and mip chains

Owner: 12c / REQ-12. Follow [ADR 0030](architecture/0030-bounded-gltf-textures.md),
the [source contract](gltf-source.md), [geometry contract](gltf-geometry.md) and
[five checkpoints](roadmap/gltf-plan.md). `convert_gltf_textures` is pure CPU
EngineTools code; its stable error operation is `asset.gltf.textures`.

## Input, ownership and selection

Use the same immutable `GltfSourceInput` root/external byte snapshots, portable
paths, persistent AssetIds and cooperative stop token as inspection/conversion.
All borrowed bytes remain alive and immutable throughout the call. The existing
source owner retains resolved image spans and decoded data-URI storage privately;
no borrowed view or codec pointer escapes. All complete-source container, URI,
resource, accessor, graph, signature and MIME checks remain unchanged.

An explicit scene index selects that source scene; otherwise use its declared
default, then scene zero. No scenes means no texture uses. Walk reachable nodes
iteratively, visit each referenced mesh once and collect its material indices.
Image header/decode/mip validation applies only to selected reachable texture
uses. Unused images still satisfy the earlier complete-source reference/signature
contract; they are not implicitly decoded. Geometry values and transforms are
separately qualified by `convert_gltf_geometry`, not by this image operation.

Results own the original source report, effective selected scene, semantic image
variants and usage records. Source indices are not persistent AssetIds or runtime
handles. To obtain identity/path metadata, match `source_image` against a source
report reference with kind `image` and the same source index; the resource report
also contains buffer references. Embedded data has no invented persistent ID.

No file, catalog/cache, Scene, editor, display or GPU is accessed or changed.
Existing project/import owners capture native bytes and later fence publication.
Material factors, payload serialization, identity-only external resources and
cache/job integration retain checkpoint 12d. This operation does not advertise
a terminal method or editor UI before their adapter owners in 15/16.

## Uses, variants and samplers

Output uses are sorted by source material, then base-color, metallic-roughness,
normal, occlusion and emissive role. Each records source material/texture, UV0 or
UV1, dense output image index and complete sampler state. Each selected primitive
must supply the UV set referenced by each of its core material texture links.
Missing sets, sets above one and invalid references reject.

Image variants follow first usage order and share only an identical source image
index plus processing semantic. Base-color/emissive share `srgb_color` processing;
metallic-roughness/occlusion share `linear_data`; normal maps use `normal`.
The same image in color and data roles gets separate mip chains. Different source
indices/AssetIds never collapse because their content digests happen to match.
Each unique encoded image decodes once even when it has multiple variants.

Samplers stay per use, independent of shared pixels. Preserve NEAREST/LINEAR
magnification, all six minification/mipmap combinations, and CLAMP_TO_EDGE,
MIRRORED_REPEAT or REPEAT on both axes. Absent magnification is LINEAR; absent
minification is LINEAR_MIPMAP_LINEAR; absent wrapping is REPEAT. A sampler that
does not request mip sampling still retains the complete generated chain.

## Pixel profile

PNG supports legal grayscale, RGB, palette, grayscale-alpha and RGBA layouts,
their legal 1/2/4/8/16-bit source depths and ordinary/Adam7 rows. Eight-bit and
lower depths expand to RGBA8. PNG16 becomes RGBA16 with both bytes preserved.
JPEG supports the pinned decoder's eight-bit sequential/progressive Huffman
frames and one/three/four-component layouts; arithmetic or other precision/coding
forms reject. Output is always RGBA with an opaque alpha when the source has none.

Mip bytes are tightly packed, top-left row order. `component_bits` is 8 or 16;
sixteen-bit components are least-significant byte first. These are owned CPU
values, not a raw C++ struct serialization, cooked payload envelope or guaranteed
GPU texture format. Later consumers choose their explicit storage/upload format.
Ignore embedded ICC/gamma/Exif orientation metadata; glTF usage controls color
interpretation, and no decoder flip/conversion global is changed.

Level zero retains decoded sample values. RGB stays sRGB encoded for `srgb_color`
and linear for `linear_data`/`normal`; alpha is always linear. Color mip RGB
decodes through the sRGB transfer, filters in linear light, then re-encodes.
Linear data filters each channel arithmetically. Normal mips decode RGB from
[0,1] to [-1,1], average weighted vectors, renormalize and re-encode. A vector
length at most 1e-12 uses +Z. Normal scale is not baked into these pixels.

Alpha is averaged independently with straight channels; no premultiplication,
alpha-coverage preservation or alpha-mode inference is performed. This permits
the same color processing for base-color and emissive uses. Material alpha state
and final rendered acceptance remain 12d/26.

Each next level is `max(1,floor(previous_dimension/2))`, continuing to 1x1.
An area box with exact integer overlap weights covers every prior texel, including
odd trailing rows/columns. Work is the number of positive weighted RGBA-texel
contributions, not a claim about scalar instructions. Quantize nearest with
half-up rounding; integer accumulation makes linear-data/alpha ties exact at
sixteen bits. sRGB/normal calculations use bounded double arithmetic. For an
eight-bit black/white pair, the color mip is 188 and the data mip is 128.

## Validation, budgets and failures

The private pinned [decoder integration](dependencies/stb-image.md) enables only
memory PNG/JPEG, without STDIO/HDR/float loaders/SIMD, with dimensions capped at
4096. No upstream build executes and no normal test downloads assets.

PNG preflight enforces signature/IHDR, legal layout, chunk names/lengths/CRCs,
palette/transparency ordering, contiguous image data and exact terminal IEND.
Before ordinary decoding, use the exact declared filtered-row inventory (Adam7
included) as fixed DEFLATE output, require exact stream consumption and Adler
checksum, validate row filters and reverse packed palette rows to reject every
index outside the declared palette. This scratch is separately bounded/tracked.

JPEG preflight validates marker/segment lengths, frame/scan parameters, declared
coding tables, initial component scan coverage, exact restart count/order from
interleaved or component scan grids, and exact terminal
EOI. Progressive scans charge full frame pixel work per scan. Codec rejection
fails the operation. These are structural guards plus the pinned JPEG decoder;
framing-complete entropy damage can still be tolerated by upstream. They do not
claim a general JPEG entropy conformance or authenticity check.

| Independent limit | Fixed bound |
| --- | ---: |
| Root/external encoded bytes | Earlier 1 MiB each / 16 MiB aggregate |
| Image dimension / pixels per image | 4096 / 4194304 |
| Aggregate unique decoded RGBA bytes | 32 MiB |
| Aggregate semantic mip bytes | 32 MiB |
| Logical owned output, including metadata | 36 MiB |
| Texture uses / semantic variants | 4096 / 381 |
| Live decoder allocations, including headers | 64 MiB |
| Cumulative decoder allocations per operation | 256 MiB |
| Decode/scan and weighted mip work items | 33554432 |

Inspect every selected image header and precharge decoded bytes, all variants,
full mip chains, metadata and work before the first decoder allocation. PNG
charges two decode pixel passes; JPEG charges frame pixels times scan count.
Mip work charges exact positive area overlaps. Repeated references cannot repeat
unbounded decode/mip work. Independent limits may reject before another ceiling
is reached; neither output nor allocator caps are whole-process RSS claims.
Bounded source parsing and encoded storage retain their earlier independent caps.

Nullable allocator hooks track every live block and cumulative charge. Failed
realloc preserves its original block; new scratch and realloc growth are zeroed,
and RAII reclaims leftovers after failure.
Allocation and cancellation failures return owned Errors with operation, source
path and available material/image context. Cancellation is cooperative between
bounded codec calls, framing loops and mip rows; it does not forcibly interrupt
an in-progress upstream decode. No partial result or artifact is published.

## Public CPU example

```cpp
#include <shark/tools/gltf_textures.hpp>

shark::Result<shark::tools::GltfTextures>
convert_scene_textures(const shark::ProjectPath& path,
                       std::span<const std::byte> bytes,
                       std::span<const shark::tools::GltfExternalSource> resources,
                       std::optional<std::size_t> scene) {
    return shark::tools::convert_gltf_textures({path, bytes, resources}, {scene});
}
```

The [offline corpus](../tests/fixtures/gltf_textures/README.md) has original
MIT-licensed PNG/JPEG/container inputs, expected pixels and generator/hash records.
The twelfth native FeatureWorld case exercises this same function, independent
expected mip values and preserved authored-scene identities. Source and geometry
cases, allocator controls and every earlier workload remain required.
