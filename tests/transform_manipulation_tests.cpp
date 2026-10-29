#include <shark/tools/transform_manipulation.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace {
using namespace shark;
using namespace shark::tools;
constexpr double quarter_turn = std::numbers::pi / 2.0;

template <typename T> T take(Result<T> result) {
    if (!result)
        INFO(result.error().message);
    REQUIRE(result);
    return std::move(result).value();
}

void rejected(const Result<Transform>& result) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::invalid_argument);
    CHECK_FALSE(result.error().context.operation.empty());
}

math::Quat around_y(double angle) {
    return {std::cos(angle / 2.0), 0.0, std::sin(angle / 2.0), 0.0};
}

math::Quat around_z(double angle) {
    return {std::cos(angle / 2.0), 0.0, 0.0, std::sin(angle / 2.0)};
}

void vector_equal(const math::Vec3& actual, const math::Vec3& expected) {
    for (int axis = 0; axis < 3; ++axis)
        CHECK(actual[axis] == Catch::Approx(expected[axis]).margin(1e-10));
}

void matrix_equal(const math::Mat4& actual, const math::Mat4& expected) {
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            CHECK(actual[column][row] == Catch::Approx(expected[column][row]).margin(1e-10));
}

math::Mat4 world(const math::Mat4& parent, const Transform& local) {
    return parent * take(transform_matrix(local));
}
} // namespace

TEST_CASE("Transform manipulation translates along local rotation or world axes",
          "[transform-manipulation]") {
    const auto parent = take(transform_matrix({{10, -20, 5}, around_z(quarter_turn), {2, 3, 4}}));
    const Transform start{{1, 2, 3}, around_y(quarter_turn), {-2, 3, 4}};
    TransformManipulation move;
    move.translation = {2, 0, 0};
    const auto local = take(calculate_manipulated_transform(start, parent, move));
    vector_equal(local.translation, {1, 2, 1});
    CHECK(local.rotation == start.rotation);
    CHECK(local.scale == start.scale);
    vector_equal(math::Vec3{world(parent, local)[3]}, {4, -18, 9});

    move.space = TransformSpace::world;
    move.translation = {6, 0, 0};
    const auto global = take(calculate_manipulated_transform(start, parent, move));
    vector_equal(global.translation, {1, 0, 3});
    CHECK(global.rotation == start.rotation);
    CHECK(global.scale == start.scale);
    vector_equal(math::Vec3{world(parent, global)[3]}, {10, -18, 17});

    // Translation also works through a sheared ancestor. It does not demand a
    // TRS decomposition of that ancestor or change the child's signed scales.
    math::Mat4 shear{1.0};
    shear[1][0] = 2;
    move.translation = {0, 3, 0};
    const auto sheared = take(calculate_manipulated_transform(start, shear, move));
    vector_equal(sheared.translation, {-5, 5, 3});
    CHECK(sheared.rotation == start.rotation);
    CHECK(sheared.scale == start.scale);
    vector_equal(math::Vec3{world(shear, sheared)[3]}, {5, 5, 3});
}

TEST_CASE("Transform rotation preserves the pivot and distinguishes local and world axes",
          "[transform-manipulation]") {
    const auto parent = take(transform_matrix({{10, -20, 5}, around_z(quarter_turn), {2, 2, 2}}));
    const Transform start{{1, 2, 3}, around_y(quarter_turn), {2, 3, 4}};
    TransformManipulation rotate;
    rotate.operation = TransformOperation::rotate;
    rotate.rotation_axis = {1, 0, 0};
    rotate.rotation_radians = quarter_turn;
    const auto local = take(calculate_manipulated_transform(start, parent, rotate));
    CHECK(local.translation == start.translation);
    CHECK(local.scale == start.scale);
    math::Mat4 local_expected{1.0};
    local_expected[0] = {0, 0, -2, 0};
    local_expected[1] = {3, 0, 0, 0};
    local_expected[2] = {0, -4, 0, 0};
    local_expected[3] = {1, 2, 3, 1};
    matrix_equal(take(transform_matrix(local)), local_expected);

    rotate.space = TransformSpace::world;
    const auto global = take(calculate_manipulated_transform(start, parent, rotate));
    CHECK(global.translation == start.translation);
    math::Mat4 world_expected{1.0};
    world_expected[0] = {0, 4, 0, 0};
    world_expected[1] = {-6, 0, 0, 0};
    world_expected[2] = {0, 0, 8, 0};
    world_expected[3] = {6, -18, 11, 1};
    matrix_equal(world(parent, global), world_expected);
    CHECK_FALSE(transform_matrices_equivalent(world(parent, local), world_expected));

    // Normalization of enormous and subnormal finite axes must not overflow.
    for (const double magnitude : std::array{std::numeric_limits<double>::max(),
                                             std::numeric_limits<double>::denorm_min()}) {
        rotate.rotation_axis = {magnitude, 0, 0};
        matrix_equal(world(parent, take(calculate_manipulated_transform(start, parent, rotate))),
                     world_expected);
    }
}

