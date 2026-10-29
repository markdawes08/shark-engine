#pragma once

#include "shark/prepared_assets.hpp"
#include "shark/scene.hpp"
#include "shark/world_position.hpp"

#include <array>
#include <span>

namespace shark {

inline constexpr std::size_t render_scene_maximum_entities = 4096;
inline constexpr std::size_t render_scene_maximum_scanned_slots = 65536;
inline constexpr std::size_t render_scene_maximum_bounds_vertices = 1'048'576;
inline constexpr std::uint32_t render_viewport_maximum_extent = 16384;
inline constexpr double render_coordinate_limit = 65536.0;

struct RenderViewport {
    std::uint32_t width{1280};
    std::uint32_t height{720};
    bool operator==(const RenderViewport&) const = default;
};

// The anchor identifies scene-local zero; never add its cells to a matrix.
// Generation is caller-owned, nonzero, and must change when this origin changes.
struct RenderOrigin {
    math::Vec3 scene_local_offset{0.0};
    std::uint64_t generation{1};
    std::optional<WorldPosition> world_anchor{};
    bool operator==(const RenderOrigin&) const = default;
};

struct RenderBounds {
    math::Vec3 minimum{0.0};
    math::Vec3 maximum{0.0};
    bool operator==(const RenderBounds&) const = default;
};
struct RenderPlane {
    math::Vec3 normal{0.0};
    double distance{0.0};
    // Absolute construction error in signed distance over the render coordinate cube.
    double uncertainty{0.0};
};
struct RenderCameraMatrices {
    math::Mat4 view{1.0};
    math::Mat4 projection{1.0};
    math::Mat4 view_projection{1.0};
    // Inward normalized planes: left, right, clip minimum-Y, clip maximum-Y, near, far.
    std::array<RenderPlane, 6> frustum{};
    bool mirrored{false};
};

// Finite RH projection, Vulkan depth [0,1], one projection-Y flip. Camera world
// scale/shear is retained by an exact affine inverse, not decomposed away.
[[nodiscard]] Result<RenderCameraMatrices> make_render_camera(const Camera& camera,
                                                              const math::Mat4& scene_local_world,
                                                              RenderViewport viewport,
                                                              const RenderOrigin& origin = {});

struct RenderRecord {
    EntityReference entity;
    std::shared_ptr<const PreparedMeshAsset> asset;
    std::uint32_t mesh_index;
    math::Mat4 world_matrix;
    // Affine identity bottom row/column, upper 3x3 inverse-transpose linear part.
    math::Mat4 normal_matrix;
    RenderBounds local_bounds;
    RenderBounds world_bounds;
    // Model parity. Raster front-face reversal also XORs camera.mirrored.
    bool mirrored;
};
struct RenderExtractionStats {
    std::size_t entities{0};
    std::size_t mesh_instances{0};
    std::size_t hidden{0};
    std::size_t culled{0};
    std::size_t visible{0};
    std::size_t unique_meshes{0};
    std::size_t bounds_vertices{0};
};
// Display-only camera values. They do not replace the selected source camera,
// mutate a Scene or become source content. The complete world matrix retains
// its affine scale/shear and is validated through make_render_camera.
struct RenderViewCamera {
    Camera camera;
    math::Mat4 scene_local_world{1.0};
};
// Display-only local transform for one live entity and its descendants. The
// source revision and entity identities remain committed observations; this
// override is provenance, not a mutation or authorization to commit one.
struct RenderTransformOverride {
    EntityHandle entity;
    Transform local;
    bool operator==(const RenderTransformOverride&) const = default;
};
struct RenderExtractionOptions {
    EntityHandle camera;
    SceneRevision expected_revision;
    RenderViewport viewport{};
    RenderOrigin origin{};
    bool frustum_culling{true};
    std::optional<RenderViewCamera> view_camera{};
    std::optional<RenderTransformOverride> transform_preview{};
};

class RenderScene final {
  public:
    [[nodiscard]] SceneRevision revision() const noexcept {
        return revision_;
    }
    [[nodiscard]] const EntityReference& camera_entity() const& noexcept {
        return camera_entity_;
    }
    const EntityReference& camera_entity() const&& = delete;
    [[nodiscard]] const RenderCameraMatrices& camera() const& noexcept {
        return camera_;
    }
    const RenderCameraMatrices& camera() const&& = delete;
    [[nodiscard]] RenderViewport viewport() const noexcept {
        return viewport_;
    }
    [[nodiscard]] const RenderOrigin& origin() const& noexcept {
        return origin_;
    }
    const RenderOrigin& origin() const&& = delete;
    [[nodiscard]] std::span<const RenderRecord> records() const& noexcept {
        return records_;
    }
    std::span<const RenderRecord> records() const&& = delete;
    [[nodiscard]] RenderExtractionStats stats() const noexcept {
        return stats_;
    }
    [[nodiscard]] const std::optional<RenderTransformOverride>&
    transform_preview() const& noexcept {
        return transform_preview_;
    }
    const std::optional<RenderTransformOverride>& transform_preview() const&& = delete;

  private:
    friend Result<RenderScene> extract_render_scene(const Scene&, const PreparedAssets&,
                                                    const RenderExtractionOptions&);
    RenderScene(SceneRevision revision, EntityReference camera_entity, RenderCameraMatrices camera,
                RenderViewport viewport, RenderOrigin origin)
        : revision_(revision), camera_entity_(std::move(camera_entity)), camera_(std::move(camera)),
          viewport_(viewport), origin_(std::move(origin)) {}
    SceneRevision revision_;
    EntityReference camera_entity_;
    RenderCameraMatrices camera_;
    RenderViewport viewport_;
    RenderOrigin origin_;
    std::vector<RenderRecord> records_;
    RenderExtractionStats stats_;
    std::optional<RenderTransformOverride> transform_preview_;
};

// Reads one exact committed revision on the Scene owner thread; pending writes
// are not applied. Result owns values/immutable leases, never Scene pointers.
[[nodiscard]] Result<RenderScene> extract_render_scene(const Scene& scene,
                                                       const PreparedAssets& assets,
                                                       const RenderExtractionOptions& options);

} // namespace shark
