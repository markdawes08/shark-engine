#pragma once

#include "shark/prepared_scene.hpp"

namespace shark {

inline constexpr std::uint32_t world_cell_payload_version = 1;
inline constexpr std::size_t world_cell_maximum_payload_bytes =
    prepared_scene_maximum_payload_bytes + 128;

// Content coordinates, independent of simulation region and authority identity.
struct WorldCellKey {
    WorldId world;
    WorldCell cell;
    auto operator<=>(const WorldCellKey&) const = default;
};

// Engine-owned static development content, not a final cooked manifest. The
// nested scene anchor must be this cell's zero-offset corner. Revision is a
// positive trusted content label, not a proof of freshness or authentication.
struct PreparedWorldCell {
    WorldCellKey key;
    std::uint64_t content_revision;
    PreparedScene scene;
    bool operator==(const PreparedWorldCell&) const = default;
};

[[nodiscard]] Result<std::vector<std::byte>> encode_world_cell(const PreparedWorldCell& cell);
[[nodiscard]] Result<PreparedWorldCell> decode_world_cell(std::span<const std::byte> payload);

// Synchronous, complete-or-error activation of one bounded static content cell.
// The fresh read-only Scene retains existing authored identities and private
// checked handles. It grants no region/player/native-physics authority. The host
// owns resident-cell limits and replacement; no existing cell changes on failure.
class LoadedWorldCell final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<LoadedWorldCell>>
    load(std::span<const std::byte> payload, std::shared_ptr<const PreparedAssets> assets);

    LoadedWorldCell(const LoadedWorldCell&) = delete;
    LoadedWorldCell& operator=(const LoadedWorldCell&) = delete;
    LoadedWorldCell(LoadedWorldCell&&) = delete;
    LoadedWorldCell& operator=(LoadedWorldCell&&) = delete;

    [[nodiscard]] const WorldCellKey& key() const& noexcept {
        return key_;
    }
    const WorldCellKey& key() const&& = delete;
    [[nodiscard]] std::uint64_t content_revision() const noexcept {
        return content_revision_;
    }
    [[nodiscard]] WorldPosition origin() const {
        return {key_.world, key_.cell, {0.0, 0.0, 0.0}};
    }
    [[nodiscard]] const Scene& scene() const& noexcept {
        return scene_->scene();
    }
    const Scene& scene() const&& = delete;
    [[nodiscard]] std::shared_ptr<const PreparedAssets> assets() const noexcept {
        return scene_->assets();
    }

  private:
    LoadedWorldCell(WorldCellKey key, std::uint64_t content_revision,
                    std::unique_ptr<LoadedPreparedScene> scene)
        : key_(key), content_revision_(content_revision), scene_(std::move(scene)) {}

    WorldCellKey key_;
    std::uint64_t content_revision_;
    std::unique_ptr<LoadedPreparedScene> scene_;
};

} // namespace shark