TEST_CASE("Transform scale supports signed nonuniform local and representable world changes",
          "[transform-manipulation]") {
    const auto parent = take(transform_matrix({{10, -20, 5}, around_z(quarter_turn), {-2, 3, 4}}));
    const Transform start{{1, 2, 3}, {}, {2, -3, 4}};
    TransformManipulation scale;
    scale.operation = TransformOperation::scale;
    scale.scale_factors = {-2, .5, 3};
    const auto local = take(calculate_manipulated_transform(start, parent, scale));
    CHECK(local.translation == start.translation);
    CHECK(local.rotation == start.rotation);
    vector_equal(local.scale, {-4, -1.5, 12});

    scale.space = TransformSpace::world;
    scale.scale_factors = {2, 3, -1};
    const auto global = take(calculate_manipulated_transform(start, parent, scale));
    CHECK(global.translation == start.translation);
    math::Mat4 expected{1.0};
    expected[0] = {0, -12, 0, 0};
    expected[1] = {18, 0, 0, 0};
    expected[2] = {0, 0, -16, 0};
    expected[3] = {4, -22, 17, 1};
    matrix_equal(world(parent, global), expected);

    // Uniform world scaling commutes with a sheared ancestor while preserving
    // its child's TRS. No arbitrary decomposition of the world is permitted.
    math::Mat4 shear{1.0};
    shear[1][0] = .5;
    scale.scale_factors = {2, 2, 2};
    const auto sheared = take(calculate_manipulated_transform(start, shear, scale));
    auto doubled = world(shear, start);
    for (int column = 0; column < 3; ++column)
        for (int row = 0; row < 3; ++row)
            doubled[column][row] *= 2;
    matrix_equal(world(shear, sheared), doubled);
}

TEST_CASE("Transform snapping uses cumulative offsets and factors about identity",
          "[transform-manipulation]") {
    const Transform start{{10, 20, 30}, {}, {2, -3, 4}};
    const math::Mat4 parent{1.0};
    TransformManipulation input;
    input.snap.translation = .5;
    input.translation = {.74, -.75, .25};
    vector_equal(take(calculate_manipulated_transform(start, parent, input)).translation,
                 {10.5, 19, 30.5});
    input.translation.x = .76;
    vector_equal(take(calculate_manipulated_transform(start, parent, input)).translation,
                 {11, 19, 30.5});
    // Recomputing from the same captured start gives identical results; a
    // preview's rounded offset is never fed back into the next update.
    const auto repeated = take(calculate_manipulated_transform(start, parent, input));
    CHECK(repeated == take(calculate_manipulated_transform(start, parent, input)));

    input.operation = TransformOperation::rotate;
    input.rotation_axis = {0, 0, 1};
    input.rotation_radians = .7;
    input.snap.rotation_radians = quarter_turn;
    CHECK(take(calculate_manipulated_transform(start, parent, input)) == start);
    input.rotation_radians = .8;
    const auto rotated = take(calculate_manipulated_transform(start, parent, input));
    matrix_equal(take(transform_matrix(rotated)),
                 take(transform_matrix({start.translation, around_z(quarter_turn), start.scale})));

    input.operation = TransformOperation::scale;
    input.scale_factors = {1.26, .74, -1.24};
    input.snap.scale = .5;
    vector_equal(take(calculate_manipulated_transform(start, parent, input)).scale, {3, -1.5, -4});
    input.scale_factors = {1, 1, .74};
    input.snap.scale = 1;
    CHECK(take(calculate_manipulated_transform(start, parent, input)) == start);
    input.scale_factors.z = .5;
    rejected(calculate_manipulated_transform(start, parent, input));
}

