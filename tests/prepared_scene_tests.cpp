// Original Shark in-memory development-format fixtures; project MIT license.
#include "shark/content_digest.hpp"
#include "shark/prepared_scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
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
PreparedScene sample() {
    PreparedEntity parent{id<AuthoredEntityId>(1)};
    parent.name.value = "Root";
    parent.transform.translation = math::Vec3{10, 0, 0};
    parent.camera = Camera{};
    PreparedEntity child{id<AuthoredEntityId>(2), parent.id, Name{"Child"}};
    child.transform.translation = math::Vec3{3, 0, 0};
    child.transform.scale = math::Vec3{-2, 1, 1};
    child.mesh_renderer = MeshRenderer{builtin_plane_asset_id(), 0, false};
    return {
        id<SceneId>(3),
        WorldPosition{id<WorldId>(4), {world_cell_min, 42, world_cell_max}, {0.125, 1023.75, 0.0}},
        {child, parent}}; // Deliberate forward parent reference.
}
void append(std::vector<std::byte>& output, std::uint64_t value, unsigned width) {
    for (unsigned index = 0; index < width; ++index)
        output.push_back(static_cast<std::byte>((value >> (index * 8U)) & 255U));
}
void append_id(std::vector<std::byte>& output, const StableId& value) {
    output.insert(output.end(), value.bytes().begin(), value.bytes().end());
}
void replace(std::vector<std::byte>& output, std::size_t offset, std::uint64_t value,
             unsigned width) {
    for (unsigned index = 0; index < width; ++index)
        output.at(offset + index) = static_cast<std::byte>((value >> (index * 8U)) & 255U);
}
void repair_digest(std::vector<std::byte>& payload) {
    const auto digest = take(sha256(std::span<const std::byte>{payload}.subspan(44)));
    std::copy(digest.bytes.begin(), digest.bytes.end(), payload.begin() + 12);
}
// Independent version-1 writer for one exact literal fixture. No engine encoder
// or private codec helper is used to construct its field order or primitive bytes.
std::vector<std::byte> literal_payload() {
    std::vector<std::byte> output;
    for (const char value : std::string{"SHKSCNE1"})
        output.push_back(static_cast<std::byte>(value));
    append(output, 1, 4);
    output.resize(44);
    append_id(output, id<SceneId>(3).value());
    append(output, 0, 1); // No world origin.
    append(output, 1, 4);
    append_id(output, id<AuthoredEntityId>(1).value());
    append(output, 0, 1); // No parent.
    append(output, 4, 4);
    for (const char value : std::string{"Root"})
        output.push_back(static_cast<std::byte>(value));
    for (const double value : {2.0, 3.0, 4.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0})
        append(output, std::bit_cast<std::uint64_t>(value), 8);
    append(output, 1, 1); // Camera present.
    append(output, 1, 4); // Orthographic.
    for (const double value : {1.0, 20.0, 0.0, 50.0})
        append(output, std::bit_cast<std::uint64_t>(value), 8);
    append(output, 1, 1); // Renderer present.
    append_id(output, id<AssetId>(0x13).value());
    append(output, 0, 4);
    append(output, 0, 1); // Invisible references are still resolved.
    REQUIRE(output.size() == 229);
    repair_digest(output);
    return output;
}
SceneEntityState state(LoadedPreparedScene& loaded, AuthoredEntityId entity) {
    return take(loaded.scene().entity_state(take(loaded.scene().find(entity))));
}
} // namespace

