// Original Shark in-memory static-cell fixtures; project MIT license.
#include "shark/content_digest.hpp"
#include "shark/world_cell.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <string>
#include <thread>

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

template <typename Id> Id id(std::uint32_t value) {
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 0xfU];
        value >>= 4U;
    }
    return Id{take(StableId::parse(text))};
}

PreparedWorldCell sample() {
    const WorldCellKey key{id<WorldId>(4), {world_cell_min, 42, world_cell_max}};
    PreparedEntity root{id<AuthoredEntityId>(1)};
    root.name.value = "Ground";
    root.transform.translation = math::Vec3{10, 0, 0};
    root.rigid_body = RigidBody{};
    PreparedEntity child{id<AuthoredEntityId>(2), root.id, Name{"Marker"}};
    child.transform.translation = math::Vec3{3, 0, 0};
    child.mesh_renderer = MeshRenderer{builtin_plane_asset_id(), 0, false};
    return {key, 7, {id<SceneId>(3), WorldPosition{key.world, key.cell, {0, 0, 0}}, {child, root}}};
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

void repair_digest(std::vector<std::byte>& output) {
    const auto digest = take(sha256(std::span<const std::byte>{output}.subspan(44)));
    std::copy(digest.bytes.begin(), digest.bytes.end(), output.begin() + 12);
}

// The cell field order and primitive bytes are independent of its encoder. The
// nested scene uses its existing separately qualified codec and complete envelope.
std::vector<std::byte> independent_envelope(const PreparedWorldCell& cell) {
    const auto scene = take(encode_prepared_scene(cell.scene));
    std::vector<std::byte> output;
    for (const char value : std::string{"SHKCELL1"})
        output.push_back(static_cast<std::byte>(value));
    append(output, 1, 4);
    output.resize(44);
    const auto& world = cell.key.world.value().bytes();
    output.insert(output.end(), world.begin(), world.end());
    for (const auto axis : {cell.key.cell.x, cell.key.cell.y, cell.key.cell.z})
        append(output, std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(axis)), 4);
    append(output, cell.content_revision, 8);
    append(output, scene.size(), 4);
    REQUIRE(output.size() == 84);
    output.insert(output.end(), scene.begin(), scene.end());
    repair_digest(output);
    return output;
}

SceneEntityState state(const LoadedWorldCell& loaded, AuthoredEntityId entity) {
    return take(loaded.scene().entity_state(take(loaded.scene().find(entity))));
}
} // namespace

TEST_CASE("world cell independent envelope preserves far cells and static content",
          "[world-cell]") {
    const auto original = sample();
    const auto independent = independent_envelope(original);
    CHECK(take(decode_world_cell(independent)) == original);
    CHECK(take(encode_world_cell(original)) == independent);
    const auto assets = take(PreparedAssets::create());
    const auto loaded = take(LoadedWorldCell::load(independent, assets));
    CHECK(loaded->key() == original.key);
    CHECK(loaded->content_revision() == 7);
    CHECK(loaded->origin() == WorldPosition{original.key.world, original.key.cell, {0, 0, 0}});
    CHECK(loaded->assets() == assets);
    CHECK(state(*loaded, id<AuthoredEntityId>(2)).world_matrix[3][0] == 13);
    CHECK(state(*loaded, id<AuthoredEntityId>(2)).mesh_renderer ==
          MeshRenderer{builtin_plane_asset_id(), 0, false});
    CHECK(state(*loaded, id<AuthoredEntityId>(1)).rigid_body == RigidBody{});
    CHECK(std::holds_alternative<AuthoredSceneScope>(
        state(*loaded, id<AuthoredEntityId>(1)).reference.scope));
    CHECK(take(loaded->scene().stats()).live_entities == 2);

    auto maximum_revision = original;
    maximum_revision.content_revision = std::numeric_limits<std::uint64_t>::max();
    CHECK(take(decode_world_cell(take(encode_world_cell(maximum_revision)))) == maximum_revision);
}

