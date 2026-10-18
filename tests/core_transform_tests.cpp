#include <shark/transform.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <random>
#include <type_traits>

namespace {

using shark::math::Mat4;
using shark::math::Quat;
using shark::math::Vec3;

bool close(const double left, const double right, const double tolerance = 1e-9) {
    return std::abs(left - right) <= tolerance;
}

Mat4 matrix(const shark::Transform& transform) {
    auto result = shark::transform_matrix(transform);
    REQUIRE(result);
    return result.value();
}

void check_vector(const glm::dvec4& actual, const glm::dvec4& expected) {
    for (int axis = 0; axis < 4; ++axis) {
        CHECK(close(actual[axis], expected[axis]));
    }
}

void check_rotation(const Quat& actual, const Quat& expected) {
    CHECK(close(actual.w, expected.w));
    CHECK(close(actual.x, expected.x));
    CHECK(close(actual.y, expected.y));
    CHECK(close(actual.z, expected.z));
}

Quat around_z(const double angle) {
    return Quat{std::cos(angle / 2.0), 0.0, 0.0, std::sin(angle / 2.0)};
}

static_assert(std::is_same_v<decltype(shark::Transform{} == shark::Transform{}), bool>);
static_assert(sizeof(Vec3) == 24 && alignof(Vec3) == alignof(double));
static_assert(sizeof(Quat) == 32 && alignof(Quat) == alignof(double));
static_assert(sizeof(Mat4) == 128 && alignof(Mat4) == alignof(double));

} // namespace

TEST_CASE("central math uses packed doubles right handed axes radians and zero to one depth",
          "[transform]") {
    CHECK(glm::cross(Vec3{1, 0, 0}, Vec3{0, 1, 0}) == Vec3{0, 0, 1});
    const auto projection = glm::perspective(std::numbers::pi / 2.0, 1.0, 1.0, 10.0);
    const auto near = projection * glm::dvec4{0, 0, -1, 1};
    const auto far = projection * glm::dvec4{0, 0, -10, 1};
    CHECK(close(near.z / near.w, 0.0));
    CHECK(close(far.z / far.w, 1.0));
    CHECK(near.w > 0.0);
    const auto corner = projection * glm::dvec4{1, 1, -1, 1};
    CHECK(close(corner.x / corner.w, 1.0));
    CHECK(close(corner.y / corner.w, 1.0));

    const Quat constructor{4, 1, 2, 3};
    CHECK(constructor.w == 4);
    CHECK(constructor.x == 1);
    CHECK(constructor.y == 2);
    CHECK(constructor.z == 3);
    CHECK(constructor[0] == 1);
    CHECK(constructor[3] == 4);
}

TEST_CASE("local transform defaults and independent analytic TRS composition", "[transform]") {
    const shark::Transform identity;
    CHECK(identity.translation == Vec3{0.0});
    CHECK(identity.scale == Vec3{1.0});
    CHECK(identity.rotation == Quat{1, 0, 0, 0});
    CHECK(matrix(identity) == Mat4{1.0});

    const shark::Transform transform{{5, -2, 3}, around_z(std::numbers::pi / 2.0), {2, -3, 4}};
    Mat4 expected{1.0};
    expected[0] = glm::dvec4{0, 2, 0, 0};
    expected[1] = glm::dvec4{3, 0, 0, 0};
    expected[2] = glm::dvec4{0, 0, 4, 0};
    expected[3] = glm::dvec4{5, -2, 3, 1};
    const auto actual = matrix(transform);
    CHECK(shark::transform_matrices_equivalent(actual, expected));
    check_vector(actual * glm::dvec4{1, 2, -1, 1}, glm::dvec4{11, 0, -1, 1});

    const shark::Transform parent{{10, 0, 0}, around_z(std::numbers::pi / 2.0), {2, 2, 2}};
    const shark::Transform child{{3, 0, 0}, Quat{1, 0, 0, 0}, {1, 1, 1}};
    check_vector(matrix(parent) * matrix(child) * glm::dvec4{0, 0, 0, 1}, glm::dvec4{10, 6, 0, 1});
}

