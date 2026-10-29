#pragma once

#include <shark/scene_render.hpp>

namespace shark::editor {

// Logical screen coordinates of the actual displayed image, not its enclosing
// panel or the native framebuffer. Right and bottom edges are exclusive.
struct ViewportRectangle {
    double x{0};
    double y{0};
    double width{0};
    double height{0};
};
struct ViewportCoordinates {
    double u;
    double v;
};
[[nodiscard]] Result<std::optional<ViewportCoordinates>>
viewport_coordinates(const ViewportRectangle&, double x, double y);

// Drag deltas are logical pixels; dolly is fractional wheel steps. No time,
// native input, owner mutation or persistent Camera component is involved.
struct NavigationMotion {
    double orbit_x{0};
    double orbit_y{0};
    double pan_x{0};
    double pan_y{0};
    double dolly{0};
};
class ViewportNavigation final {
  public:
    [[nodiscard]] Result<void> reset(const RenderScene&, const Camera&);
    void clear() noexcept;
    [[nodiscard]] const std::optional<RenderViewCamera>& view() const noexcept;
    // A complete valid candidate replaces the display view. Failure leaves the
    // prior pose/projection and orbit pivot unchanged. Full affine seed bases
    // (including reflection/shear) survive rigid navigation transforms.
    [[nodiscard]] Result<bool> navigate(const NavigationMotion&, RenderViewport,
                                        const ViewportRectangle&);

  private:
    std::optional<RenderViewCamera> view_;
    RenderOrigin origin_;
    math::Vec3 pivot_{0.0};
    double distance_{10.0};
};

inline constexpr std::size_t viewport_pick_maximum_triangles = 1'048'576;
struct ViewportHit {
    AuthoredEntityId entity;
    SceneRevision revision;
    // Distance along the normalized origin-relative ray from its near plane.
    double distance;
};
// Tests the exact owned snapshot used by the image. Whole-scene triangle work is
// charged before traversal; exhaustion never returns a partial nearest hit.
// One-sided winding follows local triangles (renderer reflection correction is
// equivalent); equal-depth hits choose the later record/primitive/triangle, as
// the viewport's LESS_OR_EQUAL depth operation does. No entity is re-resolved.
[[nodiscard]] Result<std::optional<ViewportHit>> pick_viewport(const RenderScene&,
                                                               SceneRevision expected_revision,
                                                               const ViewportRectangle&, double x,
                                                               double y);

} // namespace shark::editor
