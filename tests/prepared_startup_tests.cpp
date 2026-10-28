#include <shark/prepared_startup.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace {
using namespace shark;
template <typename T> T take(Result<T> result) {
    if (!result)
        INFO(result.error().message);
    REQUIRE(result);
    return std::move(result).value();
}
template <typename Id> Id id(char digit) {
    std::string value = "10000000-0000-4000-8000-000000000000";
    value.back() = digit;
    return Id{take(StableId::parse(value))};
}
PreparedStartup startup() {
    const auto digest = take(
        ContentDigest::parse("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    return {id<StableId>('1'), id<WorldId>('2'),
            id<SceneId>('3'),  id<AuthoredEntityId>('4'),
            Name{"Startup"},   "windows-x86_64",
            {80, digest},      {20, digest},
            {24, digest},      {{id<AssetId>('5'), {100, digest}}}};
}
void u32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes.push_back(static_cast<std::byte>((value >> (i * 8U)) & 255U));
}
void raw(std::vector<std::byte>& bytes, std::span<const std::byte> value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
}
void text(std::vector<std::byte>& bytes, std::string_view value) {
    u32(bytes, static_cast<std::uint32_t>(value.size()));
    raw(bytes, std::as_bytes(std::span{value.data(), value.size()}));
}
void file(std::vector<std::byte>& bytes, const PreparedFileIdentity& value) {
    u32(bytes, value.bytes);
    raw(bytes, value.digest.bytes);
}
// Independent byte construction fixes field order, explicit widths and version.
std::vector<std::byte> independent(const PreparedStartup& value) {
    std::vector<std::byte> body;
    raw(body, value.project.bytes());
    raw(body, value.world.value().bytes());
    raw(body, value.scene.value().bytes());
    raw(body, value.camera.value().bytes());
    text(body, value.name.value);
    text(body, value.target);
    file(body, value.scene_file);
    file(body, value.vertex_shader);
    file(body, value.pixel_shader);
    u32(body, static_cast<std::uint32_t>(value.meshes.size()));
    for (const auto& mesh : value.meshes) {
        raw(body, mesh.asset.value().bytes());
        file(body, mesh.file);
    }
    std::vector<std::byte> result;
    constexpr std::string_view magic = "SHKSTRT1";
    raw(result, std::as_bytes(std::span{magic.data(), magic.size()}));
    u32(result, 1);
    raw(result, take(sha256(body)).bytes);
    raw(result, body);
    return result;
}
} // namespace

TEST_CASE("Startup codec matches independent inventory bytes and roundtrips identities",
          "[prepared-startup]") {
    const auto value = startup();
    const auto bytes = independent(value);
    CHECK(take(encode_prepared_startup(value)) == bytes);
    CHECK(take(decode_prepared_startup(bytes)) == value);
    CHECK(prepared_startup_mesh_path(value.meshes[0].asset) ==
          "content/10000000-0000-4000-8000-000000000005.sharkmesh");
    CHECK(prepared_startup_scene_path == "content/startup.sharkscene");
    CHECK(prepared_startup_vertex_path == "assets/scene-vs.spv");
    CHECK(prepared_startup_pixel_path == "assets/scene-ps.spv");
}

TEST_CASE("Startup codec rejects corrupt framing and unsupported versions", "[prepared-startup]") {
    const auto bytes = independent(startup());
    for (const auto offset : {0U, 8U, 12U, 44U, 80U}) {
        auto broken = bytes;
        broken[offset] ^= std::byte{1};
        CHECK_FALSE(decode_prepared_startup(broken));
    }
    for (const auto size : {0U, 7U, 43U, 44U, 120U})
        CHECK_FALSE(decode_prepared_startup(std::span{bytes}.first(size)));
    auto trailing = bytes;
    trailing.push_back(std::byte{0});
    CHECK_FALSE(decode_prepared_startup(trailing));
    auto oversized = bytes;
    oversized.resize(prepared_startup_maximum_bytes + 1);
    CHECK_FALSE(decode_prepared_startup(oversized));
}

TEST_CASE("Startup codec bounds names targets file sizes identities and combined content",
          "[prepared-startup]") {
    auto invalid = [](const PreparedStartup& value) {
        CHECK_FALSE(encode_prepared_startup(value));
        CHECK_FALSE(decode_prepared_startup(independent(value)));
    };
    auto value = startup();
    value.name.value = std::string(257, 'a');
    invalid(value);
    value = startup();
    value.name.value = "bad\nname";
    invalid(value);
    value = startup();
    value.target = "linux-x86_64";
    invalid(value);
    for (const auto size : {0U, 43U, 4194305U}) {
        value = startup();
        value.scene_file.bytes = size;
        invalid(value);
    }
    for (const auto size : {0U, 19U, 21U, 4194308U}) {
        value = startup();
        value.vertex_shader.bytes = size;
        invalid(value);
    }
    value = startup();
    value.meshes.push_back(value.meshes[0]);
    invalid(value);
    value = startup();
    value.meshes[0].asset = builtin_cube_asset_id();
    invalid(value);
    value = startup();
    value.meshes[0].file.bytes = 2097153;
    invalid(value);
    value = startup();
    value.meshes.resize(127, value.meshes[0]);
    invalid(value);
    value = startup();
    value.scene_file.bytes = static_cast<std::uint32_t>(prepared_scene_maximum_payload_bytes);
    value.meshes.clear();
    for (unsigned index = 1; index <= 15; ++index) {
        std::string encoded = "20000000-0000-4000-8000-000000000000";
        encoded.back() = "0123456789abcdef"[index];
        value.meshes.push_back(
            {AssetId{take(StableId::parse(encoded))},
             {static_cast<std::uint32_t>(prepared_mesh_maximum_payload_bytes), {}}});
    }
    invalid(value);
}
