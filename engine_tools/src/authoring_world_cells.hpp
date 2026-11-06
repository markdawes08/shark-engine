#pragma once

#include "shark/tools/asset_import.hpp"
#include "shark/world_cell.hpp"

#include <nlohmann/json.hpp>

namespace shark::tools {
class AuthoringAssets;

struct AuthoringCellRequest {
    SceneRevision scene_revision;
    AssetCatalogRevision catalog_revision;
    AssetCacheRevision cache_revision;
    WorldCellKey key;
    std::uint64_t content_revision;
    AssetTargetProfile target;
    std::vector<AuthoredEntityId> collision;
    std::vector<AuthoredEntityId> navigation;
};
struct AuthoringPreparedCell {
    AuthoringCellRequest request;
    bool dirty;
    std::vector<std::byte> payload;
    ContentDigest digest;
    std::vector<std::pair<AssetId, AssetCacheKey>> roots;
};

namespace authoring_cell_wire {
[[nodiscard]] Result<AuthoringCellRequest> parse(const nlohmann::json& parameters, bool publishing);
[[nodiscard]] nlohmann::json methods();
[[nodiscard]] nlohmann::json summary(const PreparedWorldCell& cell, ContentDigest digest,
                                     std::size_t bytes);
} // namespace authoring_cell_wire
} // namespace shark::tools
