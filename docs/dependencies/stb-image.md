# stb_image dependency review

Owner: assignment 12c / EngineTools only. The selected decoder is upstream
[stb_image 2.30](https://github.com/nothings/stb/blob/003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5/stb_image.h),
from [commit 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5](https://github.com/nothings/stb/tree/003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5).
Review date: 2026-10-27. The commit was published on 2026-10-23; the selected
header's latest upstream change is 2024-05-31. This follows ADR 0002's declared
decoder choice. No patch or transitive dependency is selected.

The immutable commit archive is 1,515,348 bytes, SHA-256
`a6c31950b3bdf05eb92258768ba2a11bd44449490574b1aac67028452288c40d`.
[The dependency lock](../../dependencies.lock.json) records its exact prefix and
all 431 regular members' SHA-256 hashes. The header is 283,010 bytes, SHA-256
`594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`.
The archive has 459 total members and 5,129,455 expanded regular-file bytes.
Only `stb_image.h` is compiled; other libraries, tools, fixtures and build files
in the upstream archive are unused and never executed.

Shark selects the MIT option from the upstream
[MIT/public-domain offer](https://github.com/nothings/stb/blob/003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5/LICENSE).
The complete `LICENSE` and embedded `stb_image.h` notice remain byte-for-byte in
the verified source population. Any future authoring-tool distribution must
retain both. Runtime and Server do not distribute this dependency.

`SharkStbImage` / `Shark::StbImage` is an authoring interface target with one
SYSTEM include directory, privately linked by EngineTools. Upstream CMake,
examples and tests never run. Tools-OFF configurations ignore unnecessary
decoder archive/override inputs and create neither the target nor its source
population. Public engine headers expose no decoder types or includes.

The private implementation uses the memory loader and engine allocation hooks;
PNG/JPEG are the only enabled formats. PNG retains legal 1/2/4/8/16-bit source
channels and Adam7 interlacing; 16-bit data remains RGBA16. JPEG is eight-bit
baseline/progressive. File I/O, HDR, floating-point conversion and SIMD are disabled. Engine preflight enforces encoded size, supported image
containers/bit depth, dimensions, pixel count and decoded-byte bounds before
decoding, with `STBI_MAX_DIMENSIONS=4096` and checked decoder output.
`STB_IMAGE_IMPLEMENTATION`, `STBI_ONLY_PNG`, `STBI_ONLY_JPEG`, `STBI_NO_STDIO`,
`STBI_NO_HDR`, `STBI_NO_LINEAR` and `STBI_NO_SIMD` are private source definitions;
`STBI_MALLOC`, `STBI_REALLOC_SIZED` and `STBI_FREE` provide tracked engine allocation.
Allocation failures must return through the owning Result boundary with no
outstanding decoder allocations. Upstream format recognition does not expand
Shark's declared source profile, and color-space/mip semantics remain owned by
the engine. Radiance environment import remains assignment 27.

Upstream's [maintenance notice](https://github.com/nothings/stb/blob/003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5/README.md)
states that security fixes may take time to merge. The fixed private profile,
bounded allocator, hostile local fixtures and sanitizer qualification are
required integration controls; this pin does not claim a general-purpose image
service or safety for arbitrary future formats.

Offline builds pass `SHARK_IMAGE_ARCHIVE`, reusing the canonical archive under
`out/dependency-cache/`. `tools/image_dependency.py` verifies the complete archive
and populated inventory before each configure. It rejects unsafe paths, links,
duplicate files and archive/source drift. Limits are 2 MiB encoded, 8 MiB
expanded, 512 regular files and 1,024 total archive members. Explicit online
preparation reads at most the locked byte count plus one and atomically publishes
only verified bytes. Ordinary dependency tests use local archives and mocked
downloads.
