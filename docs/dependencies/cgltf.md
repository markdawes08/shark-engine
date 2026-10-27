# cgltf dependency review

Owner: assignment 12 / EngineTools only. The selected parser is upstream
[cgltf v1.15](https://github.com/jkuhlmann/cgltf/releases/tag/v1.15), resolved to
[commit 360db1a95480fe102ae9c69b27c5d101167ff5ba](https://github.com/jkuhlmann/cgltf/tree/360db1a95480fe102ae9c69b27c5d101167ff5ba).
The exact commit archive, its size and digest, and the reviewed header/license
hashes are pinned in [the dependency lock](../../dependencies.lock.json).
Review date: 2026-10-27. No patches or transitive dependencies are selected.

The upstream [MIT license](https://github.com/jkuhlmann/cgltf/blob/360db1a95480fe102ae9c69b27c5d101167ff5ba/LICENSE)
and embedded header notice are retained byte-for-byte in the verified source
population. Any future authoring-tool distribution must include that exact notice.
Runtime and Server must contain neither the parser nor its source dependency.

`SharkCgltf` / `Shark::Cgltf` is a private authoring interface target. It supplies
one system include directory; upstream build files, examples and tests never run.
EngineTools owns the single implementation translation unit. Shared public
headers expose engine-owned types only. Protected Tools-OFF builds ignore cgltf
archive/override variables and never populate its sources or target.

The [reviewed header](https://github.com/jkuhlmann/cgltf/blob/360db1a95480fe102ae9c69b27c5d101167ff5ba/cgltf.h)
provides `cgltf_options.memory` allocation/free callbacks and memory-only
`cgltf_parse`. External buffers/images are not loaded by that parsing call.
The integration supplies bounded memory and validated, captured resources;
`cgltf_parse_file` and the default filesystem buffer loader are not public engine
paths. Parser recognition does not establish Shark's extension, image, geometry,
rendering or decompression support. Product validation and limits remain required.

The implementation is compiled privately as C++. Its bounded allocator throws a
private abort on failure, caught immediately around `cgltf_parse`; an intrusive
RAII arena reclaims incomplete allocations without traversing partial parser
objects. This avoids upstream null-allocation cleanup through uninitialized
arrays. No exception escapes the public inspection API or reaches external C
code. The upstream header remains unmodified.

Private `CGLTF_ATOI`/`CGLTF_ATOLL` hooks use `std::from_chars` with exact range and
token checks. Integer rejection uses the same controlled unwind and arena cleanup
as allocation aborts. This covers standard-attribute suffixes and other values
not constrained by earlier accessor-size validation. Sampler domains are checked
before the vendor's enum casts. Source limits alone do not make `atoi` overflow
or an out-of-domain C++ enum conversion safe.

On MSVC only `gltf_source.cpp` adds `/EHc-` after the existing `/EHsc`. This keeps
synchronous C++ unwinding while allowing the private allocator abort through
cgltf's C-linkage declaration. Microsoft's
[/EH documentation](https://learn.microsoft.com/en-us/cpp/build/reference/eh-exception-handling-model)
states that `/EHc` assumes C-linkage functions do not throw; compiling header
implementation as C++ alone does not change that assumption. The independent
dependency fixture checks flag ordering and absence of this override on other
translation units. Allocation sweeps and sanitizer checks qualify actual cleanup.

Use `SHARK_CGLTF_ARCHIVE` for offline builds. The current machine keeps its one
archive under `out/dependency-cache/`; pass that same path to all active profiles.
The population helper rejects archive/hash/path/type/size drift and compares the
complete populated inventory with the locked archive before every configure.
Online preparation reads at most the exact locked archive byte count plus one,
then verifies size/hash and all reviewed members before atomic cache publication.
Archive processing is bounded to 1 MiB encoded, 2 MiB expanded, 128 regular files
and 256 total members. Ordinary dependency tests stay offline.
