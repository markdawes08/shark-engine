#include <shark/content_digest.hpp>
#include <shark/prepared_assets.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <thread>

namespace {
using namespace shark;
template <typename T> T take(Result<T> result) {
    if (!result)
        INFO(result.error().message);
    REQUIRE(result);
    return std::move(result).value();
}
AssetId id(unsigned value = 1) {
    auto text = std::string{"30000000-0000-4000-8000-000000000000"};
    constexpr std::string_view digits = "0123456789abcdef";
    for (std::size_t i = 0; i < 8; ++i) {
        text[text.size() - 1 - i] = digits[value & 15U];
        value >>= 4U;
    }
    return AssetId{take(StableId::parse(text))};
}
PreparedMeshAsset triangle() {
    PreparedVertex a, b, c;
    a.position = {0, 0, 0};
    b.position = {1, 0, 0};
    c.position = {0, 1, 0};
    a.normal = b.normal = c.normal = {0, 0, 1};
    b.texcoord0 = {1, 0};
    c.texcoord0 = {0, 1};
    PreparedPrimitive primitive{{a, b, c}, {0, 1, 2}, {{0.25F, 0.5F, 0.75F, 1.0F}, true}};
    PreparedMesh mesh{{std::move(primitive)}};
    return {id(), {std::move(mesh)}};
}
void patch32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    REQUIRE(offset + 4 <= bytes.size());
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 255U);
}
void reseal(std::vector<std::byte>& bytes) {
    REQUIRE(bytes.size() >= 44);
    const auto digest = take(sha256(std::span<const std::byte>{bytes}.subspan(44)));
    std::copy(digest.bytes.begin(), digest.bytes.end(), bytes.begin() + 12);
}
std::array<float, 3> subtract(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
std::array<float, 3> cross(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
void winding(const PreparedPrimitive& primitive) {
    for (std::size_t i = 0; i < primitive.indices.size(); i += 3) {
        const auto& a = primitive.vertices[primitive.indices[i]];
        const auto& b = primitive.vertices[primitive.indices[i + 1]];
        const auto& c = primitive.vertices[primitive.indices[i + 2]];
        const auto normal =
            cross(subtract(b.position, a.position), subtract(c.position, a.position));
        CHECK(normal[0] * a.normal[0] + normal[1] * a.normal[1] + normal[2] * a.normal[2] > 0.0F);
        CHECK(a.normal == b.normal);
        CHECK(a.normal == c.normal);
    }
}
} // namespace

TEST_CASE("Prepared mesh roundtrip has deterministic explicit little endian geometry",
          "[prepared-assets]") {
    const auto source = triangle();
    const auto encoded = take(encode_prepared_mesh(source));
    REQUIRE(encoded.size() == 321);
    CHECK(std::string{reinterpret_cast<const char*>(encoded.data()), 8} == "SHKMESH1");
    CHECK(encoded[8] == std::byte{1});
    CHECK(encoded[9] == std::byte{0});
    CHECK(encoded[10] == std::byte{0});
    CHECK(encoded[11] == std::byte{0});
    CHECK(encoded[68] == std::byte{0});
    CHECK(encoded[69] == std::byte{0});
    CHECK(encoded[70] == std::byte{128});
    CHECK(encoded[71] == std::byte{62}); // independently known float32 0.25 LE
    const auto decoded = take(decode_prepared_mesh(encoded));
    CHECK(decoded == source);
    CHECK(take(encode_prepared_mesh(decoded)) == encoded);
    auto multi = source;
    multi.meshes.push_back(source.meshes.front());
    multi.meshes.front().primitives.push_back(source.meshes.front().primitives.front());
    CHECK(take(decode_prepared_mesh(take(encode_prepared_mesh(multi)))) == multi);
    // Degenerate topology is retained, not silently repaired. It may reference
    // fewer than three distinct vertices and still obey the triangle format.
    auto degenerate = source;
    degenerate.meshes[0].primitives[0].vertices.resize(1);
    degenerate.meshes[0].primitives[0].indices = {0, 0, 0};
    CHECK(take(decode_prepared_mesh(take(encode_prepared_mesh(degenerate)))) == degenerate);
}

TEST_CASE("Prepared mesh rejects corrupted envelopes and resealed malformed contents",
          "[prepared-assets]") {
    const auto valid = take(encode_prepared_mesh(triangle()));
    for (std::size_t length = 0; length < valid.size(); ++length)
        CHECK_FALSE(decode_prepared_mesh(std::span<const std::byte>{valid}.first(length)));
    auto corrupt = valid;
    corrupt.back() ^= std::byte{1};
    CHECK_FALSE(decode_prepared_mesh(corrupt));
    corrupt = valid;
    corrupt[0] = std::byte{0};
    CHECK_FALSE(decode_prepared_mesh(corrupt));
    corrupt = valid;
    patch32(corrupt, 8, 2);
    CHECK_FALSE(decode_prepared_mesh(corrupt));
    for (const auto& [offset, value] : std::array<std::pair<std::size_t, std::uint32_t>, 8>{
             {{60, std::numeric_limits<std::uint32_t>::max()},  // mesh count
              {64, std::numeric_limits<std::uint32_t>::max()},  // primitive count
              {85, std::numeric_limits<std::uint32_t>::max()},  // vertex count
              {305, std::numeric_limits<std::uint32_t>::max()}, // index count
              {309, 3},                                         // index outside three vertices
              {89, 0x7fc00000U},                                // NaN position
              {109, 0},                                         // zero normal
              {145, 0x40000000U}}}) {                           // color outside [0,1]
        auto malformed = valid;
        patch32(malformed, offset, value);
        reseal(malformed);
        CHECK_FALSE(decode_prepared_mesh(malformed));
    }
    auto invalid_flag = valid;
    invalid_flag[84] = std::byte{2};
    reseal(invalid_flag);
    CHECK_FALSE(decode_prepared_mesh(invalid_flag));
    auto invalid_identity = valid;
    invalid_identity[50] = std::byte{0}; // UUID version nibble
    reseal(invalid_identity);
    CHECK_FALSE(decode_prepared_mesh(invalid_identity));
    auto trailing = valid;
    trailing.push_back(std::byte{0});
    reseal(trailing);
    CHECK_FALSE(decode_prepared_mesh(trailing));
    std::vector<std::byte> oversized(prepared_mesh_maximum_payload_bytes + 1);
    CHECK_FALSE(decode_prepared_mesh(oversized));
}

TEST_CASE("Prepared mesh typed validation rejects unusable data and bounded output",
          "[prepared-assets]") {
    const auto source = triangle();
    auto bad = source;
    bad.meshes.clear();
    CHECK_FALSE(encode_prepared_mesh(bad));
    bad = source;
    bad.meshes[0].primitives.clear();
    CHECK_FALSE(encode_prepared_mesh(bad));
    bad = source;
    bad.meshes[0].primitives[0].indices = {0, 1};
    CHECK_FALSE(encode_prepared_mesh(bad));
    bad = source;
    bad.meshes[0].primitives[0].vertices[0].texcoord1[0] = std::numeric_limits<float>::infinity();
    CHECK_FALSE(encode_prepared_mesh(bad));
    bad = source;
    bad.meshes[0].primitives[0].vertices[0].tangent[3] = 0;
    CHECK_FALSE(encode_prepared_mesh(bad));
    bad = source;
    bad.meshes[0].primitives[0].material.base_color[3] = -0.1F;
    CHECK_FALSE(encode_prepared_mesh(bad));
    bad = source;
    bad.meshes[0].primitives[0].vertices.resize(30000, source.meshes[0].primitives[0].vertices[0]);
    CHECK_FALSE(encode_prepared_mesh(bad)); // valid values, encoded output exceeds 2 MiB
    bad = source;
    bad.meshes.resize(prepared_mesh_maximum_meshes + 1);
    CHECK_FALSE(encode_prepared_mesh(bad));
}

TEST_CASE("Built-in cube and plane have stable IDs unit bounds flat normals and CCW triangles",
          "[prepared-assets]") {
    const auto assets = take(PreparedAssets::create());
    REQUIRE(assets->size() == 2);
    CHECK(builtin_cube_asset_id().value().to_string() == "00000000-0000-4000-8000-000000000013");
    CHECK(builtin_plane_asset_id().value().to_string() == "00000000-0000-4000-8000-000000000014");
    const auto cube = take(assets->find(builtin_cube_asset_id()));
    REQUIRE(cube->meshes.size() == 1);
    REQUIRE(cube->meshes[0].primitives.size() == 1);
    const auto& cube_primitive = cube->meshes[0].primitives[0];
    REQUIRE(cube_primitive.vertices.size() == 24);
    REQUIRE(cube_primitive.indices.size() == 36);
    std::array<float, 3> minimum{1, 1, 1}, maximum{-1, -1, -1};
    for (const auto& vertex : cube_primitive.vertices) {
        for (std::size_t i = 0; i < 3; ++i) {
            minimum[i] = std::min(minimum[i], vertex.position[i]);
            maximum[i] = std::max(maximum[i], vertex.position[i]);
        }
        CHECK((vertex.texcoord0[0] == 0 || vertex.texcoord0[0] == 1));
        CHECK((vertex.texcoord0[1] == 0 || vertex.texcoord0[1] == 1));
    }
    CHECK(minimum == std::array{-0.5F, -0.5F, -0.5F});
    CHECK(maximum == std::array{0.5F, 0.5F, 0.5F});
    winding(cube_primitive);
    const auto plane = take(assets->find(builtin_plane_asset_id()));
    REQUIRE(plane->meshes.size() == 1);
    REQUIRE(plane->meshes[0].primitives.size() == 1);
    const auto& plane_primitive = plane->meshes[0].primitives[0];
    REQUIRE(plane_primitive.vertices.size() == 4);
    CHECK(plane_primitive.indices == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3});
    CHECK(plane_primitive.vertices[0].position == std::array{-0.5F, 0.0F, 0.5F});
    CHECK(plane_primitive.vertices[2].position == std::array{0.5F, 0.0F, -0.5F});
    for (const auto& vertex : plane_primitive.vertices)
        CHECK(vertex.normal == std::array{0.0F, 1.0F, 0.0F});
    winding(plane_primitive);
    CHECK(take(decode_prepared_mesh(take(encode_prepared_mesh(*cube)))) == *cube);
    CHECK(take(decode_prepared_mesh(take(encode_prepared_mesh(*plane)))) == *plane);
}

