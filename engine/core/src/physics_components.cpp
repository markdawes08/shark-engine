#include "shark/physics_components.hpp"

#include <cmath>

namespace shark {
namespace {
Error invalid(const char* message) {
    return {ErrorCode::invalid_argument, message, {.operation = "physics.component.validate"}};
}
bool dimension(double value) {
    return std::isfinite(value) && value >= physics_minimum_dimension &&
           value <= physics_maximum_dimension;
}
} // namespace

Result<RigidBody> validate_rigid_body(RigidBody body) {
    if (body.collision_layer >= 16 || body.collision_mask > 0xffff)
        return invalid("Collision layer must be in [0,15] and mask in [0,65535]");
    if ((body.motion != BodyMotion::static_body && body.motion != BodyMotion::dynamic_body &&
         body.motion != BodyMotion::kinematic_body) ||
        (body.shape != ColliderShape::box && body.shape != ColliderShape::sphere &&
         body.shape != ColliderShape::capsule))
        return invalid("Unknown rigid-body motion or collider shape");
    if (!dimension(body.half_extents.x) || !dimension(body.half_extents.y) ||
        !dimension(body.half_extents.z) || !dimension(body.radius) || !dimension(body.half_height))
        return invalid("Collider dimensions must be finite meters in [0.01,256]");
    if (!std::isfinite(body.mass) || body.mass < 0.01 || body.mass > physics_maximum_mass ||
        !std::isfinite(body.friction) || body.friction < 0 || body.friction > 1 ||
        !std::isfinite(body.restitution) || body.restitution < 0 || body.restitution > 1)
        return invalid("Invalid rigid-body mass, friction or restitution");
    return body;
}

Result<Transform> validate_rigid_body_pose(const RigidBody& body, Transform transform) {
    if (auto checked = validate_rigid_body(body); !checked)
        return checked.error();
    auto canonical = canonicalize_transform(transform);
    if (!canonical)
        return canonical.error();
    transform = canonical.value();
    const auto scale = transform.scale.x;
    if (scale <= 0 || scale != transform.scale.y || scale != transform.scale.z)
        return invalid("Rigid bodies require positive uniform scale");
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(transform.translation[axis]) > physics_local_position_limit)
            return invalid("Rigid-body position exceeds the bounded local physics frame");
        if (body.shape == ColliderShape::box && !dimension(body.half_extents[axis] * scale))
            return invalid("Scaled box half extents exceed physics dimension bounds");
    }
    if (body.shape != ColliderShape::box && !dimension(body.radius * scale))
        return invalid("Scaled radius exceeds physics dimension bounds");
    if (body.shape == ColliderShape::capsule &&
        (!dimension(body.half_height * scale) ||
         (body.half_height + body.radius) * scale > physics_maximum_dimension))
        return invalid("Scaled capsule height exceeds physics dimension bounds");
    return transform;
}

} // namespace shark
