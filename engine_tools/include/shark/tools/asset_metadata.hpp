#pragma once

#include "shark/tools/source_document.hpp"

namespace shark::tools {

using AssetId = TypedId<struct AssetIdDomain>;
inline constexpr std::uint32_t asset_metadata_version = 1;
inline constexpr std::size_t asset_metadata_maximum_bytes = 65536;
inline constexpr std::size_t asset_maximum_dependencies = 256;
inline constexpr std::size_t asset_maximum_catalog_entries = 4096;
inline constexpr std::size_t asset_maximum_catalog_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t asset_maximum_page_size = 128;
inline constexpr std::string_view asset_development_cache_name = ".shark-development-cache";

// Content audience is authoring metadata, not permission to package private
// server content for a client. Closure/side validation belongs to later cooks.
enum class AssetAudience { client, server, shared };

struct AssetMetadata {
    ProjectId project;
    AssetId asset;
    std::string kind;
    std::string origin;
    std::string license;
    // A declared identifier, not discovery or a claim of importer support.
    std::string importer;
    AssetAudience audience{AssetAudience::shared};
    std::string settings_json{"{}"};
    std::vector<AssetId> dependencies{};
    std::string extras_json{"{}"};
    bool operator==(const AssetMetadata&) const = default;
};

// Exact schema 1 only; there is no legacy asset format. Settings/extras are
// bounded JSON objects, canonicalized on validation. Dependency IDs are sorted.
[[nodiscard]] Result<AssetMetadata> decode_asset_metadata(std::string_view text);
[[nodiscard]] Result<AssetMetadata> validate_asset_metadata(const AssetMetadata& metadata);
[[nodiscard]] Result<std::string> encode_asset_metadata(const AssetMetadata& metadata);

// Adjacent sidecars: source.ext.sharkasset. The source path is derived from its
// sidecar location, so a coordinated source+sidecar move changes no metadata ID.
[[nodiscard]] Result<ProjectPath> asset_sidecar_path(const ProjectPath& source);
[[nodiscard]] Result<ProjectPath> asset_source_path(const ProjectPath& sidecar);

} // namespace shark::tools