TEST_CASE("world cell rejects malformed envelopes before activation", "[world-cell]") {
    const auto good = independent_envelope(sample());
    for (const auto offset : {0U, 8U, 12U, 44U, 84U}) {
        auto invalid = good;
        invalid[offset] ^= std::byte{0x7f};
        CHECK_FALSE(decode_world_cell(invalid));
    }
    for (const std::size_t length : {0U, 43U, 44U, 83U})
        CHECK_FALSE(decode_world_cell(std::span<const std::byte>{good}.first(length)));
    CHECK_FALSE(decode_world_cell(
        std::vector<std::byte>(world_cell_maximum_payload_bytes + 1, std::byte{0})));

    auto invalid = good;
    replace(invalid, 72, 0, 8); // Content revision is not an authority epoch.
    repair_digest(invalid);
    CHECK_FALSE(decode_world_cell(invalid));
    invalid = good;
    replace(invalid, 80, std::numeric_limits<std::uint32_t>::max(), 4);
    repair_digest(invalid);
    CHECK_FALSE(decode_world_cell(invalid));
    invalid = good;
    replace(invalid, 80, 0, 4);
    repair_digest(invalid);
    CHECK_FALSE(decode_world_cell(invalid));
    invalid = good;
    invalid.push_back(std::byte{0});
    repair_digest(invalid);
    CHECK_FALSE(decode_world_cell(invalid));
    invalid = good;
    invalid.pop_back();
    repair_digest(invalid);
    CHECK_FALSE(decode_world_cell(invalid));
    invalid = good;
    invalid[84 + 44] ^= std::byte{1};
    repair_digest(invalid); // A valid outer digest cannot bypass the nested digest.
    CHECK_FALSE(decode_world_cell(invalid));

    invalid = good;
    replace(invalid, 60, 0, 4); // Valid cell, different from the captured scene anchor.
    repair_digest(invalid);
    CHECK_FALSE(decode_world_cell(invalid));
    invalid = good;
    const auto other_world = id<WorldId>(99).value().bytes();
    std::copy(other_world.begin(), other_world.end(), invalid.begin() + 44);
    repair_digest(invalid);
    CHECK_FALSE(decode_world_cell(invalid));
}

TEST_CASE("world cell requires exact canonical source anchor and positive revision",
          "[world-cell]") {
    const auto good = sample();
    auto invalid = good;
    invalid.content_revision = 0;
    CHECK_FALSE(encode_world_cell(invalid));
    invalid = good;
    invalid.key.cell.x = world_cell_min - 1;
    CHECK_FALSE(encode_world_cell(invalid));
    invalid = good;
    invalid.key.cell.z = world_cell_max + 1;
    CHECK_FALSE(encode_world_cell(invalid));
    invalid = good;
    invalid.scene.world_origin.reset();
    CHECK_FALSE(encode_world_cell(invalid));
    CHECK_FALSE(decode_world_cell(independent_envelope(invalid)));
    invalid = good;
    invalid.scene.world_origin->offset[0] = 0.125;
    CHECK_FALSE(encode_world_cell(invalid));
    CHECK_FALSE(decode_world_cell(independent_envelope(invalid)));
    invalid = good;
    invalid.scene.world_origin->offset[0] = -0.0;
    CHECK_FALSE(encode_world_cell(invalid));
    invalid = good;
    invalid.scene.world_origin->world = id<WorldId>(99);
    CHECK_FALSE(encode_world_cell(invalid));
    CHECK_FALSE(decode_world_cell(independent_envelope(invalid)));
}

