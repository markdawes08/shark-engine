#pragma once

#include <shark/transform.hpp>

#include <optional>

namespace shark::tools {

enum class TransformSpace { local, world };
enum class TransformOperation { translate, rotate, scale };

struct TransformSnap {
    // Enabled steps must be finite and positive. Translation is in meters,
    // rotation in radians and scale in dimensionless factor increments.
    std::optional<double> translation;
    std::optional<double> rotation_radians;
    std::optional<double> scale;
};

struct TransformManipulation {
    TransformOperation operation{TransformOperation::translate};
    TransformSpace space{TransformSpace::local};
    math::Vec3 translation{0.0};
    math::Vec3 rotation_axis{0.0, 1.0, 0.0};
    double rotation_radians{0.0};
    math::Vec3 scale_factors{1.0};
    TransformSnap snap{};
};

// Pure, cumulative manipulation relative to the captured start, never the last
// preview. Local translation uses start's rotation axes without its scale;
// local rotation postmultiplies start's rotation. Local scaling multiplies the
// original scale components. World operations use scene-local world axes and
// preserve the entity's world pivot. These are not global MMO coordinates.
// Snapping rounds ties away from zero; scale factors snap about identity (1).
// World operations reject unrepresentable local shear or unreliable inversion.
// This validates one entity, not descendants/history/revisions. Submit the value
// through the existing scene preview and retained SetTransformCommand owner.
[[nodiscard]] Result<Transform>
calculate_manipulated_transform(const Transform& start, const math::Mat4& parent_world,
                                const TransformManipulation& manipulation);

} // namespace shark::tools
