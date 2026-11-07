// Engine-authored walk-surface fixtures; project MIT license.
#include "shark/world_navigation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>

namespace {
using namespace shark;
template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        INFO(result.error().context.operation);
    }
    REQUIRE(result);
    return std::move(result).value();
}
WorldId world(bool other = false) {
    return WorldId{take(StableId::parse(other ? "76000000-0000-4000-8000-000000000002"
                                              : "76000000-0000-4000-8000-000000000001"))};
}
WorldCellKey key(std::int64_t x = 0) {
    return {world(), {1'000'000'000 + x, 0, -1'000'000'000}};
}
WorldPosition position(WorldCellKey cell, std::array<double, 3> offset = {}) {
    return take(normalize_world_position({cell.world, cell.cell, offset}));
}
WorldLocalFrame frame() {
    return {position(key()), 7};
}
WorldCellGeometry rectangle(double x0, double z0, double x1, double z1, double y = 0) {
    return {{{x0, y, z0}, {x0, y, z1}, {x1, y, z1}, {x1, y, z0}},
            {{0, 1, 2}, {0, 2, 3}},
            {{0, 1, 2}, {0, 2, 3}}};
}
WorldCellGeometry elbow() {
    // The horizontal [0,4]x[0,1] arm joins vertical [3,4]x[1,4].
    return {
        {{0, 0, 0}, {0, 0, 1}, {3, 0, 1}, {3, 0, 4}, {4, 0, 4}, {4, 0, 0}, {3, 0, 0}, {4, 0, 1}},
        {{0, 1, 2}, {0, 2, 6}, {6, 2, 7}, {6, 7, 5}, {2, 3, 4}, {2, 4, 7}},
        {{0, 1, 2}, {0, 2, 6}, {6, 2, 7}, {6, 7, 5}, {2, 3, 4}, {2, 4, 7}}};
}
std::unique_ptr<WorldNavigation> graph(const WorldCellGeometry& geometry) {
    const std::array tiles{WorldNavigationTile{key(), 11, geometry}};
    return take(WorldNavigation::create(frame(), tiles));
}
math::Vec3 local(const WorldPosition& point) {
    const auto value = take(world_position_relative_to(point, frame().origin, 65536));
    return {value[0], value[1], value[2]};
}
} // namespace

TEST_CASE("triangle navigation connects exact cell seams far from the origin",
          "[world-navigation]") {
    const std::array tiles{WorldNavigationTile{key(), 11, rectangle(1020, 0, 1024, 4)},
                           WorldNavigationTile{key(1), 12, rectangle(0, 0, 4, 4)}};
    const auto owner = take(WorldNavigation::create(frame(), tiles));
    const auto start = position(key(), {1021, 0, 2});
    const auto goal = position(key(1), {3, 0, 2});
    const auto route = take(owner->path(start, goal));
    REQUIRE(route.waypoints().size() >= 3);
    CHECK(route.navigation_instance() == owner->instance());
    CHECK(route.frame_generation() == frame().generation);
    CHECK(route.waypoints().front() == start);
    CHECK(route.waypoints().back() == goal);
    CHECK(std::find(route.waypoints().begin(), route.waypoints().end(),
                    position(key(1), {0, 0, 2})) != route.waypoints().end());
    const auto repeated = take(owner->path(start, goal));
    CHECK(std::equal(route.waypoints().begin(), route.waypoints().end(),
                     repeated.waypoints().begin(), repeated.waypoints().end()));
    const auto cursor = take(owner->begin(route));
    const auto completed = take(owner->advance(route, cursor, 32));
    CHECK(completed.position == goal);
    CHECK(take(owner->advance(route, completed, 1)).position == goal);
    CHECK(take(owner->advance(route, cursor, 0)).position == start);
    CHECK(cursor.position == start); // Advance returns a pure offered candidate.
    const auto missing = take(WorldNavigation::create(frame(), std::span{tiles}.first(1)));
    CHECK_FALSE(missing->path(start, goal));
    CHECK_FALSE(owner->path(start, goal, 1));
    CHECK_FALSE(owner->path(start, goal, world_navigation_maximum_triangles, 2));
}

