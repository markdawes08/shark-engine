#include <shark/client/gpu_resources.hpp>
#include <shark/stable_id.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace shark::client;

// Engine-owned shaders/textured_cube.hlsl, compiled by pinned DXC 1.9.2609.5
// with the 06a shark-vulkan-1 recipe and independently validated by spirv-val.
// Literal words keep these interface/parser tests CPU-only and compiler-free.
// They are executable shader fixtures, not invented reflection-only modules.
// vs SPIR-V SHA-256: e07a66c210b02f52ddf9c72ee441635b1081cc80febc37fa8d739f2944007ace
constexpr std::array<std::uint32_t, 266> vs_words{
    0x07230203U, 0x00010600U, 0x000e0000U, 0x00000025U, 0x00000000U, 0x00020011U, 0x00000001U,
    0x0003000eU, 0x00000000U, 0x00000001U, 0x000a000fU, 0x00000000U, 0x00000001U, 0x614d7376U,
    0x00006e69U, 0x00000002U, 0x00000003U, 0x00000004U, 0x00000005U, 0x00000006U, 0x00030003U,
    0x00000005U, 0x00000258U, 0x00090005U, 0x00000007U, 0x65707974U, 0x6e6f432eU, 0x6e617473U,
    0x66754274U, 0x2e726566U, 0x626f6c47U, 0x00736c61U, 0x00080006U, 0x00000007U, 0x00000000U,
    0x6c726f77U, 0x65695664U, 0x6f725077U, 0x7463656aU, 0x006e6f69U, 0x00040005U, 0x00000006U,
    0x626f6c67U, 0x00736c61U, 0x00060005U, 0x00000002U, 0x762e6e69U, 0x502e7261U, 0x5449534fU,
    0x004e4f49U, 0x00070005U, 0x00000003U, 0x762e6e69U, 0x542e7261U, 0x4f435845U, 0x3044524fU,
    0x00000000U, 0x00070005U, 0x00000005U, 0x2e74756fU, 0x2e726176U, 0x43584554U, 0x44524f4fU,
    0x00000030U, 0x00040005U, 0x00000001U, 0x614d7376U, 0x00006e69U, 0x00040047U, 0x00000004U,
    0x0000000bU, 0x00000000U, 0x00040047U, 0x00000002U, 0x0000001eU, 0x00000000U, 0x00040047U,
    0x00000003U, 0x0000001eU, 0x00000001U, 0x00040047U, 0x00000005U, 0x0000001eU, 0x00000000U,
    0x00040047U, 0x00000006U, 0x00000022U, 0x00000000U, 0x00040047U, 0x00000006U, 0x00000021U,
    0x00000100U, 0x00050048U, 0x00000007U, 0x00000000U, 0x00000023U, 0x00000000U, 0x00050048U,
    0x00000007U, 0x00000000U, 0x00000007U, 0x00000010U, 0x00040048U, 0x00000007U, 0x00000000U,
    0x00000004U, 0x00030047U, 0x00000007U, 0x00000002U, 0x00040015U, 0x00000008U, 0x00000020U,
    0x00000001U, 0x0004002bU, 0x00000008U, 0x00000009U, 0x00000000U, 0x00030016U, 0x0000000aU,
    0x00000020U, 0x0004002bU, 0x0000000aU, 0x0000000bU, 0x3f800000U, 0x00040017U, 0x0000000cU,
    0x0000000aU, 0x00000004U, 0x00040018U, 0x0000000dU, 0x0000000cU, 0x00000004U, 0x0003001eU,
    0x00000007U, 0x0000000dU, 0x00040020U, 0x0000000eU, 0x00000002U, 0x00000007U, 0x00040017U,
    0x0000000fU, 0x0000000aU, 0x00000003U, 0x00040020U, 0x00000010U, 0x00000001U, 0x0000000fU,
    0x00040017U, 0x00000011U, 0x0000000aU, 0x00000002U, 0x00040020U, 0x00000012U, 0x00000001U,
    0x00000011U, 0x00040020U, 0x00000013U, 0x00000003U, 0x0000000cU, 0x00040020U, 0x00000014U,
    0x00000003U, 0x00000011U, 0x00020013U, 0x00000015U, 0x00030021U, 0x00000016U, 0x00000015U,
    0x00040020U, 0x00000017U, 0x00000002U, 0x0000000dU, 0x0004003bU, 0x0000000eU, 0x00000006U,
    0x00000002U, 0x0004003bU, 0x00000010U, 0x00000002U, 0x00000001U, 0x0004003bU, 0x00000012U,
    0x00000003U, 0x00000001U, 0x0004003bU, 0x00000013U, 0x00000004U, 0x00000003U, 0x0004003bU,
    0x00000014U, 0x00000005U, 0x00000003U, 0x00050036U, 0x00000015U, 0x00000001U, 0x00000000U,
    0x00000016U, 0x000200f8U, 0x00000018U, 0x0004003dU, 0x0000000fU, 0x00000019U, 0x00000002U,
    0x0004003dU, 0x00000011U, 0x0000001aU, 0x00000003U, 0x00050041U, 0x00000017U, 0x0000001bU,
    0x00000006U, 0x00000009U, 0x0004003dU, 0x0000000dU, 0x0000001cU, 0x0000001bU, 0x00050051U,
    0x0000000aU, 0x0000001dU, 0x00000019U, 0x00000000U, 0x00050051U, 0x0000000aU, 0x0000001eU,
    0x00000019U, 0x00000001U, 0x00050051U, 0x0000000aU, 0x0000001fU, 0x00000019U, 0x00000002U,
    0x00070050U, 0x0000000cU, 0x00000020U, 0x0000001dU, 0x0000001eU, 0x0000001fU, 0x0000000bU,
    0x00050090U, 0x0000000cU, 0x00000021U, 0x00000020U, 0x0000001cU, 0x00050051U, 0x0000000aU,
    0x00000022U, 0x00000021U, 0x00000001U, 0x0004007fU, 0x0000000aU, 0x00000023U, 0x00000022U,
    0x00060052U, 0x0000000cU, 0x00000024U, 0x00000023U, 0x00000021U, 0x00000001U, 0x0003003eU,
    0x00000004U, 0x00000024U, 0x0003003eU, 0x00000005U, 0x0000001aU, 0x000100fdU, 0x00010038U,
};

