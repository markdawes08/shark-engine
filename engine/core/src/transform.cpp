#include <shark/transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace shark {
namespace {

Error transform_error(const char* operation, const char* message) {
    return Error{ErrorCode::invalid_argument, message, ErrorContext{operation}};
}

double positive_zero(const double value) noexcept {
    return value == 0.0 ? 0.0 : value;
}

bool finite(const math::Vec3& vector) noexcept {
    return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
}

bool finite(const math::Mat4& matrix) noexcept {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row])) {
                return false;
            }
        }
    }
    return true;
}

bool equivalent(const double left, const double right) noexcept {
    return std::isfinite(left) && std::isfinite(right) &&
           std::abs(left - right) <=
               transform_absolute_tolerance +
                   transform_relative_tolerance * std::max(std::abs(left), std::abs(right));
}

Result<Transform> canonicalize(Transform transform, const char* operation) {
    if (!finite(transform.translation) || !finite(transform.scale)) {
        return transform_error(operation, "Transform translation and scale must be finite");
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(transform.scale[axis]) < transform_minimum_scale) {
            return transform_error(operation, "Transform scale magnitude must be at least 1e-6");
        }
        transform.translation[axis] = positive_zero(transform.translation[axis]);
    }
    auto& rotation = transform.rotation;
    if (!std::isfinite(rotation.w) || !std::isfinite(rotation.x) || !std::isfinite(rotation.y) ||
        !std::isfinite(rotation.z)) {
        return transform_error(operation, "Transform quaternion must be finite and nonzero");
    }
    const auto maximum = std::max(
        {std::abs(rotation.w), std::abs(rotation.x), std::abs(rotation.y), std::abs(rotation.z)});
    if (maximum == 0.0) {
        return transform_error(operation, "Transform quaternion must be finite and nonzero");
    }
    const math::Quat scaled{rotation.w / maximum, rotation.x / maximum, rotation.y / maximum,
                            rotation.z / maximum};
    const auto length = std::sqrt(scaled.w * scaled.w + scaled.x * scaled.x + scaled.y * scaled.y +
                                  scaled.z * scaled.z);
    // A prior normalized result is already unit within floating-point precision.
    // Preserve it to prevent repeated canonicalization from changing stored bits.
    constexpr auto unit_tolerance = 4.0 * std::numeric_limits<double>::epsilon();
    const bool already_unit =
        maximum >= 0.5 && maximum <= 1.0 && std::abs(maximum * length - 1.0) <= unit_tolerance;
    if (!already_unit) {
        rotation =
            math::Quat{scaled.w / length, scaled.x / length, scaled.y / length, scaled.z / length};
    }
    const bool negative =
        rotation.w < 0.0 ||
        (rotation.w == 0.0 &&
         (rotation.x < 0.0 ||
          (rotation.x == 0.0 && (rotation.y < 0.0 || (rotation.y == 0.0 && rotation.z < 0.0)))));
    if (negative) {
        rotation = -rotation;
    }
    rotation.w = positive_zero(rotation.w);
    rotation.x = positive_zero(rotation.x);
    rotation.y = positive_zero(rotation.y);
    rotation.z = positive_zero(rotation.z);
    return transform;
}

math::Mat4 compose(const Transform& transform) {
    auto matrix = glm::mat4_cast(transform.rotation);
    for (int column = 0; column < 3; ++column) {
        matrix[column] *= transform.scale[column];
    }
    matrix[3] = glm::dvec4{transform.translation, 1.0};
    return matrix;
}

struct LinearAnalysis {
    glm::dmat3 unit_basis{1.0};
    math::Vec3 column_maximum{0.0};
    math::Vec3 scaled_length{0.0};
    double determinant{0.0};
};

