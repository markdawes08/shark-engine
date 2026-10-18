#include <shark/client/cube_sample.hpp>
#include <shark/client/shader_bundle.hpp>
#include <shark/stable_id.hpp>

#include "../engine/client/src/shader_digest.hpp"
#include "gpu_shader_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <string_view>

namespace {
using namespace shark::client;
using namespace shark::test::shaders;

void write_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::uint32_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

std::vector<std::byte> shader_bytes(std::span<const std::uint32_t> words) {
    std::vector<std::byte> bytes(words.size() * 4);
    for (std::size_t index = 0; index < words.size(); ++index) {
        write_u32(bytes, index * 4, words[index]);
    }
    return bytes;
}

std::vector<std::byte> bundle_bytes(bool swapped = false) {
    const auto vertex = shader_bytes(swapped ? std::span<const std::uint32_t>{ps_words}
                                             : std::span<const std::uint32_t>{vs_words});
    const auto pixel = shader_bytes(swapped ? std::span<const std::uint32_t>{vs_words}
                                            : std::span<const std::uint32_t>{ps_words});
    std::vector<std::byte> bytes(88);
    constexpr std::string_view magic{"SHRKSHD\0", 8};
    for (std::size_t index = 0; index < magic.size(); ++index) {
        bytes[index] = static_cast<std::byte>(magic[index]);
    }
    write_u32(bytes, 8, 1);
    write_u32(bytes, 12, static_cast<std::uint32_t>(vertex.size()));
    write_u32(bytes, 16, static_cast<std::uint32_t>(pixel.size()));
    const auto vertex_hash = detail::shader_sha256(vertex);
    const auto pixel_hash = detail::shader_sha256(pixel);
    std::copy(vertex_hash.begin(), vertex_hash.end(), bytes.begin() + 24);
    std::copy(pixel_hash.begin(), pixel_hash.end(), bytes.begin() + 56);
    bytes.insert(bytes.end(), vertex.begin(), vertex.end());
    bytes.insert(bytes.end(), pixel.begin(), pixel.end());
    return bytes;
}

std::string digest(std::string_view value) {
    return detail::shader_digest_text(detail::shader_sha256(std::as_bytes(std::span{value})));
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        auto id = shark::generate_stable_id();
        REQUIRE(id);
        path = std::filesystem::temp_directory_path() / ("shark-bundle-" + id.value().to_string());
        REQUIRE(std::filesystem::create_directory(path));
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
} // namespace

TEST_CASE("shader SHA-256 matches independent short padding and long vectors", "[shader-bundle]") {
    CHECK(digest("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(digest("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(digest(std::string(1000000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("versioned shader bundle decodes independent offline payload identities",
          "[shader-bundle]") {
    const auto bytes = bundle_bytes();
    auto decoded = decode_shader_bundle(bytes);
    REQUIRE(decoded);
    CHECK(decoded.value().sha256 ==
          "a8d21d517000d305f00b27bb9f1eb402e284f68e8c4bf6d41c5e3e9b5a6d6fa2");
    CHECK(decoded.value().vertex_shader.words == std::vector(vs_words.begin(), vs_words.end()));
    CHECK(decoded.value().pixel_shader.words == std::vector(ps_words.begin(), ps_words.end()));
    CHECK(decoded.value().vertex_shader.stage == ShaderStage::vertex);
    CHECK(decoded.value().pixel_shader.stage == ShaderStage::pixel);
}

TEST_CASE("bundle framing hashes and typed interfaces fail independently", "[shader-bundle]") {
    const auto original = bundle_bytes();
    for (const auto offset : {0U, 7U, 8U, 12U, 16U, 20U, 24U, 56U}) {
        CAPTURE(offset);
        auto bytes = original;
        bytes[offset] ^= std::byte{1};
        REQUIRE_FALSE(decode_shader_bundle(bytes));
    }
    for (const auto length : {0U, 19U, 87U, 88U, 1931U}) {
        REQUIRE_FALSE(decode_shader_bundle(std::span{original}.first(length)));
    }
    auto trailing = original;
    trailing.push_back(std::byte{0});
    REQUIRE_FALSE(decode_shader_bundle(trailing));
    for (const auto size : {0U, 19U, 21U, 4194308U, 0xffffffffU}) {
        auto bytes = original;
        write_u32(bytes, 12, size);
        REQUIRE_FALSE(decode_shader_bundle(bytes));
    }
    // SPIR-V generator identity is interface-neutral. Its changed byte still
    // passes the typed reader, but distribution integrity must reject it.
    auto corrupt = original;
    corrupt[88 + 8] ^= std::byte{1};
    REQUIRE(decode_shader_artifact(std::span{corrupt}.subspan(88, vs_words.size() * 4),
                                   ShaderStage::vertex));
    auto rejected = decode_shader_bundle(corrupt);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().context.operation == "shader.bundle.decode");
    CHECK(rejected.error().message.find("SHA-256") != std::string::npos);
    // Correct hashes do not make a swapped stage/layout valid.
    REQUIRE_FALSE(decode_shader_bundle(bundle_bytes(true)));
}

TEST_CASE("shader bundles load bounded Unicode files with owned path errors", "[shader-bundle]") {
    TemporaryDirectory directory;
    const auto path = directory.path / std::filesystem::path{u8"shader \u96ea.sharkshader"};
    const auto bytes = bundle_bytes();
    {
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        REQUIRE(stream.good());
    }
    auto loaded = load_shader_bundle(path);
    REQUIRE(loaded);
    CHECK(loaded.value().sha256 ==
          "a8d21d517000d305f00b27bb9f1eb402e284f68e8c4bf6d41c5e3e9b5a6d6fa2");
    REQUIRE(std::filesystem::remove(path));
    auto missing = load_shader_bundle(path);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == shark::ErrorCode::io_failure);
    CHECK(missing.error().context.operation == "shader.bundle.load");
    const auto text = path.u8string();
    CHECK(missing.error().context.path == std::string(text.begin(), text.end()));
    REQUIRE_FALSE(load_shader_bundle(directory.path));
    REQUIRE_FALSE(load_shader_bundle({}));
    const std::filesystem::path nul{std::filesystem::path::string_type{
        std::filesystem::path::value_type{'a'}, {}, std::filesystem::path::value_type{'b'}}};
    auto bad_path = load_shader_bundle(nul);
    REQUIRE_FALSE(bad_path);
    CHECK(bad_path.error().code == shark::ErrorCode::invalid_path);
    const auto default_path = default_shader_bundle_path();
    REQUIRE(default_path);
    CHECK(default_path.value().is_absolute());
    CHECK(default_path.value().filename() == "cube.sharkshader");
    CHECK(default_path.value().parent_path().filename() == "assets");
}

TEST_CASE("shared cube requests retain depth fixture and reject invalid shaders",
          "[shader-bundle]") {
    auto bundle = decode_shader_bundle(bundle_bytes());
    REQUIRE(bundle);
    auto request = make_cube_sample(bundle.value(), {96, 80});
    REQUIRE(request);
    CHECK(request.value().vertices.size() == 48);
    CHECK(request.value().indices.size() == 72);
    REQUIRE(validate_offscreen_request(request.value()));
    auto alternate = make_cube_sample(bundle.value(), {128, 128}, true);
    REQUIRE(alternate);
    CHECK(alternate.value().indices != request.value().indices);
    CHECK(alternate.value().texture_rgba8 != request.value().texture_rgba8);
    REQUIRE_FALSE(make_cube_sample(bundle.value(), {0, 128}));
    bundle.value().vertex_shader = bundle.value().pixel_shader;
    REQUIRE_FALSE(make_cube_sample(bundle.value()));
}
