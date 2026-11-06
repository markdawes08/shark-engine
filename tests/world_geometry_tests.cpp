// Original Shark geometry and development-envelope fixtures; project MIT license.
#include "shark/content_digest.hpp"
#include "shark/world_cell.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <string>

namespace {
using namespace shark;
template <typename T> T take(Result<T> value) {
    if (!value)
        INFO(value.error().message);
    REQUIRE(value);
    return std::move(value).value();
}
template <typename Id> Id id(char digit) {
    std::string value{"63000000-0000-4000-8000-000000000001"};
    value.back() = digit;
    return Id{take(StableId::parse(value))};
}
WorldCellGeometry surface() {
    return {{{0, 0, 0}, {0, 0, 1024}, {1024, 0, 0}, {1024, 0, 1024}},
            {{0, 1, 2}, {2, 1, 3}},
            {{0, 1, 2}, {2, 1, 3}}};
}
PreparedWorldCell cell(bool geometry) {
    const WorldCellKey key{id<WorldId>('1'), {world_cell_max, 0, world_cell_min}};
    PreparedWorldCell result{
        key, 9, {id<SceneId>('2'), WorldPosition{key.world, key.cell, {0, 0, 0}}, {}}};
    if (geometry)
        result.geometry = surface();
    return result;
}
void append(std::vector<std::byte>& output, std::uint64_t value, unsigned width) {
    for (unsigned index = 0; index < width; ++index)
        output.push_back(static_cast<std::byte>((value >> (index * 8U)) & 255U));
}
void replace(std::vector<std::byte>& output, std::size_t offset, std::uint64_t value,
             unsigned width) {
    for (unsigned index = 0; index < width; ++index)
        output.at(offset + index) = static_cast<std::byte>((value >> (index * 8U)) & 255U);
}
void digest(std::vector<std::byte>& output) {
    const auto hash = take(sha256(std::span<const std::byte>{output}.subspan(44)));
    std::copy(hash.bytes.begin(), hash.bytes.end(), output.begin() + 12);
}
std::vector<std::byte> independent(const PreparedWorldCell& input) {
    const auto scene = take(encode_prepared_scene(input.scene));
    std::vector<std::byte> output;
    for (const auto character : std::string{"SHKCELL1"})
        output.push_back(static_cast<std::byte>(character));
    append(output, 2, 4);
    output.resize(44);
    const auto& world = input.key.world.value().bytes();
    output.insert(output.end(), world.begin(), world.end());
    for (const auto axis : {input.key.cell.x, input.key.cell.y, input.key.cell.z})
        append(output, std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(axis)), 4);
    append(output, input.content_revision, 8);
    append(output, scene.size(), 4);
    output.insert(output.end(), scene.begin(), scene.end());
    const auto& geometry = *input.geometry;
    append(output, geometry.vertices.size(), 4);
    for (const auto& vertex : geometry.vertices)
        for (int axis = 0; axis < 3; ++axis)
            append(output, std::bit_cast<std::uint64_t>(vertex[axis]), 8);
    for (const auto* triangles : {&geometry.collision_triangles, &geometry.navigation_triangles}) {
        append(output, triangles->size(), 4);
        for (const auto& triangle : *triangles)
            for (const auto index : triangle)
                append(output, index, 4);
    }
    digest(output);
    return output;
}
} // namespace

TEST_CASE("cell geometry admits exact grid collision and explicit walkable subsets",
          "[world-geometry]") {
    const auto good = surface();
    CHECK(validate_world_cell_geometry(good));
    auto subset = good;
    subset.navigation_triangles = {{1, 2, 0}}; // Same oriented face, different starting vertex.
    CHECK(validate_world_cell_geometry(subset));
    subset.navigation_triangles.clear();
    CHECK(validate_world_cell_geometry(subset));
    auto slope = good;
    slope.vertices[2].y = 1024;
    slope.vertices[3].y = 1024;
    CHECK(validate_world_cell_geometry(slope)); // Exact 45 degree endpoint.
}