TEST_CASE("prepared scene roundtrip preserves IDs hierarchy anchor and component values",
          "[prepared-scene]") {
    const auto original = sample();
    const auto encoded = take(encode_prepared_scene(original));
    const auto decoded = take(decode_prepared_scene(encoded));
    CHECK(decoded == original);
    CHECK(take(encode_prepared_scene(decoded)) == encoded);
    auto assets = take(PreparedAssets::create());
    const auto loaded = take(LoadedPreparedScene::load(encoded, assets));
    CHECK(loaded->world_origin() == original.world_origin);
    CHECK(loaded->assets() == assets);
    CHECK(state(*loaded, id<AuthoredEntityId>(2)).world_matrix[3][0] == 13.0);
    CHECK(state(*loaded, id<AuthoredEntityId>(2)).local_transform.scale.x == -2.0);
    CHECK(state(*loaded, id<AuthoredEntityId>(2)).mesh_renderer ==
          MeshRenderer{builtin_plane_asset_id(), 0, false});
    CHECK(state(*loaded, id<AuthoredEntityId>(1)).camera == Camera{});
    CHECK(take(loaded->scene().stats()).live_entities == 2);
    CHECK(take(loaded->scene().stats()).revision == 2);
    auto noncanonical = original;
    noncanonical.entities[0].transform.rotation = math::Quat{2, 0, 0, 0};
    const auto normalized = take(decode_prepared_scene(take(encode_prepared_scene(noncanonical))));
    CHECK(normalized == original);
    CHECK(noncanonical.entities[0].transform.rotation.w == 2);
    const PreparedScene empty{id<SceneId>(9), {}, {}};
    CHECK(take(decode_prepared_scene(take(encode_prepared_scene(empty)))) == empty);
    CHECK(take(LoadedPreparedScene::load(take(encode_prepared_scene(empty)), assets))
              ->scene()
              .stats()
              .value()
              .live_entities == 0);
}

TEST_CASE("prepared scene reader accepts independent bytes and checks every optional field",
          "[prepared-scene]") {
    const auto payload = literal_payload();
    const auto decoded = take(decode_prepared_scene(payload));
    CHECK(decoded.scene == id<SceneId>(3));
    CHECK_FALSE(decoded.world_origin);
    REQUIRE(decoded.entities.size() == 1);
    const auto& entity = decoded.entities[0];
    CHECK(entity.id == id<AuthoredEntityId>(1));
    CHECK_FALSE(entity.parent);
    CHECK(entity.name.value == "Root");
    CHECK(entity.transform.translation == math::Vec3{2, 3, 4});
    CHECK(entity.transform.rotation == math::Quat{1, 0, 0, 0});
    REQUIRE(entity.camera);
    CHECK(entity.camera->projection == CameraProjection::orthographic);
    CHECK(entity.camera->vertical_fov_radians == 1.0);
    CHECK(entity.camera->orthographic_height == 20.0);
    CHECK(entity.camera->near_clip == 0.0);
    CHECK(entity.camera->far_clip == 50.0);
    CHECK(entity.mesh_renderer == MeshRenderer{builtin_cube_asset_id(), 0, false});
    CHECK(take(encode_prepared_scene(decoded)) == payload);
}

