#pragma once

#include "viewport_navigation.hpp"

#include <shark/physics_components.hpp>

#include <optional>
#include <span>
#include <vector>

namespace shark::editor {

// The preview supports at most 128 bodies; query hits are capped by Core.
inline constexpr std::size_t physics_debug_maximum_bodies = 128;
inline constexpr std::size_t physics_debug_maximum_hits = 256;
inline constexpr std::size_t physics_debug_maximum_lines = 16384;

enum class PhysicsDebugKind { static_body, dynamic_body, kinematic_body, trigger, query, hit };
struct PhysicsDebugBody {
    // Exact committed native pose; dimensions in settings are unscaled.
    Transform pose;
    RigidBody settings;
};
enum class PhysicsDebugQueryKind { raycast, overlap_sphere, overlap_box };
struct PhysicsDebugQuery {
    PhysicsDebugQueryKind kind;
    // Ray endpoints, or center and half-extents for an axis-aligned overlap box.
    math::Vec3 first{};
    math::Vec3 second{};
    double radius{0};                      // Sphere only.
    std::vector<math::Vec3> hit_positions; // Optional owned ray hit positions.
};
struct PhysicsDebugLine {
    math::Vec3 first;
    math::Vec3 second;
    PhysicsDebugKind kind;
};
struct PhysicsDebugScreenLine {
    double x0;
    double y0;
    double x1;
    double y1;
    PhysicsDebugKind kind;
};

// Pure output-only wire geometry, in bounded scene-local coordinates. These
// observations do not resolve entities, authorize picking or change simulation.
[[nodiscard]] Result<std::vector<PhysicsDebugLine>>
make_physics_debug_lines(std::span<const PhysicsDebugBody> bodies,
                         const std::optional<PhysicsDebugQuery>& query = {});

// Clip against the Vulkan homogeneous frustum, then map to the actual logical
// image rectangle. RenderScene's view-projection uses origin-relative points.
[[nodiscard]] Result<std::vector<PhysicsDebugScreenLine>>
project_physics_debug_lines(std::span<const PhysicsDebugLine> lines,
                            const math::Mat4& view_projection, const math::Vec3& scene_local_offset,
                            const ViewportRectangle& rectangle);

} // namespace shark::editor
