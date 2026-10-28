#pragma once

#include "shark/scene_render.hpp"

namespace shark::render_detail {
struct RenderTransform {
    math::Mat4 world;
    math::Mat4 normal;
    bool mirrored;
};
[[nodiscard]] Result<RenderTransform> relative_transform(const math::Mat4& scene_local_world,
                                                         const RenderOrigin& origin);
[[nodiscard]] Result<RenderBounds> transformed_bounds(const RenderBounds& local,
                                                      const math::Mat4& render_world);
// Conservative plane/AABB rejection. Contact and floating-point uncertainty stay visible.
[[nodiscard]] bool visible(const RenderBounds& bounds, const RenderCameraMatrices& camera) noexcept;
} // namespace shark::render_detail
