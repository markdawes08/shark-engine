#pragma once

#include "scene_panel.hpp"
#include "viewport_navigation.hpp"

#include <shark/tools/transform_manipulation.hpp>

#include <array>
#include <optional>
#include <span>

namespace shark::editor {

struct GizmoPoint {
    double x{0};
    double y{0};
};
struct GizmoAxis {
    bool visible{false};
    std::array<GizmoPoint, 65> points{};
    std::size_t point_count{0};
};

class ViewportGizmoGeometry final {
  public:
    [[nodiscard]] const GizmoPoint& pivot() const noexcept {
        return pivot_;
    }
    [[nodiscard]] std::span<const GizmoAxis, 3> axes() const noexcept {
        return axes_;
    }
    [[nodiscard]] const ViewportRectangle& rectangle() const noexcept {
        return rectangle_;
    }
    [[nodiscard]] SceneRevision revision() const noexcept {
        return revision_;
    }
    [[nodiscard]] tools::TransformOperation operation() const noexcept {
        return operation_;
    }
    [[nodiscard]] tools::TransformSpace space() const noexcept {
        return space_;
    }
    [[nodiscard]] const math::Mat4& view_projection() const noexcept {
        return view_projection_;
    }

  private:
    friend Result<ViewportGizmoGeometry>
    make_viewport_gizmo(const RenderScene&, const TransformObservation&, tools::TransformOperation,
                        tools::TransformSpace, const ViewportRectangle&);
    friend class ViewportGizmoGesture;
    ViewportRectangle rectangle_{};
    SceneRevision revision_{};
    GizmoPoint pivot_{};
    std::array<GizmoAxis, 3> axes_{};
    tools::TransformOperation operation_{tools::TransformOperation::translate};
    tools::TransformSpace space_{tools::TransformSpace::local};
    math::Mat4 view_projection_{1.0};
    math::Mat4 inverse_view_projection_{1.0};
    math::Mat4 frame_{1.0};
    math::Mat4 inverse_frame_{1.0};
    double translation_scale_{1.0};
    double radius_{1.0};
};

// Bounded editor-only overlay geometry in logical image pixels. Local axes use
// the full affine parent and start rotation, without the entity's own scale.
// Rotation rings may be ellipses under affine parents; edge-on handles are hidden.
[[nodiscard]] Result<ViewportGizmoGeometry>
make_viewport_gizmo(const RenderScene&, const TransformObservation&, tools::TransformOperation,
                    tools::TransformSpace, const ViewportRectangle&);
[[nodiscard]] Result<std::optional<std::size_t>> hit_viewport_gizmo(const ViewportGizmoGeometry&,
                                                                    double x, double y);

// Pure pointer math. Caller owns focus/button/capture/revision cancellation and
// the real TransformDrag. Each update returns cumulative unsnapped input for its
// shared helper, including unwrapped rotations across the +/-pi boundary.
class ViewportGizmoGesture final {
  public:
    [[nodiscard]] Result<void> begin(const ViewportGizmoGeometry&, std::size_t axis, double x,
                                     double y, tools::TransformSnap snap = {});
    [[nodiscard]] Result<tools::TransformManipulation> update(double x, double y);
    void cancel() noexcept;
    [[nodiscard]] bool active() const noexcept {
        return geometry_.has_value();
    }
    [[nodiscard]] const std::optional<ViewportGizmoGeometry>& geometry() const noexcept {
        return geometry_;
    }
    [[nodiscard]] std::size_t axis() const noexcept {
        return axis_;
    }

  private:
    std::optional<ViewportGizmoGeometry> geometry_;
    tools::TransformSnap snap_;
    std::size_t axis_{0};
    double start_parameter_{0};
    double previous_angle_{0};
    double accumulated_angle_{0};
};

} // namespace shark::editor