TEST_CASE("prepared scene decode rejects corrupt truncated and digest-valid malformed payloads",
          "[prepared-scene]") {
    const auto good = literal_payload();
    for (const auto offset : {0U, 8U, 12U, 228U}) {
        auto invalid = good;
        invalid[offset] ^= std::byte{0x7f};
        CHECK_FALSE(decode_prepared_scene(invalid));
    }
    for (const auto offset : {60U, 81U, 170U, 207U, 228U}) {
        auto invalid = good;
        invalid[offset] = std::byte{2};
        repair_digest(invalid);
        CHECK_FALSE(decode_prepared_scene(invalid));
    }
    for (const auto offset : {50U, 73U, 214U}) {
        auto invalid = good;
        invalid[offset] = std::byte{0};
        repair_digest(invalid);
        CHECK_FALSE(decode_prepared_scene(invalid));
    }
    for (const auto& [offset, value] : std::array<std::pair<std::size_t, std::uint32_t>, 3>{
             {{61, std::numeric_limits<std::uint32_t>::max()}, {82, 257}, {171, 2}}}) {
        auto invalid = good;
        replace(invalid, offset, value, 4);
        repair_digest(invalid);
        CHECK_FALSE(decode_prepared_scene(invalid));
    }
    for (const auto& [offset, value] : std::array<std::pair<std::size_t, double>, 5>{
             {{90, std::numeric_limits<double>::infinity()},
              {138, 0.0},
              {146, 0.0},
              {175, 0.0},
              {199, 0.0}}}) {
        auto invalid = good;
        replace(invalid, offset, std::bit_cast<std::uint64_t>(value), 8);
        repair_digest(invalid);
        CHECK_FALSE(decode_prepared_scene(invalid));
    }
    auto invalid = good;
    invalid[86] = std::byte{10};
    repair_digest(invalid);
    CHECK_FALSE(decode_prepared_scene(invalid));
    invalid = good;
    invalid.push_back(std::byte{0});
    repair_digest(invalid);
    CHECK_FALSE(decode_prepared_scene(invalid));
    invalid = good;
    invalid.pop_back();
    repair_digest(invalid);
    CHECK_FALSE(decode_prepared_scene(invalid));
    // A valid digest and individually valid IDs still cannot authorize a cycle,
    // a dangling parent or duplicate definitions.
    for (const auto parent : {id<AuthoredEntityId>(1), id<AuthoredEntityId>(99)}) {
        invalid = good;
        invalid[81] = std::byte{1};
        invalid.insert(invalid.begin() + 82, parent.value().bytes().begin(),
                       parent.value().bytes().end());
        repair_digest(invalid);
        CHECK_FALSE(decode_prepared_scene(invalid));
    }
    invalid = good;
    const std::vector<std::byte> entity_bytes{good.begin() + 65, good.end()};
    invalid.insert(invalid.end(), entity_bytes.begin(), entity_bytes.end());
    replace(invalid, 61, 2, 4);
    repair_digest(invalid);
    CHECK_FALSE(decode_prepared_scene(invalid));
    invalid = take(encode_prepared_scene(sample()));
    replace(invalid, 89, std::bit_cast<std::uint64_t>(-0.0), 8);
    repair_digest(invalid);
    CHECK_FALSE(decode_prepared_scene(invalid));
    CHECK_FALSE(decode_prepared_scene(std::span<const std::byte>{good}.first(43)));
    CHECK_FALSE(
        decode_prepared_scene(std::vector<std::byte>(prepared_scene_maximum_payload_bytes + 1)));
}

