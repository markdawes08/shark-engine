#pragma once

#include "shark/asset_id.hpp"

#include <cstdint>

namespace shark {

enum class CameraProjection : std::uint32_t { perspective = 0, orthographic = 1 };

// Presentation-only source values. Aspect ratio comes from the viewport; the
// entity Transform supplies the camera pose in the scene's local frame.
struct Camera {
    CameraProjection projection{CameraProjection::perspective};
    double vertical_fov_radians{1.0471975511965976};
    double orthographic_height{10.0};
    double near_clip{0.1};
    double far_clip{1000.0};
    bool operator==(const Camera&) const = default;
};

// Select a whole mesh (including its primitives) inside a prepared asset. Asset
// existence and mesh bounds are checked by the later loading owner, not Scene.
// StableId construction already rejects nil/non-UUIDv4 identities.
struct MeshRenderer {
    AssetId asset;
    std::uint32_t mesh_index{0};
    bool visible{true};
    bool operator==(const MeshRenderer&) const = default;
};

[[nodiscard]] Result<Camera> validate_camera(Camera camera);
[[nodiscard]] Result<MeshRenderer> validate_mesh_renderer(MeshRenderer renderer);

} // namespace shark