// ps SPIR-V SHA-256: 9a08883c89af45f1b6fc3975c2146311eb2051d8b2ad36a705a51405dd212406
constexpr std::array<std::uint32_t, 195> ps_words{
    0x07230203U, 0x00010600U, 0x000e0000U, 0x00000018U, 0x00000000U, 0x00020011U, 0x00000001U,
    0x0003000eU, 0x00000000U, 0x00000001U, 0x0009000fU, 0x00000004U, 0x00000001U, 0x614d7370U,
    0x00006e69U, 0x00000002U, 0x00000003U, 0x00000004U, 0x00000005U, 0x00030010U, 0x00000001U,
    0x00000007U, 0x00030003U, 0x00000005U, 0x00000258U, 0x00060005U, 0x00000006U, 0x65707974U,
    0x2e64322eU, 0x67616d69U, 0x00000065U, 0x00060005U, 0x00000004U, 0x6f6c6f63U, 0x78655472U,
    0x65727574U, 0x00000000U, 0x00060005U, 0x00000007U, 0x65707974U, 0x6d61732eU, 0x72656c70U,
    0x00000000U, 0x00060005U, 0x00000005U, 0x6f6c6f63U, 0x6d615372U, 0x72656c70U, 0x00000000U,
    0x00070005U, 0x00000002U, 0x762e6e69U, 0x542e7261U, 0x4f435845U, 0x3044524fU, 0x00000000U,
    0x00070005U, 0x00000003U, 0x2e74756fU, 0x2e726176U, 0x545f5653U, 0x65677261U, 0x00000074U,
    0x00040005U, 0x00000001U, 0x614d7370U, 0x00006e69U, 0x00070005U, 0x00000008U, 0x65707974U,
    0x6d61732eU, 0x64656c70U, 0x616d692eU, 0x00006567U, 0x00040047U, 0x00000002U, 0x0000001eU,
    0x00000000U, 0x00040047U, 0x00000003U, 0x0000001eU, 0x00000000U, 0x00040047U, 0x00000004U,
    0x00000022U, 0x00000000U, 0x00040047U, 0x00000004U, 0x00000021U, 0x00000000U, 0x00040047U,
    0x00000005U, 0x00000022U, 0x00000000U, 0x00040047U, 0x00000005U, 0x00000021U, 0x00000080U,
    0x00030016U, 0x00000009U, 0x00000020U, 0x00040017U, 0x0000000aU, 0x00000009U, 0x00000004U,
    0x00090019U, 0x00000006U, 0x00000009U, 0x00000001U, 0x00000002U, 0x00000000U, 0x00000000U,
    0x00000001U, 0x00000000U, 0x00040020U, 0x0000000bU, 0x00000000U, 0x00000006U, 0x0002001aU,
    0x00000007U, 0x00040020U, 0x0000000cU, 0x00000000U, 0x00000007U, 0x00040017U, 0x0000000dU,
    0x00000009U, 0x00000002U, 0x00040020U, 0x0000000eU, 0x00000001U, 0x0000000dU, 0x00040020U,
    0x0000000fU, 0x00000003U, 0x0000000aU, 0x00020013U, 0x00000010U, 0x00030021U, 0x00000011U,
    0x00000010U, 0x0003001bU, 0x00000008U, 0x00000006U, 0x0004003bU, 0x0000000bU, 0x00000004U,
    0x00000000U, 0x0004003bU, 0x0000000cU, 0x00000005U, 0x00000000U, 0x0004003bU, 0x0000000eU,
    0x00000002U, 0x00000001U, 0x0004003bU, 0x0000000fU, 0x00000003U, 0x00000003U, 0x00050036U,
    0x00000010U, 0x00000001U, 0x00000000U, 0x00000011U, 0x000200f8U, 0x00000012U, 0x0004003dU,
    0x0000000dU, 0x00000013U, 0x00000002U, 0x0004003dU, 0x00000006U, 0x00000014U, 0x00000004U,
    0x0004003dU, 0x00000007U, 0x00000015U, 0x00000005U, 0x00050056U, 0x00000008U, 0x00000016U,
    0x00000014U, 0x00000015U, 0x00060057U, 0x0000000aU, 0x00000017U, 0x00000016U, 0x00000013U,
    0x00000000U, 0x0003003eU, 0x00000003U, 0x00000017U, 0x000100fdU, 0x00010038U,
};