TEST_CASE("quaternion normalization handles enormous and subnormal inputs without overflow",
          "[transform]") {
    for (const auto magnitude : std::array{std::numeric_limits<double>::max(), 1e200, 1e-200,
                                           std::numeric_limits<double>::denorm_min()}) {
        shark::Transform input;
        input.rotation = Quat{magnitude, magnitude, -magnitude, magnitude};
        const auto canonical = shark::canonicalize_transform(input);
        REQUIRE(canonical);
        check_rotation(canonical.value().rotation, Quat{0.5, 0.5, -0.5, 0.5});
        CHECK(shark::transform_matrix(input));
        const auto repeated = shark::canonicalize_transform(canonical.value());
        REQUIRE(repeated);
        CHECK(repeated.value() == canonical.value());
    }
    shark::Transform mixed;
    mixed.rotation =
        Quat{std::numeric_limits<double>::max(), std::numeric_limits<double>::denorm_min(), 0, 0};
    const auto canonical = shark::canonicalize_transform(mixed);
    REQUIRE(canonical);
    CHECK(canonical.value().rotation == Quat{1, 0, 0, 0});
}

TEST_CASE("canonical quaternion sign and positive zero are deterministic", "[transform]") {
    const std::array inputs{Quat{-0.5, 0.5, -0.5, 0.5}, Quat{-0.0, -1.0, 0.0, -0.0},
                            Quat{0.0, -0.0, -1.0, 0.0}, Quat{-0.0, 0.0, -0.0, -1.0}};
    const std::array expected{Quat{0.5, -0.5, 0.5, -0.5}, Quat{0.0, 1.0, 0.0, 0.0},
                              Quat{0.0, 0.0, 1.0, 0.0}, Quat{0.0, 0.0, 0.0, 1.0}};
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const shark::Transform input{{-0.0, 0.0, -0.0}, inputs[index], {1, 1, 1}};
        const auto canonical = shark::canonicalize_transform(input);
        REQUIRE(canonical);
        CHECK(canonical.value().rotation == expected[index]);
        for (int axis = 0; axis < 3; ++axis) {
            CHECK_FALSE(std::signbit(canonical.value().translation[axis]));
        }
        for (int axis = 0; axis < 4; ++axis) {
            if (canonical.value().rotation[axis] == 0.0) {
                CHECK_FALSE(std::signbit(canonical.value().rotation[axis]));
            }
        }
        auto opposite = input;
        opposite.rotation = -opposite.rotation;
        const auto other_sign = shark::canonicalize_transform(opposite);
        REQUIRE(other_sign);
        CHECK(other_sign.value() == canonical.value());
    }
}

TEST_CASE("transform validation rejects nonfinite degenerate and below-minimum fields",
          "[transform]") {
    for (const auto invalid : std::array{std::numeric_limits<double>::infinity(),
                                         -std::numeric_limits<double>::infinity(),
                                         std::numeric_limits<double>::quiet_NaN()}) {
        for (int axis = 0; axis < 3; ++axis) {
            shark::Transform translation;
            translation.translation[axis] = invalid;
            CHECK_FALSE(shark::canonicalize_transform(translation));
            CHECK_FALSE(shark::transform_matrix(translation));
            shark::Transform scale;
            scale.scale[axis] = invalid;
            CHECK_FALSE(shark::canonicalize_transform(scale));
        }
        for (int axis = 0; axis < 4; ++axis) {
            shark::Transform rotation;
            rotation.rotation[axis] = invalid;
            CHECK_FALSE(shark::canonicalize_transform(rotation));
        }
    }
    shark::Transform zero;
    zero.rotation = Quat{0, 0, 0, 0};
    const auto zero_result = shark::canonicalize_transform(zero);
    REQUIRE_FALSE(zero_result);
    CHECK(zero_result.error().code == shark::ErrorCode::invalid_argument);
    CHECK(zero_result.error().context.operation == "transform.canonicalize");
    for (const auto invalid :
         std::array{0.0, -0.0, 1e-7, -1e-7, std::nextafter(shark::transform_minimum_scale, 0.0)}) {
        shark::Transform input;
        input.scale.y = invalid;
        const auto result = shark::transform_matrix(input);
        REQUIRE_FALSE(result);
        CHECK(result.error().context.operation == "transform.matrix");
    }
    const shark::Transform limits{{std::numeric_limits<double>::max(), 0, 0},
                                  Quat{1, 0, 0, 0},
                                  {1e-6, -1e-6, std::numeric_limits<double>::max()}};
    CHECK(shark::canonicalize_transform(limits));
    CHECK(shark::transform_matrix(limits));
}

