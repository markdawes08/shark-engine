# ADR 0004: Formats, identity and compatibility

Status: accepted contract; formats are implemented in their owning assignments.
Date: 2026-10-17.

## Source and derived data

Use readable UTF-8 JSON with explicit `format` and integer `schema_version` fields.
Reserve `.sharkproject`, `.sharkscene`, `.sharkprefab` and `.sharkmaterial` for
project, scene, prefab and material source documents. Their initial production
schemas are independently versioned at 1. This decision specifies their envelopes
and compatibility rules; field schemas and fixtures land in 08, 21 and 26.
Lua source is UTF-8 `.lua`; project settings declare the Lua API version separately.
Human formatting is not identity and is not a cache key by itself.

| Layer | Contract | Owner |
| --- | --- | --- |
| Authoring source | IDs, relative source references, component schema versions; readable and durable | 08 |
| Asset metadata | Persistent asset ID, source origin/license, import settings and dependency IDs in versioned JSON sidecars | 11 |
| Derived development cache | Disposable engine-owned binary payloads and JSON index keyed by input/settings/tool/dependency hashes and target profile | 11/12/14 |
| Cooked assets | Validated binary payloads with magic, kind, independent version, bounded lengths/offsets and content digest; little-endian on disk | 31 |
| Cook manifest | Versioned JSON mapping IDs to payloads, sizes, digests, dependencies and target profile, startup scenes and explicit dynamic roots | 31 |
| Authoring protocol | JSON-RPC 2.0 over local stdio plus a separately versioned Shark method/schema contract, initially 1 | 15 |
| Lua API | Engine-owned surface, API version 1; not the Lua language version | 19 |

Never serialize raw C++ structs, pointers, EnTT indices, compiler ABI layouts or
padding. Encoders use explicit widths and byte order. Runtime validates cooked
headers, kind, versions, offsets, counts and dependency references before making
resources visible. Container and per-kind payload versions are independent from
source schema and engine release versions. Hashes detect inconsistent payloads;
they are not a signature or a hostile-content sandbox.

Assignment 14 stages only validated development payloads and runtime scene data
prepared by EngineTools. It is a temporary, explicitly versioned export mode.
Assignment 31 replaces it with cooked dependency closure, including script-selected
assets declared as explicit roots. Final Runtime excludes source project/scene
readers, external importers, cookers and offline source shader compilers such as
DXC. Qualified Vulkan-on-Metal translation libraries and system driver compilation
remain permitted runtime dependencies; they must not require an installed
developer SDK. Source Lua scripts may
ship as script payloads for portability; no native Lua bytecode compatibility is
promised between CPU architectures or Lua releases.

## Stable identity and references

Persistent project, scene, entity, asset and prefab-local IDs are randomly generated
128-bit UUIDv4 values from OS entropy, serialized as lowercase hyphenated strings.
Tests use an injectable seeded ID generator and fixed imported IDs so creation
and callback order can be replayed; production does not use that test seed.
Reject nil/invalid IDs
where a reference is required and duplicate IDs within their defining namespace.
IDs survive rename, reparent and save/load; paths and content hashes are not IDs.
References between scenes carry scene plus entity ID. Asset metadata moves with
its source; importing an unrecognized duplicate sidecar must diagnose a collision.

Runtime entity handles contain a scene-instance generation, entity slot and slot
generation. Each dereference checks all three. Scene unload invalidates its
instance generation; slot reuse increments generation. Retire a slot or scene
token rather than wrapping a generation into a valid old handle. Serialized IDs
resolve to handles only within the active scene. Lua, physics user data and GPU
selection records use checked IDs/handles, never borrowed component pointers.
Prefab instances remap every local entity ID and internal reference atomically;
references outside the prefab remain asset or explicitly scoped scene references.

## Validation and version policy

Unknown future source versions, unknown components and unknown source fields
are errors, not silently omitted data. Explicit `extras` metadata may be preserved
opaquely with size limits. Optional absent fields receive schema-defined defaults.
Reject duplicate JSON keys, NaN/infinity, invalid UTF-8, invalid references,
excessive nesting and out-of-range values. Assignment 08 sets and tests concrete
limits before exposing a reader.

Every source version upgrade provides a tested, sequential migration or explicitly
rejects that old version with an actionable diagnostic. Assignment 08 includes a
real version-0 fixture and 0-to-1 migration; version 0 is a test/legacy contract,
not a claim that files exist now. Migrate in memory; save only on an explicit
successful operation. Preserve the original on migration failure. Cooked versions
are exact-match only: incompatible cooked data requires recooking, not runtime
migration. Protocol and Lua API major mismatch rejects initialization/discovery
with supported version details. Public changes update the ADR and manifests.

Resolve project paths relative to a canonical project root, with forward slashes
in documents. Reject absolute/drive/UNC paths, traversal and symlink/reparse-point
escapes for project-scoped operations. Detect case-folded path collisions for
portable packages; preserve Unicode names. Normal loading never fetches network
URIs. Never join untrusted relative strings without validating the resolved path.

Validate a complete candidate scene/project before replacing the active one.
Save to a temporary sibling, flush, and atomically replace with platform-specific
durability/error handling; a failed write preserves the prior valid file. Cache,
cook and export publication follow the same staging/commit principle. These
failure paths are exercised in 08, 11, 30 and 31.