TEST_CASE("world cell static profile rejects active content in both codec directions",
          "[world-cell]") {
    const auto good = sample();
    auto invalid = good;
    invalid.scene.entities[1].camera = Camera{};
    CHECK_FALSE(encode_world_cell(invalid));
    CHECK_FALSE(decode_world_cell(independent_envelope(invalid)));
    for (const bool enabled : {false, true}) {
        invalid = good;
        invalid.scene.entities[1].script = Script{"scripts/main.lua", enabled};
        CHECK_FALSE(encode_world_cell(invalid));
        CHECK_FALSE(decode_world_cell(independent_envelope(invalid)));
    }
    for (const auto motion : {BodyMotion::dynamic_body, BodyMotion::kinematic_body}) {
        invalid = good;
        invalid.scene.entities[1].rigid_body->motion = motion;
        CHECK_FALSE(encode_world_cell(invalid));
        CHECK_FALSE(decode_world_cell(independent_envelope(invalid)));
    }
    invalid = good;
    invalid.scene.entities[1].rigid_body->is_trigger = true;
    CHECK_FALSE(encode_world_cell(invalid));
    CHECK_FALSE(decode_world_cell(independent_envelope(invalid)));

    invalid = good;
    invalid.scene.entities.push_back(invalid.scene.entities[0]);
    CHECK_FALSE(encode_world_cell(invalid));
    invalid = good;
    invalid.scene.entities[0].parent = id<AuthoredEntityId>(99);
    CHECK_FALSE(encode_world_cell(invalid));
}

TEST_CASE("world cell resolves hidden mesh references and preserves last valid owner",
          "[world-cell]") {
    const auto assets = take(PreparedAssets::create());
    const auto original = sample();
    const auto valid = take(LoadedWorldCell::load(take(encode_world_cell(original)), assets));
    const auto stats = take(valid->scene().stats());
    const auto reference = state(*valid, id<AuthoredEntityId>(2)).reference;
    CHECK_FALSE(LoadedWorldCell::load(take(encode_world_cell(original)), nullptr));
    for (const auto renderer : {MeshRenderer{id<AssetId>(99), 0, false},
                                MeshRenderer{builtin_plane_asset_id(), 1, false}}) {
        auto invalid = original;
        invalid.scene.entities[0].mesh_renderer = renderer;
        const auto bytes = take(encode_world_cell(invalid));
        CHECK(decode_world_cell(bytes)); // Asset closure is a separate load boundary.
        CHECK_FALSE(LoadedWorldCell::load(bytes, assets));
        CHECK(take(valid->scene().stats()).instance == stats.instance);
        CHECK(take(valid->scene().stats()).revision == stats.revision);
        CHECK(take(valid->scene().resolve(reference)) == reference.handle);
        CHECK(state(*valid, id<AuthoredEntityId>(2)).world_matrix[3][0] == 13);
    }
    const auto replacement = take(LoadedWorldCell::load(take(encode_world_cell(original)), assets));
    CHECK(take(replacement->scene().stats()).instance != stats.instance);
    CHECK_FALSE(replacement->scene().resolve(reference));
}

TEST_CASE("world cell owns decoded content and retains creating-thread scene checks",
          "[world-cell]") {
    auto bytes = take(encode_world_cell(sample()));
    auto assets = take(PreparedAssets::create());
    auto loaded = take(LoadedWorldCell::load(bytes, assets));
    std::vector<std::byte>{}.swap(bytes);
    assets.reset();
    CHECK(state(*loaded, id<AuthoredEntityId>(1)).name.value == "Ground");
    const auto mesh = take(loaded->assets()->find(builtin_plane_asset_id()));
    bool rejected{};
    std::thread other_thread([&] {
        const auto result = loaded->scene().stats();
        rejected = !result && result.error().code == ErrorCode::conflict;
    });
    other_thread.join();
    CHECK(rejected);
    loaded.reset();
    REQUIRE(mesh->meshes.size() == 1);
    REQUIRE(mesh->meshes[0].primitives.size() == 1);
    CHECK(mesh->meshes[0].primitives[0].vertices.size() == 4);
}
