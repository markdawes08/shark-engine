#pragma once

#include "shark/prepared_assets.hpp"
#include "shark/scene.hpp"
#include "shark/world_position.hpp"

namespace shark {

inline constexpr std::uint32_t prepared_scene_version = 1;
inline constexpr std::size_t prepared_scene_maximum_payload_bytes = 4 * 1024 * 1024;
inline constexpr std::size_t prepared_scene_maximum_decoded_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t prepared_scene_maximum_entities = 4096;
inline constexpr std::size_t prepared_scene_maximum_hierarchy_depth = 256;

// Engine-owned development values. Source extras/project paths, authoring
// history, handles and authority/session records are deliberately absent.
struct PreparedEntity {
    AuthoredEntityId id;
    std::optional<AuthoredEntityId> parent{};
    Name name{};
    Transform transform{};
    std::optional<Camera> camera{};
    std::optional<MeshRenderer> mesh_renderer{};
    bool operator==(const PreparedEntity&) const = default;
};

struct PreparedScene {
    SceneId scene;
    std::optional<WorldPosition> world_origin;
    std::vector<PreparedEntity> entities;
    bool operator==(const PreparedScene&) const = default;
};

// Independently versioned little-endian payload, not JSON source or final cook.
// Both directions validate the complete candidate and canonicalize local TRS.
// Asset existence is checked separately by LoadedPreparedScene::load.
[[nodiscard]] Result<std::vector<std::byte>> encode_prepared_scene(const PreparedScene& scene);
[[nodiscard]] Result<PreparedScene> decode_prepared_scene(std::span<const std::byte> payload);

// Loading prepares a fresh Core Scene and checks every referenced mesh before
// returning ownership. Failure cannot change an already loaded scene. Reload is
// explicit replacement by the caller after this factory succeeds.
class LoadedPreparedScene final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<LoadedPreparedScene>>
    load(std::span<const std::byte> payload, std::shared_ptr<const PreparedAssets> assets);

    LoadedPreparedScene(const LoadedPreparedScene&) = delete;
    LoadedPreparedScene& operator=(const LoadedPreparedScene&) = delete;
    LoadedPreparedScene(LoadedPreparedScene&&) = delete;
    LoadedPreparedScene& operator=(LoadedPreparedScene&&) = delete;

    [[nodiscard]] Scene& scene() & noexcept {
        return *scene_;
    }
    [[nodiscard]] const Scene& scene() const& noexcept {
        return *scene_;
    }
    Scene& scene() && = delete;
    const Scene& scene() const&& = delete;
    [[nodiscard]] const std::optional<WorldPosition>& world_origin() const& noexcept {
        return world_origin_;
    }
    const std::optional<WorldPosition>& world_origin() const&& = delete;
    [[nodiscard]] std::shared_ptr<const PreparedAssets> assets() const noexcept {
        return assets_;
    }

  private:
    LoadedPreparedScene(std::unique_ptr<Scene> scene, std::optional<WorldPosition> world_origin,
                        std::shared_ptr<const PreparedAssets> assets)
        : scene_(std::move(scene)), world_origin_(std::move(world_origin)),
          assets_(std::move(assets)) {}

    std::unique_ptr<Scene> scene_;
    std::optional<WorldPosition> world_origin_;
    std::shared_ptr<const PreparedAssets> assets_;
};

} // namespace shark