ShaderArtifact vertex_shader() {
    return {ShaderStage::vertex, "vsMain", {vs_words.begin(), vs_words.end()}};
}

ShaderArtifact pixel_shader() {
    return {ShaderStage::pixel, "psMain", {ps_words.begin(), ps_words.end()}};
}

std::vector<std::byte> encode(const ShaderArtifact& artifact) {
    std::vector<std::byte> bytes;
    for (const auto word : artifact.words) {
        for (std::uint32_t shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<std::byte>((word >> shift) & 0xffU));
        }
    }
    return bytes;
}

std::size_t instruction(const ShaderArtifact& artifact, std::uint32_t opcode, std::uint32_t operand,
                        std::size_t operand_index = 1) {
    for (std::size_t offset = 5; offset < artifact.words.size();) {
        const auto count = artifact.words[offset] >> 16U;
        if ((artifact.words[offset] & 0xffffU) == opcode && count > operand_index &&
            artifact.words[offset + operand_index] == operand) {
            return offset;
        }
        offset += count;
    }
    throw std::runtime_error("fixture instruction missing");
}

OffscreenRequest valid_request() {
    OffscreenRequest request;
    request.vertices = {
        {{-1, -1, 0.5F}, {0, 0}}, {{1, -1, 0.5F}, {1, 0}}, {{0, 1, 0.5F}, {0.5F, 1}}};
    request.indices = {0, 1, 2};
    request.texture_extent = {1, 1};
    request.texture_rgba8 = {255, 128, 64, 255};
    request.vertex_shader = vertex_shader();
    request.pixel_shader = pixel_shader();
    return request;
}