TEST_CASE("Transform manipulation rejects shear singular and nonfinite candidates",
          "[transform-manipulation]") {
    const Transform start{{1, 2, 3}, {}, {1, 1, 1}};
    const auto parent = take(transform_matrix({{}, {}, {2, 1, 1}}));
    TransformManipulation input;
    input.space = TransformSpace::world;
    input.operation = TransformOperation::rotate;
    input.rotation_axis = {0, 0, 1};
    input.rotation_radians = quarter_turn / 2;
    rejected(calculate_manipulated_transform(start, parent, input));
    // The same rotation remains valid locally, including its sheared world.
    input.space = TransformSpace::local;
    CHECK(calculate_manipulated_transform(start, parent, input));

    input.space = TransformSpace::world;
    input.operation = TransformOperation::scale;
    input.scale_factors = {2, 1, 1};
    const Transform rotated{{1, 2, 3}, around_z(quarter_turn / 2), {1, 1, 1}};
    rejected(calculate_manipulated_transform(rotated, math::Mat4{1}, input));
    for (double invalid : std::array{0.0, 1e-7, std::numeric_limits<double>::infinity(),
                                     std::numeric_limits<double>::quiet_NaN()}) {
        input.space = TransformSpace::local;
        input.scale_factors = {invalid, 1, 1};
        rejected(calculate_manipulated_transform(start, parent, input));
    }
    input = {};
    math::Mat4 singular{1.0};
    singular[0] = singular[1];
    rejected(calculate_manipulated_transform(start, singular, input));
    math::Mat4 perspective{1.0};
    perspective[0][3] = .01;
    rejected(calculate_manipulated_transform(start, perspective, input));
    input.translation.x = std::numeric_limits<double>::max();
    rejected(calculate_manipulated_transform(start, parent, input));
    input = {};
    input.operation = static_cast<TransformOperation>(99);
    rejected(calculate_manipulated_transform(start, parent, input));
    input = {};
    input.space = static_cast<TransformSpace>(99);
    rejected(calculate_manipulated_transform(start, parent, input));
    input = {};
    input.operation = TransformOperation::rotate;
    input.rotation_axis = {0, 0, 0};
    rejected(calculate_manipulated_transform(start, parent, input));
    input = {};
    for (const double step : std::array{0.0, -1.0, std::numeric_limits<double>::infinity(),
                                        std::numeric_limits<double>::quiet_NaN()}) {
        input.snap.translation = step;
        rejected(calculate_manipulated_transform(start, parent, input));
    }
    input.translation = {1, 0, 0};
    input.snap.translation = std::numeric_limits<double>::denorm_min();
    rejected(calculate_manipulated_transform(start, parent, input));
}

TEST_CASE("Transform identity manipulations preserve canonical signed TRS representation",
          "[transform-manipulation]") {
    const Transform start{{1, 2, 3}, around_y(.37), {2, -3, -4}};
    const auto canonical = take(canonicalize_transform(start));
    math::Mat4 parent{1.0};
    parent[1][0] = .25;
    for (const auto space : {TransformSpace::local, TransformSpace::world}) {
        for (const auto operation : {TransformOperation::translate, TransformOperation::rotate,
                                     TransformOperation::scale}) {
            TransformManipulation input;
            input.space = space;
            input.operation = operation;
            CHECK(take(calculate_manipulated_transform(start, parent, input)) == canonical);
        }
    }
    const auto invalid_start = Transform{{}, {}, {0, 1, 1}};
    rejected(calculate_manipulated_transform(invalid_start, parent, {}));
}