TEST_CASE("cell geometry rejects malformed packed surfaces without changing input",
          "[world-geometry]") {
    const auto good = surface();
    auto invalid = good;
    invalid.vertices[0].x = 0.1;
    CHECK_FALSE(validate_world_cell_geometry(invalid));
    for (const auto number : {-1.0, 1024.001, std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN()}) {
        invalid = good;
        invalid.vertices[0].y = number;
        CHECK_FALSE(validate_world_cell_geometry(invalid));
    }
    invalid = good;
    invalid.collision_triangles[0][2] = 4;
    CHECK_FALSE(validate_world_cell_geometry(invalid));
    invalid = good;
    invalid.vertices[2] = invalid.vertices[0];
    CHECK_FALSE(validate_world_cell_geometry(invalid));
    invalid = good;
    invalid.vertices.push_back(good.vertices[0]);
    invalid.collision_triangles.push_back({4, 2, 1});
    CHECK_FALSE(
        validate_world_cell_geometry(invalid)); // Geometric duplicate despite index/winding.
    invalid = good;
    invalid.vertices.resize(world_geometry_maximum_vertices + 1);
    CHECK_FALSE(validate_world_cell_geometry(invalid));
    invalid = good;
    invalid.collision_triangles.resize(world_geometry_maximum_collision_triangles + 1);
    CHECK_FALSE(validate_world_cell_geometry(invalid));
    invalid = good;
    invalid.navigation_triangles.resize(world_geometry_maximum_navigation_triangles + 1);
    CHECK_FALSE(validate_world_cell_geometry(invalid));
    CHECK_FALSE(validate_world_cell_geometry({}));
    CHECK(surface() == good);
}

TEST_CASE("navigation selection rejects unsupported slope orientation and noncollision faces",
          "[world-geometry]") {
    auto invalid = surface();
    invalid.navigation_triangles = {{0, 2, 1}};
    CHECK_FALSE(validate_world_cell_geometry(invalid));
    invalid = surface();
    invalid.navigation_triangles = {{0, 2, 3}};
    CHECK_FALSE(validate_world_cell_geometry(invalid));
    invalid = surface();
    invalid.navigation_triangles.push_back({1, 2, 0});
    CHECK_FALSE(validate_world_cell_geometry(invalid));
    invalid = surface();
    invalid.vertices[2].x = 512;
    invalid.vertices[3].x = 512;
    invalid.vertices[2].y = 1024;
    invalid.vertices[3].y = 1024;
    CHECK_FALSE(validate_world_cell_geometry(invalid));
}

TEST_CASE("cell geometry version two bytes are independent and version one remains compatible",
          "[world-geometry]") {
    const auto source = cell(true);
    auto bytes = independent(source);
    CHECK(take(encode_world_cell(source)) == bytes);
    CHECK(take(decode_world_cell(bytes)) == source);
    const auto assets = take(PreparedAssets::create());
    const auto loaded = take(LoadedWorldCell::load(bytes, assets));
    bytes.clear();
    bytes.shrink_to_fit();
    REQUIRE(loaded->geometry());
    CHECK(*loaded->geometry() == surface());
    CHECK(loaded->key() == source.key);

    const auto legacy = take(encode_world_cell(cell(false)));
    CHECK(legacy.at(8) == std::byte{1});
    CHECK_FALSE(take(decode_world_cell(legacy)).geometry);
    CHECK_FALSE(take(LoadedWorldCell::load(legacy, assets))->geometry());
}

TEST_CASE("cell geometry envelopes reject bounded count and semantic attacks", "[world-geometry]") {
    const auto source = cell(true);
    const auto good = independent(source);
    const auto scene_size = take(encode_prepared_scene(source.scene)).size();
    const auto geometry_start = 84 + scene_size;
    auto invalid = good;
    replace(invalid, geometry_start, std::numeric_limits<std::uint32_t>::max(), 4);
    digest(invalid);
    CHECK_FALSE(decode_world_cell(invalid));
    invalid = good;
    replace(invalid, geometry_start + 4, std::bit_cast<std::uint64_t>(0.1), 8);
    digest(invalid);
    CHECK_FALSE(decode_world_cell(invalid));
    invalid = good;
    replace(invalid, 8, 3, 4);
    CHECK_FALSE(decode_world_cell(invalid));
    invalid = good;
    replace(invalid, 8, 1, 4);
    CHECK_FALSE(decode_world_cell(invalid)); // Legacy envelope cannot hide trailing geometry.
    invalid = take(encode_world_cell(cell(false)));
    replace(invalid, 8, 2, 4);
    CHECK_FALSE(decode_world_cell(invalid));
    invalid = good;
    invalid.pop_back();
    digest(invalid);
    CHECK_FALSE(decode_world_cell(invalid));
    invalid = good;
    invalid.push_back(std::byte{0});
    digest(invalid);
    CHECK_FALSE(decode_world_cell(invalid));
}