class ShaderFiles final {
  public:
    ShaderFiles() {
        auto id = shark::generate_stable_id();
        if (!id) {
            throw std::runtime_error("cannot obtain temporary fixture identity");
        }
        path = std::filesystem::temp_directory_path() / ("shark-shader-" + id.value().to_string());
        if (!std::filesystem::create_directory(path)) {
            throw std::runtime_error("shader fixture directory already exists");
        }
    }
    ShaderFiles(const ShaderFiles&) = delete;
    ShaderFiles& operator=(const ShaderFiles&) = delete;
    ~ShaderFiles() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

} // namespace

// Shared only by the CPU graphics-owner seam tests in this test executable.
namespace shark::client::test {
OffscreenRequest offscreen_request_fixture() {
    return valid_request();
}
} // namespace shark::client::test

TEST_CASE("Owned shader artifacts decode real pinned compiler output", "[client][gpu-resources]") {
    for (const auto& shader : {vertex_shader(), pixel_shader()}) {
        REQUIRE(validate_shader_artifact(shader));
        auto bytes = encode(shader);
        auto decoded = decode_shader_artifact(bytes, shader.stage);
        REQUIRE(decoded);
        CHECK(decoded.value().words == shader.words);
        CHECK(decoded.value().entry_point == shader.entry_point);
        bytes[0] = std::byte{0};
        CHECK(validate_shader_artifact(decoded.value()));
        CHECK_FALSE(decode_shader_artifact(bytes, shader.stage));
    }
}

TEST_CASE("Shader artifact file loading reports paths and owns its bytes",
          "[client][gpu-resources]") {
    ShaderFiles files;
    const auto path = files.path / std::filesystem::path(u8"cube space \u03bb.spv");
    const auto missing = load_shader_artifact(path, ShaderStage::vertex);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == shark::ErrorCode::io_failure);
    CHECK(missing.error().context.operation == "shader.load");
    CHECK_FALSE(missing.error().context.path.empty());
    const auto bytes = encode(vertex_shader());
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        REQUIRE(output.good());
    }
    auto loaded = load_shader_artifact(path, ShaderStage::vertex);
    REQUIRE(loaded);
    CHECK(loaded.value().words == vertex_shader().words);
    CHECK_FALSE(load_shader_artifact(path, ShaderStage::pixel));
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "bad";
    }
    CHECK(validate_shader_artifact(loaded.value()));
    const auto malformed = load_shader_artifact(path, ShaderStage::vertex);
    REQUIRE_FALSE(malformed);
    CHECK(malformed.error().code == shark::ErrorCode::invalid_argument);
    CHECK_FALSE(malformed.error().context.path.empty());
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.seekp(4 * 1024 * 1024);
        output.put('x');
        REQUIRE(output.good());
    }
    CHECK_FALSE(load_shader_artifact(path, ShaderStage::vertex));
}

TEST_CASE("Shader reader rejects malformed bounded binary framing", "[client][gpu-resources]") {
    CHECK_FALSE(decode_shader_artifact({}, ShaderStage::vertex));
    auto shader = vertex_shader();
    auto bytes = encode(shader);
    bytes.pop_back();
    CHECK_FALSE(decode_shader_artifact(bytes, ShaderStage::vertex));
    bytes.resize(4U * 1024U * 1024U + 4U);
    CHECK_FALSE(decode_shader_artifact(bytes, ShaderStage::vertex));
    SECTION("byte order") {
        shader.words[0] = 0x03022307;
    }
    SECTION("unsupported version") {
        shader.words[1] = 0x00010700;
    }
    SECTION("schema") {
        shader.words[4] = 1;
    }
    SECTION("zero bound") {
        shader.words[3] = 0;
    }
    SECTION("oversized bound") {
        shader.words[3] = 65537;
    }
    SECTION("zero length instruction") {
        shader.words[5] = 17;
    }
    SECTION("truncated instruction") {
        shader.words[5] = 0xffff0011;
    }
    SECTION("truncated tail") {
        shader.words.pop_back();
    }
    CHECK_FALSE(validate_shader_artifact(shader));
}