TEST_CASE("bounded navigation pursuit follows every portal without cutting an elbow",
          "[world-navigation]") {
    const auto owner = graph(elbow());
    const auto start = position(key(), {0.5, 0, 0.5});
    const auto goal = position(key(), {3.5, 0, 3.5});
    const auto route = take(owner->path(start, goal));
    REQUIRE(route.waypoints().size() > 2);
    auto cursor = take(owner->begin(route));
    for (std::size_t step = 0; step < 128 && cursor.position != goal; ++step) {
        const auto candidate = take(owner->advance(route, cursor, 0.125));
        const auto before = local(cursor.position);
        const auto after = local(candidate.position);
        CHECK(glm::length(after - before) <= 0.125 + 1e-9);
        CHECK(after.y == 0.0);
        CHECK(after.x >= 0.0);
        CHECK(after.x <= 4.0);
        CHECK(after.z >= 0.0);
        CHECK(after.z <= 4.0);
        if (after.z > 1.0)
            CHECK(after.x >= 3.0);
        cursor = candidate;
    }
    CHECK(cursor.position == goal);
    CHECK(take(owner->advance(route, take(owner->begin(route)), 64)).position == goal);
    CHECK_FALSE(owner->path(start, position(key(), {1, 0, 2}))); // Outside both arms.
}

TEST_CASE("walk projection adjusts only bounded vertical distance on the authored surface",
          "[world-navigation]") {
    auto slope = rectangle(0, 0, 4, 4);
    slope.vertices[2].y = 2;
    slope.vertices[3].y = 2;
    const auto owner = graph(slope);
    const auto exact = position(key(), {2, 1, 2});
    const auto near = position(key(), {2, 1 + 1.0 / 2048, 2});
    CHECK(take(owner->project_walk_surface(near)) == exact);
    CHECK(take(owner->project_walk_surface(exact, 0)) == exact);
    // Projection is an explicit adapter; ordinary path membership stays strict.
    CHECK_FALSE(owner->path(near, position(key(), {3, 1.5, 2})));
    const auto farther = position(key(), {2, 1 + 1.0 / 512, 2});
    CHECK_FALSE(owner->project_walk_surface(farther));
    CHECK(take(owner->project_walk_surface(farther, 1.0 / 256)) == exact);
    CHECK_FALSE(owner->project_walk_surface(position(key(), {5, 2.5, 2})));
    auto foreign = near;
    foreign.world = world(true);
    CHECK_FALSE(owner->project_walk_surface(foreign));
    for (const auto bound : {-1.0, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN(), 65537.0})
        CHECK_FALSE(owner->project_walk_surface(near, bound));
    const auto empty = take(WorldNavigation::create(frame(), {}));
    CHECK_FALSE(empty->project_walk_surface(exact));

    const std::array next_tile{WorldNavigationTile{key(1), 12, rectangle(0, 0, 4, 4)}};
    const auto fractional =
        take(WorldNavigation::create({position(key(), {1023.9, 0, .1}), 8}, next_tile));
    const auto precise = position(key(1), {.1, 1.0 / 2048, 2.1});
    const auto projected = take(fractional->project_walk_surface(precise));
    CHECK(projected == position(key(1), {.1, 0, 2.1}));
    CHECK(projected.cell.x == precise.cell.x);
    CHECK(projected.cell.z == precise.cell.z);
    CHECK(projected.offset[0] == precise.offset[0]);
    CHECK(projected.offset[2] == precise.offset[2]);
}

TEST_CASE("navigation rejects overlapping layers and invalid surface topology",
          "[world-navigation]") {
    auto geometry = rectangle(0, 0, 4, 4);
    const auto initial = graph(geometry);
    const auto valid = take(initial->path(position(key(), {1, 0, 1}), position(key(), {3, 0, 3})));
    SECTION("an overlapping upper surface is not silently selected as another floor") {
        const auto upper = rectangle(0, 0, 4, 4, 1);
        geometry.vertices.insert(geometry.vertices.end(), upper.vertices.begin(),
                                 upper.vertices.end());
        for (auto triangle : upper.collision_triangles) {
            for (auto& index : triangle)
                index += 4;
            geometry.collision_triangles.push_back(triangle);
            geometry.navigation_triangles.push_back(triangle);
        }
        const std::array tiles{WorldNavigationTile{key(), 11, geometry}};
        CHECK_FALSE(WorldNavigation::create(frame(), tiles));
    }
    SECTION("partial positive-area overlap rejects even when triangles share no full edge") {
        geometry.vertices.insert(geometry.vertices.end(), {{1, 0, 1}, {1, 0, 3}, {3, 0, 3}});
        geometry.collision_triangles.push_back({4, 5, 6});
        geometry.navigation_triangles.push_back({4, 5, 6});
        const std::array tiles{WorldNavigationTile{key(), 11, geometry}};
        CHECK_FALSE(WorldNavigation::create(frame(), tiles));
    }
    SECTION("a third triangle on an edge cannot form a valid single-layer manifold") {
        geometry.vertices.push_back({2, 0, 3});
        geometry.collision_triangles.push_back({0, 1, 4});
        geometry.navigation_triangles.push_back({0, 1, 4});
        const std::array tiles{WorldNavigationTile{key(), 11, geometry}};
        CHECK_FALSE(WorldNavigation::create(frame(), tiles));
    }
    SECTION("invalid source index fails through shared geometry validation") {
        geometry.navigation_triangles[0][2] = 4000;
        const std::array tiles{WorldNavigationTile{key(), 11, geometry}};
        CHECK_FALSE(WorldNavigation::create(frame(), tiles));
    }
    CHECK(take(initial->begin(valid)).position == valid.waypoints().front());
}

