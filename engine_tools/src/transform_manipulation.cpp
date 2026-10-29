#include <shark/tools/transform_manipulation.hpp>

#include <algorithm>
#include <cmath>

namespace shark::tools {
namespace {

Error failure(const char* message) {
    return Error{ErrorCode::invalid_argument, message,
                 ErrorContext{"authoring.transform-manipulation"}};
}

bool finite(const math::Vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool valid_step(const std::optional<double>& step) noexcept {
    return !step || (std::isfinite(*step) && *step > 0.0);
}

Result<double> snapped(double value, const std::optional<double>& step, double origin = 0.0) {
    if (!step)
        return value;
    const auto units = (value - origin) / *step;
    if (!std::isfinite(units))
        return failure("Transform snap quotient is not representable");
    const auto result = origin + std::round(units) * *step;
    if (!std::isfinite(result))
        return failure("Snapped transform value is not representable");
    return result;
}

Result<math::Vec3> snapped(math::Vec3 value, const std::optional<double>& step,
                           double origin = 0.0) {
    for (int axis = 0; axis < 3; ++axis) {
        auto result = snapped(value[axis], step, origin);
        if (!result)
            return result.error();
        value[axis] = result.value();
    }
    return value;
}

Result<Transform> validated(Transform value, const math::Mat4& parent_world) {
    auto canonical = canonicalize_transform(value);
    if (!canonical)
        return canonical.error();
    auto local = transform_matrix(canonical.value());
    if (!local)
        return local.error();
    auto world = validate_transform_matrix(parent_world * local.value());
    if (!world)
        return world.error();
    return canonical.value();
}

} // namespace

Result<Transform> calculate_manipulated_transform(const Transform& start,
                                                  const math::Mat4& parent_world,
                                                  const TransformManipulation& manipulation) {
    if ((manipulation.space != TransformSpace::local &&
         manipulation.space != TransformSpace::world) ||
        (manipulation.operation != TransformOperation::translate &&
         manipulation.operation != TransformOperation::rotate &&
         manipulation.operation != TransformOperation::scale))
        return failure("Unknown transform operation or coordinate space");
    if (!finite(manipulation.translation) || !finite(manipulation.rotation_axis) ||
        !std::isfinite(manipulation.rotation_radians) || !finite(manipulation.scale_factors))
        return failure("Transform manipulation values must be finite");
    if (!valid_step(manipulation.snap.translation) ||
        !valid_step(manipulation.snap.rotation_radians) || !valid_step(manipulation.snap.scale))
        return failure("Transform snap steps must be finite and positive");
    auto parent_valid = validate_transform_matrix(parent_world);
    if (!parent_valid)
        return parent_valid.error();
    auto initial = validated(start, parent_world);
    if (!initial)
        return initial.error();
    auto result = initial.value();
    math::Vec3 delta{0.0};
    math::Vec3 factors{1.0};
    math::Quat rotation{1.0, 0.0, 0.0, 0.0};
    if (manipulation.operation == TransformOperation::translate) {
        auto value = snapped(manipulation.translation, manipulation.snap.translation);
        if (!value)
            return value.error();
        delta = value.value();
        if (delta == math::Vec3{0.0})
            return result;
    } else if (manipulation.operation == TransformOperation::rotate) {
        const auto maximum = std::max({std::abs(manipulation.rotation_axis.x),
                                       std::abs(manipulation.rotation_axis.y),
                                       std::abs(manipulation.rotation_axis.z)});
        if (maximum == 0.0)
            return failure("Transform rotation axis must be nonzero");
        const auto scaled_axis = manipulation.rotation_axis / maximum;
        const auto axis = scaled_axis / std::hypot(scaled_axis.x, scaled_axis.y, scaled_axis.z);
        auto angle = snapped(manipulation.rotation_radians, manipulation.snap.rotation_radians);
        if (!angle)
            return angle.error();
        if (angle.value() == 0.0)
            return result;
        rotation = glm::angleAxis(angle.value(), axis);
    } else {
        auto value = snapped(manipulation.scale_factors, manipulation.snap.scale, 1.0);
        if (!value)
            return value.error();
        factors = value.value();
        if (factors == math::Vec3{1.0})
            return result;
    }

    if (manipulation.space == TransformSpace::local) {
        if (manipulation.operation == TransformOperation::translate)
            result.translation += result.rotation * delta;
        else if (manipulation.operation == TransformOperation::rotate)
            result.rotation = result.rotation * rotation;
        else
            result.scale *= factors;
        return validated(result, parent_world);
    }

    auto inverse = inverse_transform_matrix(parent_world);
    if (!inverse)
        return inverse.error();
    auto local = transform_matrix(result);
    if (!local)
        return local.error();
    auto desired_world = parent_world * local.value();
    if (manipulation.operation == TransformOperation::translate) {
        // Preserve the original TRS representation instead of decomposing an
        // unchanged linear basis (which could move reflection signs).
        result.translation += math::Vec3{inverse.value() * glm::dvec4{delta, 0.0}};
        desired_world[3] += glm::dvec4{delta, 0.0};
    } else {
        const auto linear = manipulation.operation == TransformOperation::rotate
                                ? glm::mat4_cast(rotation)
                                : glm::scale(math::Mat4{1.0}, factors);
        for (int column = 0; column < 3; ++column)
            desired_world[column] = linear * desired_world[column];
        auto decomposed = decompose_transform(inverse.value() * desired_world);
        if (!decomposed)
            return decomposed.error();
        result = decomposed.value();
        // Rotation/scaling occur around the entity origin, never the parent or
        // global origin. Keep the captured local pivot's exact stored values.
        result.translation = initial.value().translation;
    }
    auto candidate = validated(result, parent_world);
    if (!candidate)
        return candidate.error();
    auto candidate_matrix = transform_matrix(candidate.value());
    if (!candidate_matrix)
        return candidate_matrix.error();
    if (!transform_matrices_equivalent(parent_world * candidate_matrix.value(), desired_world))
        return failure("World manipulation cannot preserve the requested transform reliably");
    return candidate.value();
}

} // namespace shark::tools
