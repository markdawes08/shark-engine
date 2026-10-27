# ADR 0030: bounded glTF images and semantic texture mips

Date: 2026-10-27. Status: accepted for checkpoint 12c; execution evidence is
recorded separately in its handoff and receipt. Follow the
[five checkpoints](../roadmap/gltf-plan.md), [source boundary](0028-bounded-gltf-sources.md),
[geometry boundary](0029-bounded-gltf-geometry.md) and [texture contract](../gltf-textures.md).

## Decision

Use the exactly pinned stb_image privately in a single EngineTools translation
unit, with memory-only PNG/JPEG decoding. PNG low-depth and palette samples
expand to RGBA8; PNG16 retains RGBA16 precision. JPEG has eight-bit components.
Radiance HDR remains assignment 27. Source image codecs never enter Core,
Server, Runtime or the public header dependency surface.

The pure texture operation reuses the existing source owner and its complete
bounded source checks. Retain resolved image bytes on that owner for the call;
do not add a file callback or expose a project-file bypass. Collect core texture
uses from reachable materials in the selected scene, checking every selected
primitive's referenced UV sets. Complete material conversion, external resource
identity integration and cache/job publication remain 12d.

Inspect all selected image headers and precharge unique decoded bytes, semantic
mip variants, metadata and work before the first decoder allocation. Preserve
all previous source and geometry limits. Bound decoder live and cumulative
allocations separately using nullable intrusive tracking. Failed realloc leaves
its original block alive; RAII releases every remaining block even when upstream
cleanup loses a partial allocation. No exception crosses decoder allocator hooks.

## Format and processing policy

PNG preflight checks complete framing, chunk CRCs, legal dimensions/layout,
transparency/palette rules, contiguous data and terminal IEND. Inflate into an
exact precomputed row inventory, including Adam7, and verify DEFLATE consumption,
Adler checksum, filters and effective palette indices before ordinary decode.
This keeps over-expanded rows and undeclared palette entries from reaching the
decoder's permissive paths.

JPEG preflight validates complete marker/segment framing, supported frame/scan
parameters, declared coding tables, initial component scans, exact restart
inventory/order from scan MCU grids, and terminal EOI. All decoder scratch and
realloc growth is initialized before use. Progressive scan work is independently charged. This is structural
validation plus the pinned decoder, not a second JPEG entropy validator; some
framing-complete damaged entropy may still be tolerated by upstream. Do not
promise general image conformance or corruption detection beyond these checks.

Return owned top-left RGBA bytes, eight-bit or explicit little-endian sixteen-bit.
Keep the source image index separate from persistent AssetId and dense variant
references. Deduplicate variants only by source image plus processing semantic.
Sampler state stays on each usage, so changing wrap/filter never changes image
identity or forces redundant pixel storage.

Base-color/emissive RGB uses sRGB encoding; generate its mips through linear
light and re-encode. Metallic-roughness/occlusion uses linear channel arithmetic.
Normal mips average signed vectors, renormalize and use +Z on cancellation.
Alpha is independently linear and straight, with no premultiplication or
alpha-coverage adjustment. Material normal scale and alpha modes remain 12d/26.

Always build a complete mip chain down to 1x1. Use exact integer area-overlap
weights so odd dimensions retain every texel. Quantize nearest with half-up
rounding; data and alpha sums use integer arithmetic to retain sixteen-bit ties.
Default filters are LINEAR magnification and LINEAR_MIPMAP_LINEAR minification,
with REPEAT wrapping; absent filter selection is an explicit engine policy.

## Qualification and later owners

Licensed original offline fixtures provide independently expected color/data/
normal results, PNG precision/layout/interlace and JPEG frame examples. Preserve
the source/geometry families, allocation sweeps and earlier workloads. Connect
the shared operation to FeatureWorld and both eventual authoring adapters.

The [texture contract](../gltf-textures.md) fixes independent budgets and the
public C++ example. Owned CPU bytes are intermediate values, not a new cooked
payload or guaranteed GPU format. Rendering, material assembly, publication and
complete optional-extension fallback keep their later checkpoint owners. Full
REQ-12 remains NOT_RUN until 12e.
