#pragma once

#include "shark/tools/asset_import.hpp"

namespace shark::tools {

inline constexpr std::size_t gltf_source_maximum_file_bytes = asset_import_maximum_source_bytes;
inline constexpr std::size_t gltf_source_maximum_total_bytes = asset_import_maximum_total_bytes;
inline constexpr std::size_t gltf_source_maximum_json_depth = 32;
inline constexpr std::size_t gltf_source_maximum_json_nodes = 65536;
inline constexpr std::size_t gltf_source_maximum_uri_bytes = 4096;
inline constexpr std::size_t gltf_source_maximum_resources = asset_import_maximum_nodes - 1;
inline constexpr std::size_t gltf_source_maximum_table_entries = 4096;
inline constexpr std::size_t gltf_source_maximum_total_entries = 16384;
inline constexpr std::size_t gltf_source_maximum_accessor_elements = 262144;
inline constexpr std::size_t gltf_source_maximum_total_accessor_elements = 4 * 1024 * 1024;
inline constexpr std::size_t gltf_source_maximum_parser_bytes = 32 * 1024 * 1024;
inline constexpr std::size_t gltf_source_maximum_diagnostics = 32;
inline constexpr std::size_t gltf_source_maximum_diagnostic_bytes = 1024;

enum class GltfContainer { json, binary };
enum class GltfResourceKind { buffer, image };
enum class GltfResourceStorage { external, data_uri, binary_chunk, buffer_view };

// Explicit immutable source snapshots. An external AssetId is supplied by the
// caller's catalog mapping, never invented from a path or a content digest.
struct GltfExternalSource {
    ProjectPath path;
    AssetId asset;
    std::span<const std::byte> bytes;
};
struct GltfSourceInput {
    ProjectPath path;
    std::span<const std::byte> bytes;
    std::span<const GltfExternalSource> external_sources{};
    std::stop_token stop{};
};
struct GltfResourceReference {
    GltfResourceKind kind;
    std::size_t index;
    GltfResourceStorage storage;
    std::optional<ProjectPath> path;
    std::optional<AssetId> asset;
    std::size_t byte_count;
    ContentDigest digest;
    std::string mime_type;
};
struct GltfSourceCounts {
    std::size_t scenes{};
    std::size_t nodes{};
    std::size_t meshes{};
    std::size_t primitives{};
    std::size_t accessors{};
    std::size_t materials{};
    std::size_t textures{};
    std::size_t samplers{};
};
struct GltfSourceInspection {
    GltfContainer container;
    GltfSourceCounts counts;
    std::optional<std::size_t> selected_scene;
    std::vector<GltfResourceReference> resources;
    std::vector<std::string> diagnostics;
};

// Pure CPU source inspection: bounded container/parser/accessor/reference
// validation over supplied bytes. Performs no file, network, cache or Scene I/O.
// Success describes source data, not converted geometry or decoded images.
// Paths are portable semantic references; native snapshots remain the project
// owner's responsibility. Every external snapshot must be referenced exactly by
// its canonical path, with unique nonnil AssetIds. Repeated uses share a mapping.
[[nodiscard]] Result<GltfSourceInspection> inspect_gltf_source(const GltfSourceInput& input);

} // namespace shark::tools
