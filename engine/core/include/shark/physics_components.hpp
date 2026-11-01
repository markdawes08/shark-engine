#pragma once

#include "shark/transform.hpp"

#include <cstdint>

namespace shark {

enum class BodyMotion : std::uint32_t { static_body = 0, dynamic_body = 1, kinematic_body = 2 };
enum class ColliderShape : std::uint32_t { box = 0, sphere = 1, capsule = 2 };

inline constexpr double physics_minimum_dimension = 0.01;
inline constexpr double physics_maximum_dimension = 256.0;
inline constexpr double physics_local_position_limit = 8192.0;
inline constexpr double physics_maximum_mass = 1'000'000.0;

// One atomic authored component: collider dimensions are unscaled meters, a
// capsule is aligned with local Y, and half_height excludes its two end caps.
// All fields, including inactive shape fields, remain validated and persisted.
struct RigidBody {
    BodyMotion motion{BodyMotion::static_body};
    ColliderShape shape{ColliderShape::box};
    math::Vec3 half_extents{0.5, 0.5, 0.5};
    double radius{0.5};
    double half_height{0.5};
    double mass{1.0};
    double friction{0.5};
    double restitution{0.0};
    std::uint32_t collision_layer{0};
    std::uint32_t collision_mask{0xffff};
    bool is_trigger{false};
    bool operator==(const RigidBody&) const = default;
};

[[nodiscard]] Result<RigidBody> validate_rigid_body(RigidBody body);
// Scene separately enforces a root entity and non-replica ownership. This pure
// check validates canonical local pose and positive uniform baked shape scale.
[[nodiscard]] Result<Transform> validate_rigid_body_pose(const RigidBody& body,
                                                         Transform transform);

} // namespace shark