Result<LinearAnalysis> analyze(const math::Mat4& matrix, const char* operation) {
    if (!finite(matrix)) {
        return transform_error(operation, "Transform matrix must be finite");
    }
    if (matrix[0][3] != 0.0 || matrix[1][3] != 0.0 || matrix[2][3] != 0.0 || matrix[3][3] != 1.0) {
        return transform_error(operation, "Transform matrix must have the exact affine last row");
    }
    LinearAnalysis analysis;
    for (int column = 0; column < 3; ++column) {
        const math::Vec3 basis{matrix[column]};
        const auto maximum = std::max({std::abs(basis.x), std::abs(basis.y), std::abs(basis.z)});
        if (maximum == 0.0) {
            return transform_error(operation, "Transform matrix has a zero basis column");
        }
        const auto scaled = basis / maximum;
        const auto length = std::hypot(scaled.x, scaled.y, scaled.z);
        analysis.unit_basis[column] = scaled / length;
        analysis.column_maximum[column] = maximum;
        analysis.scaled_length[column] = length;
    }
    analysis.determinant = glm::determinant(analysis.unit_basis);
    if (!std::isfinite(analysis.determinant) ||
        std::abs(analysis.determinant) <= transform_minimum_basis_determinant) {
        return transform_error(operation, "Transform matrix basis is singular or ill-conditioned");
    }
    return analysis;
}

} // namespace

Result<Transform> canonicalize_transform(Transform transform) {
    return canonicalize(transform, "transform.canonicalize");
}

Result<math::Mat4> transform_matrix(Transform transform) {
    constexpr auto operation = "transform.matrix";
    auto canonical = canonicalize(transform, operation);
    if (!canonical) {
        return canonical.error();
    }
    auto matrix = compose(canonical.value());
    auto valid = analyze(matrix, operation);
    if (!valid) {
        return valid.error();
    }
    return matrix;
}

Result<void> validate_transform_matrix(const math::Mat4& matrix) {
    auto valid = analyze(matrix, "transform.validate-matrix");
    if (!valid) {
        return valid.error();
    }
    return {};
}

bool transform_matrices_equivalent(const math::Mat4& left, const math::Mat4& right) noexcept {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!equivalent(left[column][row], right[column][row])) {
                return false;
            }
        }
    }
    return true;
}

Result<Transform> decompose_transform(const math::Mat4& matrix) {
    constexpr auto operation = "transform.decompose";
    auto analyzed = analyze(matrix, operation);
    if (!analyzed) {
        return analyzed.error();
    }
    auto analysis = analyzed.value();
    for (int first = 0; first < 3; ++first) {
        for (int second = first + 1; second < 3; ++second) {
            if (std::abs(glm::dot(analysis.unit_basis[first], analysis.unit_basis[second])) >
                transform_orthogonality_tolerance) {
                return transform_error(operation,
                                       "Transform matrix contains nonrepresentable local shear");
            }
        }
    }
    Transform result;
    result.translation = math::Vec3{matrix[3]};
    for (int axis = 0; axis < 3; ++axis) {
        result.scale[axis] = analysis.column_maximum[axis] * analysis.scaled_length[axis];
    }
    if (analysis.determinant < 0.0) {
        analysis.unit_basis[0] = -analysis.unit_basis[0];
        result.scale.x = -result.scale.x;
    }
    result.rotation = glm::quat_cast(analysis.unit_basis);
    auto canonical = canonicalize(result, operation);
    if (!canonical) {
        return canonical.error();
    }
    if (!transform_matrices_equivalent(compose(canonical.value()), matrix)) {
        return transform_error(
            operation, "Transform matrix cannot be reconstructed as local TRS within tolerance");
    }
    return canonical;
}

Result<math::Mat4> inverse_transform_matrix(const math::Mat4& matrix) {
    constexpr auto operation = "transform.inverse";
    auto analyzed = analyze(matrix, operation);
    if (!analyzed) {
        return analyzed.error();
    }
    const auto& analysis = analyzed.value();
    const auto inverse_basis = glm::inverse(analysis.unit_basis);
    math::Mat4 inverse{1.0};
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            // L = U * diag(maximum * scaled_length). Divide separately to avoid
            // overflowing a column length or underflowing an intermediate reciprocal.
            inverse[column][row] = (inverse_basis[column][row] / analysis.scaled_length[row]) /
                                   analysis.column_maximum[row];
        }
    }
    inverse[3] = glm::dvec4{-(glm::dmat3{inverse} * math::Vec3{matrix[3]}), 1.0};
    if (!finite(inverse) || !transform_matrices_equivalent(matrix * inverse, math::Mat4{1.0}) ||
        !transform_matrices_equivalent(inverse * matrix, math::Mat4{1.0})) {
        return transform_error(
            operation, "Transform matrix inverse is not finite or reliable within tolerance");
    }
    return inverse;
}

} // namespace shark
