#pragma once

#include "shark/error.hpp"
#include "shark/math.hpp"

namespace shark {

inline constexpr double transform_minimum_scale = 1e-6;
inline constexpr double transform_absolute_tolerance = 1e-9;
inline constexpr double transform_relative_tolerance = 1e-9;
inline constexpr double transform_orthogonality_tolerance = 1e-9;
inline constexpr double transform_minimum_basis_determinant = 1e-12;

// Local meters, radians and TRS; this is not a global WorldPosition or a render
// origin. Quat's constructor takes w,x,y,z; future source formats use x,y,z,w.
struct Transform {
    math::Vec3 translation{0.0};
    math::Quat rotation{1.0, 0.0, 0.0, 0.0};
    math::Vec3 scale{1.0};
    bool operator==(const Transform&) const = default;
};

[[nodiscard]] Result<Transform> canonicalize_transform(Transform transform);
[[nodiscard]] Result<math::Mat4> transform_matrix(Transform transform);

// Affine row must be exactly [0,0,0,1]. World shear is allowed; finite nonzero
// basis columns must have |det(normalized columns)| above the declared floor.
[[nodiscard]] Result<void> validate_transform_matrix(const math::Mat4& matrix);

// Rejects non-TRS shear. An odd reflection is represented by negative X scale;
// an even number of reflected axes is represented by the proper rotation.
[[nodiscard]] Result<Transform> decompose_transform(const math::Mat4& matrix);

// Supports affine world shear, rejecting nonfinite or numerically unreliable
// inverses. Both multiplication orders must reproduce identity within tolerance.
[[nodiscard]] Result<math::Mat4> inverse_transform_matrix(const math::Mat4& matrix);

// Finite, per-element absolute + relative tolerance. Large translation never
// contributes allowance to an unrelated linear or translation element.
[[nodiscard]] bool transform_matrices_equivalent(const math::Mat4& left,
                                                 const math::Mat4& right) noexcept;

} // namespace shark
