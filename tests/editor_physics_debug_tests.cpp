#include "../editor/physics_debug.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace {
using namespace shark;
using namespace shark::editor;

template <typename T> T take(Result<T> result) {
    if (!result)
        INFO(result.error().message);
    REQUIRE(result);
    return std::move(result).value();
}
void near(double value, double expected) {
    CHECK(value == Catch::Approx(expected).margin(1e-8));
}
std::array<math::Vec3, 2> bounds(const std::vector<PhysicsDebugLine>& lines) {
    const double inf = std::numeric_limits<double>::infinity();
    std::array<math::Vec3, 2> result{math::Vec3{inf}, math::Vec3{-inf}};
    for (const auto& line : lines)
        for (const auto& point : {line.first, line.second})
            for (int axis = 0; axis < 3; ++axis) {
                result[0][axis] = std::min(result[0][axis], point[axis]);
                result[1][axis] = std::max(result[1][axis], point[axis]);
            }
    return result;
}
} // namespace

TEST_CASE("Physics debug wire shapes use native committed pose and uniform scale",
          "[editor-physics]") {
    PhysicsDebugBody body;
    body.pose.translation = {10, 0, -5};
    body.pose.rotation = {std::sqrt(.5), 0, 0, std::sqrt(.5)};
    body.pose.scale = {2, 2, 2};
    body.settings.half_extents = {1, 2, 3};
    body.settings.motion = BodyMotion::dynamic_body;
    const std::array bodies{body};
    const auto box = take(make_physics_debug_lines(bodies));
    REQUIRE(box.size() == 12);
    CHECK(std::all_of(box.begin(), box.end(), [](const auto& line) {
        return line.kind == PhysicsDebugKind::dynamic_body;
    }));
    const auto box_bounds = bounds(box);
    near(box_bounds[0].x, 6);
    near(box_bounds[1].x, 14);
    near(box_bounds[0].y, -2);
    near(box_bounds[1].y, 2);
    near(box_bounds[0].z, -11);
    near(box_bounds[1].z, 1);

    body.pose.rotation = {1, 0, 0, 0};
    body.settings.shape = ColliderShape::sphere;
    body.settings.radius = 1;
    body.settings.is_trigger = true;
    const auto ball = take(make_physics_debug_lines(std::array{body}));
    REQUIRE(ball.size() == 72);
    CHECK(ball.front().kind == PhysicsDebugKind::trigger);
    const auto sphere_bounds = bounds(ball);
    near(sphere_bounds[0].x, 8);
    near(sphere_bounds[1].x, 12);
    near(sphere_bounds[0].y, -2);
    near(sphere_bounds[1].y, 2);

    body.settings.shape = ColliderShape::capsule;
    body.settings.half_height = 1.5;
    const auto capsule = take(make_physics_debug_lines(std::array{body}));
    REQUIRE(capsule.size() == 100);
    const auto capsule_bounds = bounds(capsule);
    near(capsule_bounds[0].x, 8);
    near(capsule_bounds[1].x, 12);
    near(capsule_bounds[0].y, -5);
    near(capsule_bounds[1].y, 5);
}

TEST_CASE("Physics debug query geometry is bounded and reflects the requested shapes",
          "[editor-physics]") {
    PhysicsDebugQuery query{PhysicsDebugQueryKind::raycast, {0, 0, 0}, {2, 0, 0}, 0, {{1, 0, 0}}};
    auto lines = take(make_physics_debug_lines({}, query));
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].kind == PhysicsDebugKind::query);
    CHECK(lines[1].kind == PhysicsDebugKind::hit);
    near(lines[0].second.x, 2);
    query = {PhysicsDebugQueryKind::overlap_sphere, {2, 3, 4}, {}, 2, {}};
    lines = take(make_physics_debug_lines({}, query));
    REQUIRE(lines.size() == 72);
    const auto ball = bounds(lines);
    near(ball[0].x, 0);
    near(ball[1].z, 6);
    query = {PhysicsDebugQueryKind::overlap_box, {2, 3, 4}, {1, 2, 3}, 0, {}};
    lines = take(make_physics_debug_lines({}, query));
    REQUIRE(lines.size() == 12);
    const auto box = bounds(lines);
    near(box[0].y, 1);
    near(box[1].z, 7);

    auto invalid = PhysicsDebugBody{};
    invalid.pose.scale = {1, -1, 1};
    CHECK_FALSE(make_physics_debug_lines(std::array{invalid}));
    query.second.x = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(make_physics_debug_lines({}, query));
    query = {PhysicsDebugQueryKind::raycast,
             {0, 0, 0},
             {1, 0, 0},
             0,
             std::vector<math::Vec3>(physics_debug_maximum_hits + 1)};
    CHECK_FALSE(make_physics_debug_lines({}, query));
    CHECK_FALSE(
        make_physics_debug_lines(std::vector<PhysicsDebugBody>(physics_debug_maximum_bodies + 1)));
}

TEST_CASE("Physics debug projection clips near and screen planes using image and render origin",
          "[editor-physics]") {
    const ViewportRectangle image{100, 50, 400, 200};
    const std::array input{
        PhysicsDebugLine{{-2, 0, .5}, {2, 0, .5}, PhysicsDebugKind::query},
        PhysicsDebugLine{{-.5, 0, -1}, {.5, 0, 1}, PhysicsDebugKind::hit},
        PhysicsDebugLine{{0, 0, -2}, {.5, 0, -1}, PhysicsDebugKind::static_body}};
    const auto projected = take(project_physics_debug_lines(input, math::Mat4{1}, {}, image));
    REQUIRE(projected.size() == 2);
    near(projected[0].x0, 100);
    near(projected[0].x1, 500);
    near(projected[0].y0, 150);
    near(projected[1].x0, 300); // z=0 near-plane intersection.
    near(projected[1].x1, 400);
    const std::array shifted{PhysicsDebugLine{{2, 4, .5}, {3, 4, .5}, PhysicsDebugKind::query}};
    const auto origin = take(project_physics_debug_lines(shifted, math::Mat4{1}, {2, 4, 0}, image));
    REQUIRE(origin.size() == 1);
    near(origin[0].x0, 300);
    near(origin[0].x1, 500);
    auto invalid = input;
    invalid[1].first.x = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(project_physics_debug_lines(invalid, math::Mat4{1}, {}, image));
    auto matrix = math::Mat4{1};
    matrix[0][0] = std::numeric_limits<double>::infinity();
    CHECK_FALSE(project_physics_debug_lines(input, matrix, {}, image));
    CHECK_FALSE(project_physics_debug_lines(input, math::Mat4{1}, {}, {0, 0, 0, 20}));
}