TEST_CASE(
    "Prepared snapshots share immutable leases and failed replacements preserve existing content",
    "[prepared-assets]") {
    auto payload = take(encode_prepared_mesh(triangle()));
    const std::array input{PreparedAssetInput{id(), payload}};
    auto current = take(PreparedAssets::create(input));
    REQUIRE(current->size() == 3);
    const auto retained = current->retained_bytes();
    CHECK(retained > payload.size());
    CHECK(retained <= prepared_assets_maximum_bytes);
    const auto lease = take(current->resolve({id(), 0, true}));
    CHECK(take(current->resolve({id(), 0, false})) == lease);
    CHECK(take(current->find(id())) == lease);
    CHECK_FALSE(current->resolve({id(), 1, false}));
    const auto missing = current->find(id(99));
    REQUIRE_FALSE(missing);
    CHECK(missing.error().context.asset_id == id(99).value().to_string());
    CHECK(missing.error().context.operation == "prepared.assets.find");
    payload.back() ^= std::byte{1};
    CHECK_FALSE(PreparedAssets::create(input));
    CHECK(take(current->find(id())) == lease);
    CHECK(current->retained_bytes() == retained);
    auto replacement = triangle();
    replacement.meshes[0].primitives[0].material.base_color = {1, 0, 0, 1};
    auto replacement_bytes = take(encode_prepared_mesh(replacement));
    const std::array replacement_input{PreparedAssetInput{id(), replacement_bytes}};
    current = take(PreparedAssets::create(replacement_input));
    const auto fresh = take(current->find(id()));
    CHECK(fresh != lease);
    CHECK(fresh->meshes[0].primitives[0].material.base_color == std::array{1.0F, 0.0F, 0.0F, 1.0F});
    CHECK(lease->meshes[0].primitives[0].material.base_color ==
          std::array{0.25F, 0.5F, 0.75F, 1.0F});
    std::fill(replacement_bytes.begin(), replacement_bytes.end(), std::byte{0});
    current.reset();
    CHECK(fresh->asset == id());
    CHECK(fresh->meshes[0].primitives[0].vertices[1].position == std::array{1.0F, 0.0F, 0.0F});
    CHECK(lease->meshes[0].primitives[0].indices == std::vector<std::uint32_t>{0, 1, 2});
}