TEST_CASE("prepared scene encoding validates complete bounded hierarchy before returning bytes",
          "[prepared-scene]") {
    const auto valid = sample();
    auto invalid = valid;
    invalid.entities.push_back(invalid.entities[0]);
    CHECK_FALSE(encode_prepared_scene(invalid));
    invalid = valid;
    invalid.entities[0].parent = id<AuthoredEntityId>(99);
    CHECK_FALSE(encode_prepared_scene(invalid));
    invalid = valid;
    invalid.entities[0].parent = invalid.entities[0].id;
    CHECK_FALSE(encode_prepared_scene(invalid));
    invalid = valid;
    invalid.entities[1].parent = invalid.entities[0].id;
    CHECK_FALSE(encode_prepared_scene(invalid));
    invalid = valid;
    invalid.entities[0].name.value.assign(maximum_name_bytes + 1, 'x');
    CHECK_FALSE(encode_prepared_scene(invalid));
    invalid = valid;
    invalid.entities[0].transform.scale.x = 0;
    CHECK_FALSE(encode_prepared_scene(invalid));
    invalid = valid;
    invalid.entities[0].transform.translation.x = std::numeric_limits<double>::max();
    invalid.entities[1].transform.scale.x = 2;
    CHECK_FALSE(encode_prepared_scene(invalid));
    invalid = valid;
    invalid.entities[1].camera->near_clip = 0;
    CHECK_FALSE(encode_prepared_scene(invalid));
    invalid = valid;
    invalid.world_origin->offset[0] = -0.0;
    CHECK_FALSE(encode_prepared_scene(invalid));
    invalid = valid;
    invalid.world_origin->cell.x = world_cell_min - 1;
    CHECK_FALSE(encode_prepared_scene(invalid));

    PreparedScene chain{id<SceneId>(3), {}, {}};
    for (std::uint32_t index = 1; index <= prepared_scene_maximum_hierarchy_depth; ++index)
        chain.entities.push_back(
            {id<AuthoredEntityId>(index),
             index == 1 ? std::nullopt : std::optional{id<AuthoredEntityId>(index - 1)}});
    CHECK(encode_prepared_scene(chain));
    chain.entities.push_back({id<AuthoredEntityId>(257), id<AuthoredEntityId>(256)});
    CHECK_FALSE(encode_prepared_scene(chain));
    PreparedScene wide{id<SceneId>(3), {}, {}};
    for (std::uint32_t index = 1; index <= prepared_scene_maximum_entities; ++index)
        wide.entities.push_back({id<AuthoredEntityId>(index)});
    const auto encoded = take(encode_prepared_scene(wide));
    CHECK(take(decode_prepared_scene(encoded)).entities.size() == prepared_scene_maximum_entities);
    wide.entities.push_back({id<AuthoredEntityId>(4097)});
    CHECK_FALSE(encode_prepared_scene(wide));
}

TEST_CASE("prepared scene load rejects unresolved invisible meshes and preserves existing owners",
          "[prepared-scene]") {
    auto assets = take(PreparedAssets::create());
    const auto valid = sample();
    const auto payload = take(encode_prepared_scene(valid));
    auto loaded = take(LoadedPreparedScene::load(payload, assets));
    const auto original = state(*loaded, id<AuthoredEntityId>(2));
    const auto before = take(loaded->scene().stats());
    auto invalid = valid;
    invalid.entities[0].mesh_renderer->asset = id<AssetId>(0xff);
    const auto missing = LoadedPreparedScene::load(take(encode_prepared_scene(invalid)), assets);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().context.operation == "prepared.scene.load");
    CHECK(missing.error().context.entity_id == id<AuthoredEntityId>(2).value().to_string());
    invalid = valid;
    invalid.entities[0].mesh_renderer->mesh_index = 1;
    CHECK_FALSE(LoadedPreparedScene::load(take(encode_prepared_scene(invalid)), assets));
    CHECK_FALSE(LoadedPreparedScene::load(payload, nullptr));
    CHECK(take(loaded->scene().stats()).revision == before.revision);
    CHECK(take(loaded->scene().resolve(original.reference)) == original.reference.handle);

    auto replacement = take(LoadedPreparedScene::load(payload, assets));
    CHECK(take(replacement->scene().stats()).instance != before.instance);
    CHECK_FALSE(replacement->scene().resolve(original.reference));
    const auto lease = take(assets->resolve(*original.mesh_renderer));
    assets.reset();
    CHECK(loaded->assets()->size() == 2);
    REQUIRE(loaded->scene().destroy_entity(original.reference.handle));
    CHECK_FALSE(loaded->scene().resolve(original.reference));
    loaded.reset();
    replacement.reset();
    CHECK(lease->asset == builtin_plane_asset_id());
    CHECK(lease->meshes[0].primitives[0].indices.size() == 6);
}

TEST_CASE("loaded prepared scene retains Core owner-thread guards", "[prepared-scene]") {
    auto loaded =
        take(LoadedPreparedScene::load(literal_payload(), take(PreparedAssets::create())));
    bool rejected = false;
    std::thread other([&] { rejected = !loaded->scene().stats(); });
    other.join();
    CHECK(rejected);
    CHECK(take(loaded->scene().stats()).live_entities == 1);
}