TEST_CASE("TRS decomposition preserves mirrors with a fixed negative X convention", "[transform]") {
    const std::array scales{Vec3{-2, 3, 4}, Vec3{2, -3, 4}, Vec3{2, 3, -4}, Vec3{-2, -3, 4},
                            Vec3{-2, -3, -4}};
    for (const auto scale : scales) {
        const shark::Transform input{{8, -7, 2}, Quat{0.5, 0.5, 0.5, 0.5}, scale};
        const auto original = matrix(input);
        const auto decomposed = shark::decompose_transform(original);
        REQUIRE(decomposed);
        CHECK(shark::transform_matrices_equivalent(matrix(decomposed.value()), original));
        CHECK(decomposed.value().translation == input.translation);
        CHECK(decomposed.value().scale.y > 0.0);
        CHECK(decomposed.value().scale.z > 0.0);
        CHECK((decomposed.value().scale.x < 0.0) == (scale.x * scale.y * scale.z < 0.0));
        CHECK(decomposed.value().rotation.w >= 0.0);
        const auto repeated = shark::decompose_transform(original);
        REQUIRE(repeated);
        CHECK(repeated.value() == decomposed.value());
    }
    Mat4 minimum{1.0};
    minimum[0][0] = shark::transform_minimum_scale;
    CHECK(shark::decompose_transform(minimum));
    minimum[0][0] = std::nextafter(shark::transform_minimum_scale, 0.0);
    CHECK_FALSE(shark::decompose_transform(minimum));
}

TEST_CASE("world matrices allow shear while local decomposition rejects it", "[transform]") {
    const shark::Transform parent{{0, 0, 0}, Quat{1, 0, 0, 0}, {2, 1, 1}};
    const shark::Transform child{{0, 0, 0}, around_z(std::numbers::pi / 4.0), {1, 1, 1}};
    const auto sheared = matrix(parent) * matrix(child);
    REQUIRE(shark::validate_transform_matrix(sheared));
    const auto decomposed = shark::decompose_transform(sheared);
    REQUIRE_FALSE(decomposed);
    CHECK(decomposed.error().context.operation == "transform.decompose");
    CHECK(shark::inverse_transform_matrix(sheared));

    Mat4 translation_cannot_hide_shear{1.0};
    translation_cannot_hide_shear[1][0] = 0.001;
    translation_cannot_hide_shear[3][0] = 1e250;
    CHECK_FALSE(shark::decompose_transform(translation_cannot_hide_shear));
}

TEST_CASE("matrix validation rejects perspective nonfinite and collapsed bases", "[transform]") {
    for (int column = 0; column < 4; ++column) {
        Mat4 perspective{1.0};
        perspective[column][3] = column == 3 ? std::nextafter(1.0, 0.0) : 1e-300;
        CHECK_FALSE(shark::validate_transform_matrix(perspective));
        CHECK_FALSE(shark::decompose_transform(perspective));
        CHECK_FALSE(shark::inverse_transform_matrix(perspective));
    }
    for (const auto invalid : std::array{std::numeric_limits<double>::infinity(),
                                         std::numeric_limits<double>::quiet_NaN()}) {
        Mat4 nonfinite{1.0};
        nonfinite[3][1] = invalid;
        const auto result = shark::validate_transform_matrix(nonfinite);
        REQUIRE_FALSE(result);
        CHECK(result.error().context.operation == "transform.validate-matrix");
    }
    Mat4 zero_column{1.0};
    zero_column[0] = glm::dvec4{0.0};
    CHECK_FALSE(shark::validate_transform_matrix(zero_column));
    Mat4 near_parallel{1.0};
    near_parallel[1] = glm::dvec4{1.0, 1e-13, 0.0, 0.0};
    CHECK_FALSE(shark::validate_transform_matrix(near_parallel));
    CHECK_FALSE(shark::inverse_transform_matrix(near_parallel));
    near_parallel[1][1] = 2e-12;
    CHECK(shark::validate_transform_matrix(near_parallel));

    Mat4 overflow{1.0};
    overflow[0][0] = 1e200;
    CHECK_FALSE(shark::validate_transform_matrix(overflow * overflow));
    Mat4 underflow{1.0};
    underflow[0][0] = 1e-200;
    CHECK_FALSE(shark::validate_transform_matrix(underflow * underflow));
}