TEST_CASE("Shader file paths reject malformed native encoding before opening",
          "[client][gpu-resources]") {
    ShaderFiles files;
    const auto path = files.path / "cube.spv";
    const auto bytes = encode(vertex_shader());
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        REQUIRE(output.good());
    }
    REQUIRE(load_shader_artifact(path, ShaderStage::vertex));
    auto nul_native = path.native();
    nul_native.push_back(std::filesystem::path::value_type{});
    nul_native += std::filesystem::path("ignored-suffix").native();
    std::vector<std::filesystem::path> invalid{
        {}, std::filesystem::path(nul_native), std::filesystem::path(std::string(4097, 'x'))};
    std::u8string oversized_utf8;
    for (std::size_t index = 0; index < 2049; ++index) {
        oversized_utf8 += u8"\u03bb";
    }
    invalid.emplace_back(oversized_utf8);
#if defined(_WIN32)
    invalid.emplace_back(std::wstring{static_cast<wchar_t>(0xd800)});
    invalid.emplace_back(std::wstring{static_cast<wchar_t>(0xdc00)});
    invalid.emplace_back(std::wstring{static_cast<wchar_t>(0xd800), L'x'});
#else
    invalid.emplace_back(std::string("\xc0\x80"));
    invalid.emplace_back(std::string("\xed\xa0\x80"));
    invalid.emplace_back(std::string("\xf4\x90\x80\x80"));
    invalid.emplace_back(std::string("\xc2"));
    invalid.emplace_back(std::string("\xe0\x80\x80"));
    invalid.emplace_back(std::string("\xc2x"));
#endif
    for (const auto& candidate : invalid) {
        const auto result = load_shader_artifact(candidate, ShaderStage::vertex);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == shark::ErrorCode::invalid_path);
        CHECK(result.error().context.operation == "shader.load");
        CHECK(result.error().context.path.empty());
    }
    const auto directory = load_shader_artifact(files.path, ShaderStage::vertex);
    REQUIRE_FALSE(directory);
    CHECK(directory.error().code == shark::ErrorCode::io_failure);
    CHECK(directory.error().context.path.size() <= 4096);
}

TEST_CASE("Shader reader rejects stage entry and interface mutations", "[client][gpu-resources]") {
    auto shader = vertex_shader();
    SECTION("stage") {
        shader.stage = ShaderStage::pixel;
    }
    SECTION("unsupported stage") {
        shader.stage = static_cast<ShaderStage>(123);
    }
    SECTION("entry contract") {
        shader.entry_point = "main";
    }
    SECTION("entry payload") {
        shader.words[instruction(shader, 15, 0) + 3] = 0x616d;
    }
    SECTION("interface ID out of bound") {
        shader.words[instruction(shader, 15, 0) + 5] = shader.words[3];
    }
    SECTION("missing interface variable") {
        shader.words[instruction(shader, 15, 0) + 5] = 1;
    }
    SECTION("duplicate interface variable") {
        shader.words[instruction(shader, 15, 0) + 5] = 3;
    }
    SECTION("vertex position float width") {
        shader.words[instruction(shader, 23, 15) + 3] = 4;
    }
    SECTION("vertex UV location") {
        shader.words[instruction(shader, 71, 3) + 3] = 2;
    }
    SECTION("interstage UV width") {
        shader.words[instruction(shader, 23, 17) + 3] = 3;
    }
    SECTION("position builtin") {
        shader.words[instruction(shader, 71, 4) + 3] = 1;
    }
    SECTION("unsupported decoration group") {
        shader.words.push_back((1U << 16) | 73U);
    }
    const auto result = validate_shader_artifact(shader);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == shark::ErrorCode::invalid_argument);
    CHECK(result.error().context.operation == "shader.layout");
}