TEST_CASE("navigation paths and progress remain bound to their graph frame and route",
          "[world-navigation]") {
    const auto geometry = rectangle(0, 0, 4, 4);
    const auto owner = graph(geometry);
    const auto start = position(key(), {1, 0, 1});
    const auto goal = position(key(), {3, 0, 3});
    const auto route = take(owner->path(start, goal));
    const auto cursor = take(owner->begin(route));
    const auto replacement = graph(geometry);
    CHECK(replacement->instance() != owner->instance());
    CHECK_FALSE(replacement->begin(route));
    CHECK_FALSE(replacement->advance(route, cursor, 1));
    const auto another = take(owner->path(start, goal));
    CHECK_FALSE(owner->advance(another, cursor, 1));
    auto invalid = cursor;
    invalid.next_waypoint = route.waypoints().size();
    CHECK_FALSE(owner->advance(route, invalid, 1));
    invalid.next_waypoint = 0;
    CHECK_FALSE(owner->advance(route, invalid, 1));
    invalid = cursor;
    invalid.position = position(key(), {1, 1, 1});
    CHECK_FALSE(owner->advance(route, invalid, 1));
    for (const auto distance : {-1.0, std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::quiet_NaN(), 65537.0})
        CHECK_FALSE(owner->advance(route, cursor, distance));
    CHECK(cursor.position == start);
}

TEST_CASE("navigation requires ready surface endpoints and finite bounded queries",
          "[world-navigation]") {
    const auto owner = graph(rectangle(0, 0, 4, 4));
    const auto start = position(key(), {1, 0, 1});
    const auto goal = position(key(), {3, 0, 3});
    CHECK_FALSE(owner->path(start, goal, 0));
    CHECK_FALSE(owner->path(start, goal, 2049));
    CHECK_FALSE(owner->path(start, goal, 2048, 1));
    CHECK_FALSE(owner->path(start, goal, 2048, 2049));
    CHECK_FALSE(owner->path(start, position(key(), {3, 0.5, 3})));
    auto foreign = goal;
    foreign.world = world(true);
    CHECK_FALSE(owner->path(start, foreign));
    const auto empty = take(WorldNavigation::create(frame(), {}));
    CHECK_FALSE(empty->path(start, goal));
    auto no_navigation = rectangle(0, 0, 4, 4);
    no_navigation.navigation_triangles.clear();
    const auto collision_only = graph(no_navigation);
    CHECK_FALSE(collision_only->path(start, goal));
    auto invalid_frame = frame();
    invalid_frame.generation = 0;
    CHECK_FALSE(WorldNavigation::create(invalid_frame, {}));
    const std::array zero_generation{WorldNavigationTile{key(), 0, rectangle(0, 0, 4, 4)}};
    CHECK_FALSE(WorldNavigation::create(frame(), zero_generation));
    const std::array duplicates{WorldNavigationTile{key(), 1, rectangle(0, 0, 1, 1)},
                                WorldNavigationTile{key(), 2, rectangle(2, 0, 3, 1)}};
    CHECK_FALSE(WorldNavigation::create(frame(), duplicates));
}

TEST_CASE("authored navigation is a centerline profile without implied radius clearance",
          "[world-navigation]") {
    const auto owner = graph(rectangle(0, 0, 4, 0.25));
    const auto start = position(key(), {0.25, 0, 0.125});
    const auto goal = position(key(), {3.75, 0, 0.125});
    const auto route = take(owner->path(start, goal));
    CHECK(take(owner->advance(route, take(owner->begin(route)), 16)).position == goal);
    // Cell-local exact-grid slopes retain their actual surface height.
    auto slope = rectangle(0, 0, 4, 4);
    slope.vertices[2].y = 2;
    slope.vertices[3].y = 2;
    const auto sloped = graph(slope);
    const auto sloped_path =
        take(sloped->path(position(key(), {1, 0.5, 1}), position(key(), {3, 1.5, 3})));
    const auto advanced =
        take(sloped->advance(sloped_path, take(sloped->begin(sloped_path)), 0.25));
    const auto candidate = local(advanced.position);
    CHECK(std::abs(candidate.y - candidate.x * 0.5) <= 1e-7);
}