TEST_CASE("Prepared snapshots reject ambiguous reserved mismatched and oversized inputs",
          "[prepared-assets]") {
    const auto payload = take(encode_prepared_mesh(triangle()));
    const std::array duplicate{PreparedAssetInput{id(), payload},
                               PreparedAssetInput{id(), payload}};
    CHECK_FALSE(PreparedAssets::create(duplicate));
    const std::array reserved{PreparedAssetInput{builtin_cube_asset_id(), payload}};
    CHECK_FALSE(PreparedAssets::create(reserved));
    const std::array mismatched{PreparedAssetInput{id(2), payload}};
    CHECK_FALSE(PreparedAssets::create(mismatched));
    std::vector<PreparedAssetInput> excessive(prepared_assets_maximum_count - 1, {id(), payload});
    CHECK_FALSE(PreparedAssets::create(excessive));
    std::vector<std::byte> full(prepared_mesh_maximum_payload_bytes);
    std::vector<PreparedAssetInput> total;
    for (unsigned i = 1; i <= 17; ++i)
        total.push_back({id(i), full});
    const auto too_large = PreparedAssets::create(total);
    REQUIRE_FALSE(too_large);
    CHECK(too_large.error().message == "Prepared asset snapshot exceeds its input byte bound");
    const auto shared = take(PreparedAssets::create());
    std::shared_ptr<const PreparedMeshAsset> from_worker;
    std::thread worker([&] { from_worker = shared->find(builtin_plane_asset_id()).value(); });
    worker.join();
    CHECK(from_worker == take(shared->find(builtin_plane_asset_id())));
}