TEST_CASE("Shader reader rejects mismatched descriptor and matrix layouts",
          "[client][gpu-resources]") {
    auto shader = vertex_shader();
    SECTION("set") {
        shader.words[instruction(shader, 71, 34, 2) + 3] = 1;
    }
    SECTION("binding") {
        shader.words[instruction(shader, 71, 33, 2) + 3] = 257;
    }
    SECTION("duplicate set decoration") {
        shader.words.insert(shader.words.end(), {(4U << 16) | 71U, 6, 34, 0});
    }
    SECTION("duplicate type") {
        shader.words.insert(shader.words.end(), {(3U << 16) | 22U, 10, 32});
    }
    SECTION("member offset") {
        shader.words[instruction(shader, 72, 35, 3) + 4] = 16;
    }
    SECTION("member stride") {
        shader.words[instruction(shader, 72, 7, 3) + 4] = 32;
    }
    SECTION("member major order") {
        shader.words[instruction(shader, 72, 4, 3) + 3] = 5;
    }
    SECTION("member index") {
        shader.words[instruction(shader, 72, 35, 3) + 2] = 1;
    }
    SECTION("matrix dimension") {
        shader.words[instruction(shader, 24, 13) + 3] = 3;
    }
    SECTION("descriptor type") {
        shader.words[instruction(shader, 32, 14) + 3] = 12;
    }
    SECTION("pointer storage class") {
        shader.words[instruction(shader, 32, 14) + 2] = 0;
    }
    REQUIRE_FALSE(validate_shader_artifact(shader));
}

TEST_CASE("Pixel stage requires exact texture sampler and color interfaces",
          "[client][gpu-resources]") {
    auto shader = pixel_shader();
    SECTION("texture array") {
        shader.words[instruction(shader, 25, 6) + 5] = 1;
    }
    SECTION("texture dimension") {
        shader.words[instruction(shader, 25, 6) + 3] = 2;
    }
    SECTION("storage image") {
        shader.words[instruction(shader, 25, 6) + 7] = 2;
    }
    SECTION("texture component type") {
        shader.words[instruction(shader, 22, 9) + 2] = 64;
    }
    SECTION("sampler binding") {
        const auto offset = instruction(shader, 71, 5);
        shader.words[offset + 3] = 1; // First decoration is DescriptorSet.
    }
    SECTION("color width") {
        shader.words[instruction(shader, 23, 10) + 3] = 3;
    }
    SECTION("sampler type") {
        shader.words[instruction(shader, 32, 12) + 3] = 6;
    }
    REQUIRE_FALSE(validate_shader_artifact(shader));
}

TEST_CASE("Offscreen request validates bounded owned triangle uploads", "[client][gpu-resources]") {
    auto request = valid_request();
    REQUIRE(validate_offscreen_request(request));
    SECTION("zero target extent") {
        request.extent.width = 0;
    }
    SECTION("oversized target extent") {
        request.extent.height = 1025;
    }
    SECTION("zero texture extent") {
        request.texture_extent.height = 0;
    }
    SECTION("overflowing texture extent") {
        request.texture_extent = {std::numeric_limits<std::uint32_t>::max(),
                                  std::numeric_limits<std::uint32_t>::max()};
    }
    SECTION("short upload") {
        request.texture_rgba8.pop_back();
    }
    SECTION("trailing upload") {
        request.texture_rgba8.push_back(0);
    }
    SECTION("too few vertices") {
        request.vertices.pop_back();
    }
    SECTION("too many vertices") {
        request.vertices.resize(65537);
    }
    SECTION("index outside vertex storage") {
        request.indices[2] = 3;
    }
    SECTION("incomplete triangle") {
        request.indices.pop_back();
    }
    SECTION("too many indices") {
        request.indices.resize(196611);
    }
    SECTION("infinite vertex") {
        request.vertices[0].position[0] = std::numeric_limits<float>::infinity();
    }
    SECTION("NaN UV") {
        request.vertices[0].uv[1] = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("NaN matrix") {
        request.transform[12] = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("wrong stage") {
        request.pixel_shader = vertex_shader();
    }
    SECTION("mutated loaded artifact") {
        request.vertex_shader.words[0] = 0;
    }
    REQUIRE_FALSE(validate_offscreen_request(request));
}

TEST_CASE("Offscreen upload accepts the documented finite boundary", "[client][gpu-resources]") {
    auto request = valid_request();
    request.extent = {1024, 1024};
    request.texture_extent = {1024, 1024};
    request.texture_rgba8.resize(1024U * 1024U * 4U);
    request.vertices.resize(65536);
    request.indices.resize(196608, 65535);
    REQUIRE(validate_offscreen_request(request));
}
