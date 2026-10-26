#pragma once

#include "shark/tools/asset_import.hpp"

namespace shark::tools::detail {

struct CacheBinding {
    AssetId asset;
    AssetTargetProfile target;
    AssetCacheKey key;
    bool operator==(const CacheBinding&) const = default;
};
struct CacheState {
    ProjectId project;
    std::vector<AssetArtifact> artifacts;
    std::vector<CacheBinding> bindings;
    bool operator==(const CacheState&) const = default;
};
struct CacheIndexEntry {
    AssetArtifact artifact;
    std::size_t offset;
    std::size_t length;
};
struct CacheIndex {
    ProjectId project;
    std::vector<CacheIndexEntry> entries;
    std::vector<CacheBinding> bindings;
};

[[nodiscard]] Result<std::string> canonical_import_object(std::string_view text);
[[nodiscard]] Result<std::string> normalize_byte_settings(std::string_view text);
[[nodiscard]] Result<std::string> encode_cache_index(const CacheIndex& index);
[[nodiscard]] Result<CacheIndex> decode_cache_index(std::string_view text);
[[nodiscard]] Result<std::string> encode_cache(const CacheState& state);
[[nodiscard]] Result<CacheState> decode_cache(std::string_view bytes, ProjectId project);
[[nodiscard]] Result<void> validate_target(const AssetTargetProfile& profile);
[[nodiscard]] Result<void> validate_importer_info(const AssetImporterInfo& info);
[[nodiscard]] Result<AssetCacheKey> import_key(ProjectId project, const AssetArtifact& artifact);
[[nodiscard]] Result<ContentDigest> hash_text(std::string_view text);

} // namespace shark::tools::detail