TEST_CASE("affine inverse has independent analytic shear and translation results", "[transform]") {
    Mat4 input{1.0};
    input[0][0] = 2.0;
    input[1][0] = 1.0;
    input[1][1] = 3.0;
    input[2][2] = 4.0;
    input[3] = glm::dvec4{4, 5, 6, 1};
    Mat4 expected{1.0};
    expected[0][0] = 0.5;
    expected[1][0] = -1.0 / 6.0;
    expected[1][1] = 1.0 / 3.0;
    expected[2][2] = 0.25;
    expected[3] = glm::dvec4{-7.0 / 6.0, -5.0 / 3.0, -1.5, 1.0};
    const auto inverse = shark::inverse_transform_matrix(input);
    REQUIRE(inverse);
    CHECK(shark::transform_matrices_equivalent(inverse.value(), expected));
    check_vector(inverse.value() * glm::dvec4{8, 11, 18, 1}, glm::dvec4{1, 2, 3, 1});
    CHECK(shark::transform_matrices_equivalent(input * inverse.value(), Mat4{1.0}));
    CHECK(shark::transform_matrices_equivalent(inverse.value() * input, Mat4{1.0}));
}

TEST_CASE("inverse scales columns before arithmetic and rejects unrepresentable results",
          "[transform]") {
    Mat4 extreme{1.0};
    extreme[0][0] = 1e200;
    extreme[1][1] = 1e-200;
    const auto inverse = shark::inverse_transform_matrix(extreme);
    REQUIRE(inverse);
    CHECK(inverse.value()[0][0] == 1e-200);
    CHECK(inverse.value()[1][1] == 1e200);
    CHECK_FALSE(shark::decompose_transform(extreme));

    const auto maximum = std::numeric_limits<double>::max();
    Mat4 enormous_columns{1.0};
    enormous_columns[0] = glm::dvec4{maximum, maximum, 0, 0};
    enormous_columns[1] = glm::dvec4{-maximum, maximum, 0, 0};
    CHECK(shark::validate_transform_matrix(enormous_columns));
    CHECK_FALSE(shark::decompose_transform(enormous_columns));
    CHECK(shark::inverse_transform_matrix(enormous_columns));

    Mat4 inverse_overflow{1.0};
    inverse_overflow[0][0] = std::numeric_limits<double>::denorm_min();
    REQUIRE(shark::validate_transform_matrix(inverse_overflow));
    const auto failed = shark::inverse_transform_matrix(inverse_overflow);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == shark::ErrorCode::invalid_argument);
    CHECK(failed.error().context.operation == "transform.inverse");
}

TEST_CASE("matrix comparison never lets large translation mask unrelated errors", "[transform]") {
    Mat4 left{1.0};
    left[3] = glm::dvec4{1e250, 1.0, 0.0, 1.0};
    auto right = left;
    right[0][0] += 0.001;
    CHECK_FALSE(shark::transform_matrices_equivalent(left, right));
    right = left;
    right[3][1] += 0.001;
    CHECK_FALSE(shark::transform_matrices_equivalent(left, right));
    right = left;
    right[0][1] = 5e-10;
    CHECK(shark::transform_matrices_equivalent(left, right));
    right[0][1] = 2e-9;
    CHECK_FALSE(shark::transform_matrices_equivalent(left, right));
    right = left;
    right[3][0] = -1e250;
    CHECK_FALSE(shark::transform_matrices_equivalent(left, right));
    left[0][0] = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(shark::transform_matrices_equivalent(left, left));
}

TEST_CASE("fixed seed canonical transforms round trip through matrices and inverses",
          "[transform]") {
    std::mt19937 random{1337};
    for (int sample = 0; sample < 512; ++sample) {
        const auto signed_value = [&random]() {
            return static_cast<double>(static_cast<std::int64_t>(random()) % 20001 - 10000);
        };
        shark::Transform input;
        input.translation = Vec3{signed_value(), signed_value(), signed_value()};
        input.rotation = Quat{signed_value(), signed_value(), signed_value(), signed_value()};
        for (int axis = 0; axis < 3; ++axis) {
            const auto value = signed_value();
            input.scale[axis] = (value < 0.0 ? -1.0 : 1.0) * (0.25 + std::abs(value) / 1000.0);
        }
        const auto canonical = shark::canonicalize_transform(input);
        REQUIRE(canonical);
        const auto repeated = shark::canonicalize_transform(canonical.value());
        REQUIRE(repeated);
        CHECK(repeated.value() == canonical.value());
        const auto original = matrix(canonical.value());
        const auto decomposed = shark::decompose_transform(original);
        REQUIRE(decomposed);
        CHECK(shark::transform_matrices_equivalent(matrix(decomposed.value()), original));
        const auto inverse = shark::inverse_transform_matrix(original);
        REQUIRE(inverse);
        CHECK(shark::transform_matrices_equivalent(original * inverse.value(), Mat4{1.0}));
        CHECK(shark::transform_matrices_equivalent(inverse.value() * original, Mat4{1.0}));
    }
}
