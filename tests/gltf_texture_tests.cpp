// Original Shark-authored offline fixtures; MIT, see fixtures/gltf_textures/LICENSE.
#include "shark/tools/gltf_textures.hpp"

#include "gltf_image_test_port.hpp"
#include "gltf_source_test_port.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace shark;
using namespace shark::tools;

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        INFO(result.error().context.path);
    }
    REQUIRE(result);
    return std::move(result).value();
}
ProjectPath path(std::string_view value) {
    return take(ProjectPath::parse(value));
}
AssetId asset(std::size_t index = 1) {
    auto suffix = std::to_string(index);
    suffix.insert(0, 12 - suffix.size(), '0');
    return AssetId{take(StableId::parse("12000000-0000-4000-8000-" + suffix))};
}
std::span<const std::byte> bytes(std::string_view value) {
    return std::as_bytes(std::span<const char>{value.data(), value.size()});
}
std::string fixture_file(const char* name) {
    const auto file = std::filesystem::path{SHARK_GLTF_TEXTURE_FIXTURE_DIRECTORY} / name;
    std::ifstream input{file, std::ios::binary};
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}
std::string join(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value : values) {
        if (!result.empty())
            result += ',';
        result += value;
    }
    return result;
}
std::string base64(std::string_view input) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t i = 0; i < input.size(); i += 3) {
        const auto first = static_cast<unsigned char>(input[i]);
        const auto second = i + 1 < input.size() ? static_cast<unsigned char>(input[i + 1]) : 0U;
        const auto third = i + 2 < input.size() ? static_cast<unsigned char>(input[i + 2]) : 0U;
        const auto value = (static_cast<unsigned>(first) << 16U) | (second << 8U) | third;
        result += alphabet[(value >> 18U) & 63U];
        result += alphabet[(value >> 12U) & 63U];
        result += i + 1 < input.size() ? alphabet[(value >> 6U) & 63U] : '=';
        result += i + 2 < input.size() ? alphabet[value & 63U] : '=';
    }
    return result;
}
std::string color_material(std::size_t texture = 0, std::size_t uv = 0) {
    return "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":" + std::to_string(texture) +
           ",\"texCoord\":" + std::to_string(uv) + "}}}";
}
std::string data_material(std::size_t texture = 0, std::size_t uv = 0) {
    return "{\"occlusionTexture\":{\"index\":" + std::to_string(texture) +
           ",\"texCoord\":" + std::to_string(uv) + "}}";
}
std::string normal_material(std::size_t texture = 0) {
    return "{\"normalTexture\":{\"index\":" + std::to_string(texture) + "}}";
}
std::string all_roles(std::size_t texture = 0) {
    const auto reference = "{\"index\":" + std::to_string(texture) + '}';
    return "{\"pbrMetallicRoughness\":{\"baseColorTexture\":" + reference +
           ",\"metallicRoughnessTexture\":" + reference + "},\"normalTexture\":" + reference +
           ",\"occlusionTexture\":" + reference + ",\"emissiveTexture\":" + reference + '}';
}
struct Fixture {
    std::string binary{fixture_file("triangle.bin")};
    std::vector<std::string> images{fixture_file("gamma-alpha.png")};
    std::vector<std::string> image_definitions;
    std::vector<std::string> textures{R"({"source":0})"};
    std::vector<std::string> materials{color_material()};
    std::vector<std::size_t> primitive_materials{0};
    std::string attributes{R"("POSITION":0,"TEXCOORD_0":1)"};
    std::string meshes;
    std::string nodes{R"([{"mesh":0}])"};
    std::string scenes{R"([{"nodes":[0]}])"};
    std::string samplers;
    std::string root_extra;
    bool default_scene{true};

    std::string primitive(std::size_t material) const {
        return "{\"attributes\":{" + attributes + "},\"material\":" + std::to_string(material) +
               '}';
    }
    std::string document() const {
        std::string result = R"({"asset":{"version":"2.0"})";
        result += ",\"buffers\":[{\"uri\":\"triangle.bin\",\"byteLength\":" +
                  std::to_string(binary.size()) + "}]";
        result +=
            R"(,"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}])";
        std::vector<std::string> definitions = image_definitions;
        if (definitions.empty()) {
            for (std::size_t i = 0; i < images.size(); ++i)
                definitions.push_back("{\"uri\":\"image" + std::to_string(i) + ".png\"}");
        }
        result += ",\"images\":[" + join(definitions) + "]";
        result += ",\"textures\":[" + join(textures) + "]";
        result += ",\"materials\":[" + join(materials) + "]";
        if (!samplers.empty())
            result += ",\"samplers\":" + samplers;
        std::vector<std::string> primitives;
        for (const auto material : primitive_materials)
            primitives.push_back(primitive(material));
        result += ",\"meshes\":" +
                  (meshes.empty() ? "[{\"primitives\":[" + join(primitives) + "]}]" : meshes);
        result += ",\"nodes\":" + nodes;
        if (!scenes.empty()) {
            result += ",\"scenes\":" + scenes;
            if (default_scene)
                result += ",\"scene\":0";
        }
        return result + root_extra + '}';
    }
};
Result<GltfTextures> convert(const Fixture& fixture, GltfTextureSettings settings = {},
                             std::stop_token stop = {}) {
    const auto text = fixture.document();
    std::vector<GltfExternalSource> external;
    external.push_back({path("triangle.bin"), asset(), bytes(fixture.binary)});
    if (fixture.image_definitions.empty()) {
        for (std::size_t i = 0; i < fixture.images.size(); ++i)
            external.push_back({path("image" + std::to_string(i) + ".png"), asset(i + 2),
                                bytes(fixture.images[i])});
    }
    return convert_gltf_textures({path("model.gltf"), bytes(text), external, stop}, settings);
}
void rejected(const Result<GltfTextures>& result, std::string_view source = "model.gltf") {
    REQUIRE_FALSE(result);
    CHECK_FALSE(result.error().message.empty());
    CHECK(result.error().context.operation == "asset.gltf.textures");
    CHECK(result.error().context.path == source);
    CHECK(detail::gltf_live_allocations() == 0);
    CHECK(detail::gltf_image_live_allocations() == 0);
}
std::vector<unsigned> components(const GltfTextureImage& image, std::size_t level = 0) {
    REQUIRE(level < image.mips.size());
    const auto& data = image.mips[level].rgba;
    std::vector<unsigned> result;
    const auto width = image.component_bits / 8;
    REQUIRE((width == 1 || width == 2));
    REQUIRE(data.size() % width == 0);
    for (std::size_t i = 0; i < data.size(); i += width) {
        auto value = std::to_integer<unsigned>(data[i]);
        if (width == 2)
            value |= std::to_integer<unsigned>(data[i + 1]) << 8U;
        result.push_back(value);
    }
    return result;
}
void check_components(const GltfTextureImage& image, std::initializer_list<unsigned> expected,
                      std::size_t level = 0) {
    CHECK(components(image, level) == std::vector<unsigned>(expected));
}
std::uint32_t big_integer(std::string_view input, std::size_t offset) {
    REQUIRE(input.size() >= offset + 4);
    std::uint32_t result{};
    for (std::size_t i = 0; i < 4; ++i)
        result = (result << 8U) | static_cast<unsigned char>(input[offset + i]);
    return result;
}
void big_integer(std::string& input, std::size_t offset, std::uint32_t value) {
    REQUIRE(input.size() >= offset + 4);
    for (std::size_t i = 0; i < 4; ++i)
        input[offset + i] = static_cast<char>((value >> ((3 - i) * 8)) & 255U);
}
std::uint32_t crc(std::string_view input) {
    std::uint32_t value = 0xffffffffU;
    for (const auto character : input) {
        value ^= static_cast<unsigned char>(character);
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1U) ^ ((value & 1U) != 0 ? 0xedb88320U : 0U);
    }
    return value ^ 0xffffffffU;
}
std::string png_chunk(std::string_view type, std::string_view payload) {
    REQUIRE(type.size() == 4);
    std::string result(4, '\0');
    big_integer(result, 0, static_cast<std::uint32_t>(payload.size()));
    result += type;
    result += payload;
    const auto checksum = crc(std::string_view{result}.substr(4));
    result.append(4, '\0');
    big_integer(result, result.size() - 4, checksum);
    return result;
}
std::size_t chunk_offset(std::string_view png, std::string_view type) {
    std::size_t offset = 8;
    while (offset + 12 <= png.size()) {
        const auto length = big_integer(png, offset);
        REQUIRE(length <= png.size() - offset - 12);
        if (png.substr(offset + 4, 4) == type)
            return offset;
        offset += length + 12;
    }
    FAIL("Required original PNG chunk is absent");
}
std::string chunk_data(std::string_view png, std::string_view type) {
    const auto offset = chunk_offset(png, type);
    return std::string{png.substr(offset + 8, big_integer(png, offset))};
}
void replace_chunk(std::string& png, std::string_view type, std::string_view payload) {
    const auto offset = chunk_offset(png, type);
    png.replace(offset, big_integer(png, offset) + 12, png_chunk(type, payload));
}
void dimensions(std::string& png, std::uint32_t width, std::uint32_t height) {
    auto header = chunk_data(png, "IHDR");
    big_integer(header, 0, width);
    big_integer(header, 4, height);
    replace_chunk(png, "IHDR", header);
}
std::string zlib_stored(std::string_view raw) {
    REQUIRE(raw.size() <= 65535);
    std::string result{"\x78\x01\x01", 3};
    const auto length = static_cast<unsigned>(raw.size());
    result += static_cast<char>(length & 255U);
    result += static_cast<char>((length >> 8U) & 255U);
    result += static_cast<char>((length ^ 65535U) & 255U);
    result += static_cast<char>(((length ^ 65535U) >> 8U) & 255U);
    result += raw;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const auto character : raw) {
        a = (a + static_cast<unsigned char>(character)) % 65521U;
        b = (b + a) % 65521U;
    }
    result.append(4, '\0');
    big_integer(result, result.size() - 4, (b << 16U) | a);
    return result;
}
struct AllocationReset {
    ~AllocationReset() {
        detail::gltf_fail_allocation(std::nullopt);
        detail::gltf_image_fail_allocation(std::nullopt);
    }
};
} // namespace

TEST_CASE("glTF texture saved external image has independent role color mip and identity results",
          "[gltf-textures]") {
    const auto source = fixture_file("external.gltf");
    const auto binary = fixture_file("triangle.bin");
    const auto image = fixture_file("gamma-alpha.png");
    const std::array external{GltfExternalSource{path("triangle.bin"), asset(), bytes(binary)},
                              GltfExternalSource{path("gamma-alpha.png"), asset(2), bytes(image)}};
    const auto result = take(convert_gltf_textures({path("model.gltf"), bytes(source), external}));
    REQUIRE(result.images.size() == 3);
    REQUIRE(result.usages.size() == 5);
    CHECK(result.selected_scene == 0);
    CHECK(result.source.container == GltfContainer::json);
    REQUIRE(result.source.resources.size() == 2);
    CHECK(result.source.resources[0].asset == asset());
    CHECK(result.source.resources[1].asset == asset(2));
    CHECK(result.source.resources[1].path == path("gamma-alpha.png"));
    CHECK(result.source.resources[1].storage == GltfResourceStorage::external);
    CHECK(result.images[0].semantic == GltfTextureSemantic::srgb_color);
    CHECK(result.images[1].semantic == GltfTextureSemantic::linear_data);
    CHECK(result.images[2].semantic == GltfTextureSemantic::normal);
    for (const auto& output : result.images) {
        CHECK(output.source_image == 0);
        CHECK(output.component_bits == 8);
        REQUIRE(output.mips.size() == 2);
        CHECK(output.mips[0].width == 2);
        CHECK(output.mips[0].height == 2);
        CHECK(output.mips[1].width == 1);
        CHECK(output.mips[1].height == 1);
        check_components(output, {0, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 64, 255, 255, 255, 128});
    }
    check_components(result.images[0], {188, 188, 188, 112}, 1);
    check_components(result.images[1], {128, 128, 128, 112}, 1);
    check_components(result.images[2], {128, 128, 255, 112}, 1);
    const std::array roles{GltfTextureRole::base_color, GltfTextureRole::metallic_roughness,
                           GltfTextureRole::normal, GltfTextureRole::occlusion,
                           GltfTextureRole::emissive};
    const std::array<std::size_t, 5> expected_images{0, 1, 2, 1, 0};
    for (std::size_t i = 0; i < roles.size(); ++i) {
        CHECK(result.usages[i].role == roles[i]);
        CHECK(result.usages[i].image == expected_images[i]);
        CHECK(result.usages[i].source_material == 0);
        CHECK(result.usages[i].source_texture == 0);
        CHECK(result.usages[i].texcoord == 0);
    }
}

TEST_CASE("glTF texture embedded JSON and GLB preserve decoded pixels without invented AssetIds",
          "[gltf-textures]") {
    for (const auto* file : {"embedded.gltf", "embedded.glb"}) {
        INFO(file);
        const auto source = fixture_file(file);
        const auto result = take(convert_gltf_textures({path(file), bytes(source), {}}));
        REQUIRE(result.images.size() == 3);
        REQUIRE(result.usages.size() == 5);
        REQUIRE(result.source.resources.size() == 2);
        for (const auto& resource : result.source.resources) {
            CHECK_FALSE(resource.asset);
            CHECK_FALSE(resource.path);
        }
        CHECK(result.source.container == (std::string_view{file}.ends_with("glb")
                                              ? GltfContainer::binary
                                              : GltfContainer::json));
        CHECK(result.source.resources[1].storage == (std::string_view{file}.ends_with("glb")
                                                         ? GltfResourceStorage::buffer_view
                                                         : GltfResourceStorage::data_uri));
        check_components(result.images[0], {188, 188, 188, 112}, 1);
        check_components(result.images[1], {128, 128, 128, 112}, 1);
        check_components(result.images[2], {128, 128, 255, 112}, 1);
    }
}

TEST_CASE("glTF texture JPEG and PNG16 data URIs preserve media and source precision",
          "[gltf-textures]") {
    for (const auto* name : {"baseline.jpg", "precision16.png"}) {
        Fixture input;
        input.images[0] = fixture_file(name);
        const auto mime = std::string_view{name}.ends_with("jpg") ? "image/jpeg" : "image/png";
        input.image_definitions = {"{\"uri\":\"data:" + std::string{mime} + ";base64," +
                                   base64(input.images[0]) + "\"}"};
        input.materials = {data_material()};
        const auto result = take(convert(input));
        REQUIRE(result.source.resources.size() == 2);
        CHECK(result.source.resources[1].storage == GltfResourceStorage::data_uri);
        CHECK(result.source.resources[1].mime_type == mime);
        CHECK_FALSE(result.source.resources[1].asset);
        CHECK(result.images[0].component_bits ==
              (std::string_view{name}.ends_with("jpg") ? 8U : 16U));
    }
}

TEST_CASE("glTF texture PNG16 retains every source low byte and little endian mip precision",
          "[gltf-textures]") {
    Fixture input;
    input.images = {fixture_file("precision16.png")};
    input.materials = {data_material()};
    const auto result = take(convert(input));
    REQUIRE(result.images.size() == 1);
    const auto& image = result.images[0];
    CHECK(image.component_bits == 16);
    REQUIRE(image.mips.size() == 2);
    REQUIRE(image.mips[0].rgba.size() == 16);
    CHECK(std::to_integer<unsigned>(image.mips[0].rgba[0]) == 2);
    CHECK(std::to_integer<unsigned>(image.mips[0].rgba[1]) == 1);
    CHECK(std::to_integer<unsigned>(image.mips[0].rgba[4]) == 0xcd);
    CHECK(std::to_integer<unsigned>(image.mips[0].rgba[5]) == 0xab);
    check_components(image, {0x0102, 0x1234, 0xabcd, 0x4321, 0xfedc, 0x8765, 0x0fed, 0xffff});
    check_components(image, {32751, 19661, 24029, 41360}, 1);
}

TEST_CASE("glTF texture PNG packed gray channels expand to exact opaque RGBA8", "[gltf-textures]") {
    const std::array<const char*, 4> names{"gray1.png", "gray2.png", "gray4.png", "gray8.png"};
    const std::array<std::vector<unsigned>, 4> expected{
        std::vector<unsigned>{255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255},
        std::vector<unsigned>{0, 0, 0, 255, 85, 85, 85, 255, 170, 170, 170, 255, 255, 255, 255,
                              255},
        std::vector<unsigned>{0, 0, 0, 255, 136, 136, 136, 255, 255, 255, 255, 255},
        std::vector<unsigned>{19, 19, 19, 255, 231, 231, 231, 255}};
    for (std::size_t i = 0; i < names.size(); ++i) {
        INFO(names[i]);
        Fixture input;
        input.images = {fixture_file(names[i])};
        input.materials = {data_material()};
        const auto result = take(convert(input));
        REQUIRE(result.images.size() == 1);
        CHECK(result.images[0].component_bits == 8);
        CHECK(components(result.images[0]) == expected[i]);
    }
}

TEST_CASE("glTF texture PNG RGB gray alpha palette and transparency preserve exact channels",
          "[gltf-textures]") {
    const std::array<const char*, 5> names{"rgb8.png", "gray-alpha8.png", "palette2.png",
                                           "rgb-transparent.png", "gray-transparent.png"};
    const std::array<std::vector<unsigned>, 5> expected{
        std::vector<unsigned>{1, 2, 3, 255, 240, 220, 200, 255},
        std::vector<unsigned>{19, 19, 19, 37, 231, 231, 231, 99},
        std::vector<unsigned>{5, 15, 25, 0, 35, 45, 55, 120, 65, 75, 85, 255},
        std::vector<unsigned>{1, 2, 3, 0, 9, 8, 7, 255},
        std::vector<unsigned>{19, 19, 19, 0, 231, 231, 231, 255}};
    for (std::size_t i = 0; i < names.size(); ++i) {
        INFO(names[i]);
        Fixture input;
        input.images = {fixture_file(names[i])};
        input.materials = {data_material()};
        const auto result = take(convert(input));
        CHECK(components(result.images[0]) == expected[i]);
    }
}

TEST_CASE("glTF texture Adam7 decoding restores independent top left pixel order",
          "[gltf-textures]") {
    Fixture input;
    input.images = {fixture_file("adam7.png")};
    input.materials = {data_material()};
    const auto result = take(convert(input));
    const auto& image = result.images[0];
    CHECK(image.mips[0].width == 5);
    CHECK(image.mips[0].height == 3);
    const auto rgba = components(image);
    REQUIRE(rgba.size() == 60);
    for (unsigned y = 0; y < 3; ++y) {
        for (unsigned x = 0; x < 5; ++x) {
            const auto offset = static_cast<std::size_t>((y * 5 + x) * 4);
            CHECK(rgba[offset] == x * 30);
            CHECK(rgba[offset + 1] == y * 50);
            CHECK(rgba[offset + 2] == x * 10 + y);
            CHECK(rgba[offset + 3] == 10 + x + y * 7);
        }
    }
}

TEST_CASE("glTF texture original baseline and progressive JPEG decode the same independent pixels",
          "[gltf-textures]") {
    for (const auto* name : {"baseline.jpg", "progressive.jpg"}) {
        INFO(name);
        Fixture input;
        input.images = {fixture_file(name)};
        input.materials = {data_material()};
        const auto result = take(convert(input));
        REQUIRE(result.images.size() == 1);
        const auto& image = result.images[0];
        CHECK(image.component_bits == 8);
        REQUIRE(image.mips.size() == 4);
        const std::array<unsigned, 4> dimensions{8, 4, 2, 1};
        for (std::size_t level = 0; level < dimensions.size(); ++level) {
            CHECK(image.mips[level].width == dimensions[level]);
            CHECK(image.mips[level].height == dimensions[level]);
            const auto rgba = components(image, level);
            for (std::size_t i = 0; i < rgba.size(); i += 4) {
                CHECK(rgba[i] == 128);
                CHECK(rgba[i + 1] == 128);
                CHECK(rgba[i + 2] == 128);
                CHECK(rgba[i + 3] == 255);
            }
        }
    }
}

TEST_CASE("glTF texture normal mips normalize vectors and average straight alpha independently",
          "[gltf-textures]") {
    Fixture input;
    input.images = {fixture_file("normal-pair.png")};
    input.materials = {normal_material()};
    const auto result = take(convert(input));
    const auto& image = result.images[0];
    CHECK(image.semantic == GltfTextureSemantic::normal);
    check_components(image, {255, 128, 128, 32, 128, 255, 128, 224});
    check_components(image, {218, 218, 128, 128}, 1);
    const auto rgba = components(image, 1);
    const auto x = static_cast<double>(rgba[0]) * 2 / 255 - 1;
    const auto y = static_cast<double>(rgba[1]) * 2 / 255 - 1;
    const auto z = static_cast<double>(rgba[2]) * 2 / 255 - 1;
    CHECK(std::abs(std::sqrt(x * x + y * y + z * z) - 1) < 0.01);
}

TEST_CASE("glTF texture exactly cancelling normal vectors use the finite positive Z fallback",
          "[gltf-textures]") {
    Fixture input;
    input.images = {fixture_file("normal-cancel.png")};
    input.materials = {normal_material()};
    const auto result = take(convert(input));
    check_components(result.images[0], {128, 128, 255, 128}, 1);
}

TEST_CASE("glTF texture odd square mips include every source texel", "[gltf-textures]") {
    Fixture input;
    input.images = {fixture_file("data-npot.png")};
    input.materials = {data_material()};
    const auto result = take(convert(input));
    const auto& image = result.images[0];
    REQUIRE(image.mips.size() == 2);
    CHECK(image.mips[0].width == 3);
    CHECK(image.mips[0].height == 3);
    CHECK(image.mips[1].width == 1);
    CHECK(image.mips[1].height == 1);
    check_components(image, {90, 40, 90, 255}, 1);
}

TEST_CASE("glTF texture odd one dimensional mips use area weights at fractional boundaries",
          "[gltf-textures]") {
    Fixture input;
    input.images = {fixture_file("wide-npot.png")};
    input.materials = {data_material()};
    const auto result = take(convert(input));
    const auto& image = result.images[0];
    REQUIRE(image.mips.size() == 3);
    CHECK(image.mips[1].width == 2);
    CHECK(image.mips[1].height == 1);
    CHECK(image.mips[2].width == 1);
    CHECK(image.mips[2].height == 1);
    check_components(image, {0, 0, 0, 255, 100, 0, 0, 255}, 1);
    check_components(image, {50, 0, 0, 255}, 2);
}

TEST_CASE("glTF texture sampler defaults and all six core minification modes are preserved",
          "[gltf-textures]") {
    Fixture default_input;
    const auto defaults = take(convert(default_input));
    REQUIRE(defaults.usages.size() == 1);
    const auto& sampler = defaults.usages[0].sampler;
    CHECK(sampler.magnification == GltfTextureFilter::linear);
    CHECK(sampler.minification == GltfTextureFilter::linear);
    CHECK(sampler.mip == GltfTextureMipFilter::linear);
    CHECK(sampler.wrap_s == GltfTextureWrap::repeat);
    CHECK(sampler.wrap_t == GltfTextureWrap::repeat);
    const std::array<unsigned, 6> values{9728, 9729, 9984, 9985, 9986, 9987};
    const std::array filters{GltfTextureFilter::nearest, GltfTextureFilter::linear,
                             GltfTextureFilter::nearest, GltfTextureFilter::linear,
                             GltfTextureFilter::nearest, GltfTextureFilter::linear};
    const std::array mips{GltfTextureMipFilter::none,    GltfTextureMipFilter::none,
                          GltfTextureMipFilter::nearest, GltfTextureMipFilter::nearest,
                          GltfTextureMipFilter::linear,  GltfTextureMipFilter::linear};
    for (std::size_t i = 0; i < values.size(); ++i) {
        INFO(values[i]);
        Fixture input;
        input.textures = {R"({"source":0,"sampler":0})"};
        input.samplers = "[{\"minFilter\":" + std::to_string(values[i]) + "}]";
        const auto result = take(convert(input));
        CHECK(result.usages[0].sampler.minification == filters[i]);
        CHECK(result.usages[0].sampler.mip == mips[i]);
        CHECK(result.images[0].mips.size() == 2);
    }
}

TEST_CASE("glTF texture independent magnification and wrap axes map every core mode",
          "[gltf-textures]") {
    const std::array<unsigned, 3> values{33071, 33648, 10497};
    const std::array wraps{GltfTextureWrap::clamp_to_edge, GltfTextureWrap::mirrored_repeat,
                           GltfTextureWrap::repeat};
    for (std::size_t i = 0; i < values.size(); ++i) {
        Fixture input;
        input.textures = {R"({"source":0,"sampler":0})"};
        input.samplers = "[{\"magFilter\":9728,\"wrapS\":" + std::to_string(values[i]) +
                         ",\"wrapT\":" + std::to_string(values[(i + 1) % values.size()]) + "}]";
        const auto result = take(convert(input));
        CHECK(result.usages[0].sampler.magnification == GltfTextureFilter::nearest);
        CHECK(result.usages[0].sampler.wrap_s == wraps[i]);
        CHECK(result.usages[0].sampler.wrap_t == wraps[(i + 1) % wraps.size()]);
    }
    Fixture input;
    input.textures = {R"({"source":0,"sampler":0})"};
    input.samplers = R"([{"magFilter":9729}])";
    CHECK(take(convert(input)).usages[0].sampler.magnification == GltfTextureFilter::linear);
}

TEST_CASE("glTF texture equal source images share semantic variants across textures and samplers",
          "[gltf-textures]") {
    Fixture input;
    input.textures = {R"({"source":0})", R"({"source":0,"sampler":0})"};
    input.samplers = R"([{"magFilter":9728,"minFilter":9728,"wrapS":33071}])";
    input.materials = {color_material(1), all_roles(0)};
    input.primitive_materials = {1, 0, 1, 0};
    const auto result = take(convert(input));
    REQUIRE(result.images.size() == 3);
    REQUIRE(result.usages.size() == 6);
    CHECK(result.usages[0].source_material == 0);
    CHECK(result.usages[1].source_material == 1);
    CHECK(result.usages[0].image == result.usages[1].image);
    CHECK(result.usages[0].image == result.usages[5].image);
    CHECK(result.usages[2].image == result.usages[4].image);
    CHECK(result.usages[0].sampler.mip == GltfTextureMipFilter::none);
    CHECK(result.usages[1].sampler.mip == GltfTextureMipFilter::linear);
}

TEST_CASE("glTF texture distinct source image indices never collapse on equal bytes or digests",
          "[gltf-textures]") {
    Fixture input;
    input.images.push_back(input.images[0]);
    input.textures.push_back(R"({"source":1})");
    input.materials = {color_material(), color_material(1)};
    input.primitive_materials = {0, 1};
    const auto result = take(convert(input));
    REQUIRE(result.images.size() == 2);
    CHECK(result.images[0].source_image == 0);
    CHECK(result.images[1].source_image == 1);
    CHECK(result.usages[0].image != result.usages[1].image);
    CHECK(result.images[0].mips == result.images[1].mips);
    REQUIRE(result.source.resources.size() == 3);
    CHECK(result.source.resources[1].digest == result.source.resources[2].digest);
    CHECK(result.source.resources[1].asset != result.source.resources[2].asset);
}

TEST_CASE("glTF texture selected reachable materials are unique and ordered by source material",
          "[gltf-textures]") {
    Fixture input;
    input.materials = {data_material(), normal_material(), color_material()};
    input.meshes = "[{\"primitives\":[" + input.primitive(2) + ',' + input.primitive(0) + ',' +
                   input.primitive(2) + "]},{\"primitives\":[" + input.primitive(1) + "]}]";
    input.nodes = R"([{"mesh":0},{"mesh":1},{"mesh":0}])";
    input.scenes = R"([{"nodes":[0,2]},{"nodes":[1]}])";
    const auto first = take(convert(input));
    REQUIRE(first.usages.size() == 2);
    REQUIRE(first.images.size() == 2);
    CHECK(first.usages[0].source_material == 0);
    CHECK(first.usages[1].source_material == 2);
    CHECK(first.images[0].semantic == GltfTextureSemantic::linear_data);
    CHECK(first.images[1].semantic == GltfTextureSemantic::srgb_color);
    const auto second = take(convert(input, {{1}}));
    REQUIRE(second.usages.size() == 1);
    CHECK(second.selected_scene == 1);
    CHECK(second.usages[0].source_material == 1);
    CHECK(second.images[0].semantic == GltfTextureSemantic::normal);
    CHECK(second.source.counts.materials == 3);
    CHECK(second.source.counts.meshes == 2);
    rejected(convert(input, {{2}}));
}

TEST_CASE("glTF texture no scenes and materialless primitives yield an empty selected conversion",
          "[gltf-textures]") {
    Fixture input;
    input.scenes.clear();
    const auto no_scene = take(convert(input));
    CHECK_FALSE(no_scene.selected_scene);
    CHECK(no_scene.images.empty());
    CHECK(no_scene.usages.empty());
    CHECK(no_scene.source.resources.size() == 2);
    rejected(convert(input, {{0}}));
    input.scenes = R"([{"nodes":[0]}])";
    input.default_scene = false;
    CHECK(take(convert(input)).selected_scene == 0);
    input.meshes = "[{\"primitives\":[{\"attributes\":{" + input.attributes + "}}]}]";
    const auto materialless = take(convert(input));
    CHECK(materialless.usages.empty());
    CHECK(materialless.images.empty());
}

TEST_CASE("glTF texture every selected primitive must provide its referenced supported UV set",
          "[gltf-textures]") {
    Fixture input;
    input.materials = {color_material(0, 1)};
    rejected(convert(input));
    input.attributes = R"("POSITION":0,"TEXCOORD_1":1)";
    const auto valid = take(convert(input));
    CHECK(valid.usages[0].texcoord == 1);
    input.materials = {color_material(0, 2)};
    rejected(convert(input));
    input.materials = {color_material()};
    input.attributes = R"("POSITION":0,"TEXCOORD_0":1)";
    input.meshes = "[{\"primitives\":[" + input.primitive(0) +
                   R"(,{"attributes":{"POSITION":0},"material":0}]}])";
    rejected(convert(input));
}

TEST_CASE(
    "glTF texture unused image contents stay outside selected decoding while inventory remains",
    "[gltf-textures]") {
    Fixture input;
    auto unused = fixture_file("gamma-alpha.png");
    unused.resize(16);
    input.images.push_back(std::move(unused));
    input.textures.push_back(R"({"source":1})");
    input.materials.push_back(color_material(1));
    const auto result = take(convert(input));
    REQUIRE(result.images.size() == 1);
    REQUIRE(result.source.resources.size() == 3);
    CHECK(result.source.resources[2].byte_count == 16);
    CHECK(result.source.counts.materials == 2);
    CHECK(result.source.counts.textures == 2);
}

TEST_CASE("glTF texture cancellation before conversion allocates neither parser nor decoder work",
          "[gltf-textures]") {
    AllocationReset reset;
    detail::gltf_fail_allocation(std::nullopt);
    detail::gltf_image_fail_allocation(std::nullopt);
    std::stop_source cancellation;
    cancellation.request_stop();
    rejected(convert(Fixture{}, {}, cancellation.get_token()));
    CHECK(detail::gltf_allocation_attempts() == 0);
    CHECK(detail::gltf_image_allocation_attempts() == 0);
}

TEST_CASE(
    "glTF texture results own their pixels and repeated changed snapshots cannot mutate old output",
    "[gltf-textures]") {
    Fixture input;
    const auto first = take(convert(input));
    const auto again = take(convert(input));
    CHECK(first.images == again.images);
    CHECK(first.usages == again.usages);
    const auto original_digest = first.source.resources[1].digest;
    input.images[0] = fixture_file("data-npot.png");
    const auto changed = take(convert(input));
    CHECK(first.source.resources[1].digest == original_digest);
    CHECK(changed.source.resources[1].digest != original_digest);
    CHECK(first.images[0].mips[0].width == 2);
    CHECK(changed.images[0].mips[0].width == 3);
    input.images[0].assign(input.images[0].size(), '\0');
    input.binary.assign(input.binary.size(), '\0');
    check_components(first.images[0], {188, 188, 188, 112}, 1);
    CHECK(changed.images[0].mips[0].width == 3);
}

TEST_CASE("glTF texture PNG truncated at every byte rejects with contained owned failures",
          "[gltf-textures]") {
    Fixture input;
    const auto original = input.images[0];
    for (std::size_t count = 8; count < original.size(); ++count) {
        INFO(count);
        input.images[0] = original.substr(0, count);
        rejected(convert(input));
    }
}

TEST_CASE("glTF texture JPEG truncated at every byte rejects baseline and progressive framing",
          "[gltf-textures]") {
    for (const auto* name : {"baseline.jpg", "progressive.jpg"}) {
        INFO(name);
        Fixture input;
        const auto original = fixture_file(name);
        for (std::size_t count = 3; count < original.size(); ++count) {
            INFO(count);
            input.images[0] = original.substr(0, count);
            rejected(convert(input));
        }
    }
}

TEST_CASE("glTF texture PNG CRC and chunk arithmetic fail before decoder allocation",
          "[gltf-textures]") {
    AllocationReset reset;
    Fixture input;
    const auto original = input.images[0];
    for (const auto* type : {"IHDR", "IDAT", "IEND"}) {
        INFO(type);
        input.images[0] = original;
        const auto offset = chunk_offset(input.images[0], type);
        const auto checksum = offset + 8 + big_integer(input.images[0], offset);
        input.images[0][checksum] =
            static_cast<char>(static_cast<unsigned char>(input.images[0][checksum]) ^ 1U);
        detail::gltf_image_fail_allocation(std::nullopt);
        rejected(convert(input));
        CHECK(detail::gltf_image_allocation_attempts() == 0);
    }
    input.images[0] = original;
    big_integer(input.images[0], 33, 0x7fffffffU);
    detail::gltf_image_fail_allocation(std::nullopt);
    rejected(convert(input));
    CHECK(detail::gltf_image_allocation_attempts() == 0);
}

TEST_CASE("glTF texture PNG invalid depth color compression filter and interlace headers reject",
          "[gltf-textures]") {
    Fixture input;
    const auto original = input.images[0];
    for (const auto& [offset, value] :
         std::array<std::pair<std::size_t, char>, 5>{{{8, 3}, {9, 1}, {10, 1}, {11, 1}, {12, 2}}}) {
        INFO(offset);
        input.images[0] = original;
        auto header = chunk_data(input.images[0], "IHDR");
        header[offset] = value;
        replace_chunk(input.images[0], "IHDR", header);
        rejected(convert(input));
    }
}

TEST_CASE("glTF texture PNG duplicate missing reordered and unknown critical chunks reject",
          "[gltf-textures]") {
    Fixture input;
    const auto original = input.images[0];
    std::vector<std::string> malformed;
    auto duplicate = original;
    duplicate.insert(33, png_chunk("IHDR", chunk_data(original, "IHDR")));
    malformed.push_back(std::move(duplicate));
    malformed.push_back(original.substr(0, chunk_offset(original, "IEND")));
    malformed.push_back(original + '\0');
    auto critical = original;
    critical.insert(33, png_chunk("ABCD", {}));
    malformed.push_back(std::move(critical));
    auto noncontiguous = original;
    const auto idat = chunk_data(original, "IDAT");
    replace_chunk(noncontiguous, "IDAT", std::string_view{idat}.substr(0, 3));
    const auto tail = png_chunk("tEXt", std::string_view{"key\0value", 9}) +
                      png_chunk("IDAT", std::string_view{idat}.substr(3));
    noncontiguous.insert(chunk_offset(noncontiguous, "IEND"), tail);
    malformed.push_back(std::move(noncontiguous));
    for (std::size_t i = 0; i < malformed.size(); ++i) {
        INFO(i);
        input.images[0] = malformed[i];
        rejected(convert(input));
    }
}

TEST_CASE("glTF texture PNG Adler and DEFLATE consumed length are validated beyond chunk CRC",
          "[gltf-textures]") {
    Fixture input;
    const auto original = input.images[0];
    auto idat = chunk_data(original, "IDAT");
    idat.back() = static_cast<char>(static_cast<unsigned char>(idat.back()) ^ 1U);
    replace_chunk(input.images[0], "IDAT", idat);
    rejected(convert(input));
    input.images[0] = original;
    idat = chunk_data(original, "IDAT");
    idat.insert(idat.size() - 4, 1, '\0');
    replace_chunk(input.images[0], "IDAT", idat);
    rejected(convert(input));
    input.images[0] = original;
    idat = chunk_data(original, "IDAT");
    idat[0] = '\0';
    replace_chunk(input.images[0], "IDAT", idat);
    rejected(convert(input));
}

TEST_CASE(
    "glTF texture PNG decoded byte count and filter validity are independent of valid checksums",
    "[gltf-textures]") {
    Fixture input;
    const auto original = input.images[0];
    for (const auto count : {std::size_t{17}, std::size_t{19}}) {
        INFO(count);
        input.images[0] = original;
        replace_chunk(input.images[0], "IDAT", zlib_stored(std::string(count, '\0')));
        rejected(convert(input));
    }
    input.images[0] = original;
    auto raw = std::string(18, '\0');
    raw[0] = 5;
    replace_chunk(input.images[0], "IDAT", zlib_stored(raw));
    rejected(convert(input));
}

TEST_CASE("glTF texture PNG palette presence transparency bounds and decoded indices reject",
          "[gltf-textures]") {
    Fixture input;
    const auto original = fixture_file("palette2.png");
    input.images[0] = original;
    auto offset = chunk_offset(input.images[0], "PLTE");
    input.images[0].erase(offset, big_integer(input.images[0], offset) + 12);
    rejected(convert(input));
    input.images[0] = original;
    replace_chunk(input.images[0], "tRNS", std::string(4, '\0'));
    rejected(convert(input));
    input.images[0] = original;
    replace_chunk(input.images[0], "PLTE", std::string(4, '\0'));
    rejected(convert(input));
    input.images[0] = original;
    replace_chunk(input.images[0], "IDAT", zlib_stored(std::string{"\0\x1c", 2}));
    rejected(convert(input));
}

TEST_CASE("glTF texture PNG declared dimensions and aggregate pixels reject before decode work",
          "[gltf-textures]") {
    AllocationReset reset;
    Fixture input;
    const auto original = input.images[0];
    for (const auto& [width, height] : std::array<std::pair<std::uint32_t, std::uint32_t>, 5>{
             {{0, 2}, {2, 0}, {4097, 1}, {4096, 1025}, {0xffffffffU, 0xffffffffU}}}) {
        INFO(width);
        INFO(height);
        input.images[0] = original;
        dimensions(input.images[0], width, height);
        detail::gltf_image_fail_allocation(std::nullopt);
        rejected(convert(input));
        CHECK(detail::gltf_image_allocation_attempts() == 0);
    }
}

TEST_CASE("glTF texture all selected image headers are preflighted before decoding the first image",
          "[gltf-textures]") {
    AllocationReset reset;
    Fixture input;
    input.images.push_back(input.images[0]);
    dimensions(input.images[1], 4097, 1);
    input.textures.push_back(R"({"source":1})");
    input.materials.push_back(color_material(1));
    input.primitive_materials = {0, 1};
    detail::gltf_image_fail_allocation(std::nullopt);
    rejected(convert(input));
    CHECK(detail::gltf_image_allocation_attempts() == 0);
}

TEST_CASE(
    "glTF texture aggregate decoded and variant output amplification is charged before decode",
    "[gltf-textures]") {
    AllocationReset reset;
    for (const auto count : {std::size_t{2}, std::size_t{9}}) {
        Fixture input;
        auto encoded = input.images[0];
        dimensions(encoded, count == 2 ? 2048U : 1024U, 1024U);
        input.images.assign(count, encoded);
        input.textures.clear();
        input.materials.clear();
        input.primitive_materials.clear();
        for (std::size_t i = 0; i < count; ++i) {
            input.textures.push_back("{\"source\":" + std::to_string(i) + '}');
            input.materials.push_back(count == 2 ? all_roles(i) : color_material(i));
            input.primitive_materials.push_back(i);
        }
        detail::gltf_image_fail_allocation(std::nullopt);
        rejected(convert(input));
        CHECK(detail::gltf_image_allocation_attempts() == 0);
    }
}

TEST_CASE(
    "glTF texture repeated material roles cannot amplify usages beyond their independent bound",
    "[gltf-textures]") {
    AllocationReset reset;
    Fixture input;
    input.materials.assign(820, all_roles());
    input.primitive_materials.clear();
    for (std::size_t i = 0; i < input.materials.size(); ++i)
        input.primitive_materials.push_back(i);
    detail::gltf_image_fail_allocation(std::nullopt);
    const auto result = convert(input);
    rejected(result);
    CHECK(result.error().message.find("bound") != std::string::npos);
    CHECK(detail::gltf_image_allocation_attempts() == 0);
}

TEST_CASE(
    "glTF texture JPEG unsupported frame precision zero quantization and trailing content reject",
    "[gltf-textures]") {
    Fixture input;
    const auto original = fixture_file("baseline.jpg");
    auto precision = original;
    const auto frame = precision.find(std::string{"\xff\xc0", 2});
    REQUIRE(frame != std::string::npos);
    precision[frame + 4] = 12;
    auto unsupported = original;
    unsupported[frame + 1] = static_cast<char>(0xc3);
    auto quantization = original;
    quantization[7] = 0;
    const std::array malformed{precision, unsupported, quantization, original + '\0'};
    for (const auto& image : malformed) {
        input.images[0] = image;
        rejected(convert(input));
    }
}

TEST_CASE("glTF texture JPEG dimensions and repeated progressive scan work preflight before decode",
          "[gltf-textures]") {
    AllocationReset reset;
    Fixture input;
    input.images[0] = fixture_file("baseline.jpg");
    const auto frame = input.images[0].find(std::string{"\xff\xc0", 2});
    REQUIRE(frame != std::string::npos);
    input.images[0][frame + 7] = 0x10;
    input.images[0][frame + 8] = 1;
    detail::gltf_image_fail_allocation(std::nullopt);
    rejected(convert(input));
    CHECK(detail::gltf_image_allocation_attempts() == 0);
    auto progressive = fixture_file("progressive.jpg");
    const auto progressive_frame = progressive.find(std::string{"\xff\xc2", 2});
    REQUIRE(progressive_frame != std::string::npos);
    progressive[progressive_frame + 5] = 8;
    progressive[progressive_frame + 6] = 0;
    progressive[progressive_frame + 7] = 8;
    progressive[progressive_frame + 8] = 0;
    const auto scan = progressive.find(std::string{"\xff\xda", 2});
    REQUIRE(scan != std::string::npos);
    const auto first_scan = progressive.substr(scan, 11);
    for (unsigned i = 0; i < 8; ++i)
        progressive.insert(progressive.size() - 2, first_scan);
    input.images[0] = std::move(progressive);
    detail::gltf_image_fail_allocation(std::nullopt);
    rejected(convert(input));
    CHECK(detail::gltf_image_allocation_attempts() == 0);
}

TEST_CASE("glTF texture one encoded image decodes once for three semantic variants",
          "[gltf-textures]") {
    AllocationReset reset;
    Fixture input;
    detail::gltf_image_fail_allocation(std::nullopt);
    REQUIRE(convert(input));
    const auto single_attempts = detail::gltf_image_allocation_attempts();
    REQUIRE(single_attempts > 0);
    input.materials = {all_roles()};
    detail::gltf_image_fail_allocation(std::nullopt);
    const auto result = take(convert(input));
    CHECK(result.images.size() == 3);
    CHECK(detail::gltf_image_allocation_attempts() == single_attempts);
    CHECK(detail::gltf_image_live_allocations() == 0);
}

TEST_CASE("glTF texture every decoder allocation failure releases all PNG JPEG and PNG16 scratch",
          "[gltf-textures]") {
    AllocationReset reset;
    for (const auto* name : {"gamma-alpha.png", "precision16.png", "palette2.png", "adam7.png",
                             "baseline.jpg", "progressive.jpg"}) {
        INFO(name);
        Fixture input;
        input.images[0] = fixture_file(name);
        input.materials = {all_roles()};
        detail::gltf_image_fail_allocation(std::nullopt);
        REQUIRE(convert(input));
        const auto attempts = detail::gltf_image_allocation_attempts();
        REQUIRE(attempts > 0);
        REQUIRE(attempts < 128);
        for (std::size_t i = 0; i < attempts; ++i) {
            INFO(i);
            detail::gltf_image_fail_allocation(i);
            rejected(convert(input));
        }
        detail::gltf_image_fail_allocation(std::nullopt);
        REQUIRE(convert(input));
        CHECK(detail::gltf_image_live_allocations() == 0);
    }
}

TEST_CASE("glTF texture parser allocation failures remain contained before image decoding",
          "[gltf-textures]") {
    AllocationReset reset;
    Fixture input;
    detail::gltf_fail_allocation(std::nullopt);
    REQUIRE(convert(input));
    const auto attempts = detail::gltf_allocation_attempts();
    REQUIRE(attempts > 0);
    REQUIRE(attempts < 512);
    for (std::size_t i = 0; i < attempts; ++i) {
        INFO(i);
        detail::gltf_fail_allocation(i);
        rejected(convert(input));
    }
    detail::gltf_fail_allocation(std::nullopt);
    REQUIRE(convert(input));
}

TEST_CASE("glTF texture missing snapshots oversized encoded sources and invalid samplers preserve "
          "source fences",
          "[gltf-textures]") {
    Fixture input;
    auto document = input.document();
    const std::array external{
        GltfExternalSource{path("triangle.bin"), asset(), bytes(input.binary)}};
    rejected(convert_gltf_textures({path("model.gltf"), bytes(document), external}));
    input.images[0].resize(gltf_source_maximum_file_bytes + 1, '\0');
    rejected(convert(input));
    input.images[0] = fixture_file("gamma-alpha.png");
    input.textures = {R"({"source":0,"sampler":0})"};
    for (const auto* sampler : {R"([{"magFilter":9987}])", R"([{"minFilter":9999}])",
                                R"([{"wrapS":0}])", R"([{"wrapT":-1}])"}) {
        input.samplers = sampler;
        rejected(convert(input));
    }
    input.samplers = "[{}]";
    input.root_extra = R"(,"extensionsRequired":["KHR_texture_basisu"])";
    rejected(convert(input));
}

TEST_CASE("glTF texture JPEG missing tables unknown selectors and absent progressive DC reject",
          "[gltf-textures]") {
    Fixture input;
    const auto baseline = fixture_file("baseline.jpg");
    std::vector<std::string> malformed;
    for (const auto marker : {0xdbU, 0xc4U}) {
        auto image = baseline;
        const auto offset =
            image.find(std::string{static_cast<char>(0xff), static_cast<char>(marker)});
        REQUIRE(offset != std::string::npos);
        REQUIRE(image.size() >= offset + 4);
        const auto length =
            (static_cast<std::size_t>(static_cast<unsigned char>(image[offset + 2])) << 8U) |
            static_cast<unsigned char>(image[offset + 3]);
        image.erase(offset, length + 2);
        malformed.push_back(std::move(image));
    }
    auto unknown_huffman = baseline;
    const auto scan = unknown_huffman.find(std::string{"\xff\xda", 2});
    REQUIRE(scan != std::string::npos);
    unknown_huffman[scan + 6] = 0x11;
    malformed.push_back(std::move(unknown_huffman));
    auto unknown_quantization = baseline;
    const auto frame = unknown_quantization.find(std::string{"\xff\xc0", 2});
    REQUIRE(frame != std::string::npos);
    unknown_quantization[frame + 12] = 1;
    malformed.push_back(std::move(unknown_quantization));
    auto missing_dc = fixture_file("progressive.jpg");
    const auto dc_scan = missing_dc.find(std::string{"\xff\xda", 2});
    REQUIRE(dc_scan != std::string::npos);
    missing_dc.erase(dc_scan, 11);
    malformed.push_back(std::move(missing_dc));
    for (std::size_t i = 0; i < malformed.size(); ++i) {
        INFO(i);
        input.images[0] = malformed[i];
        rejected(convert(input));
    }
}

TEST_CASE("glTF texture JPEG restart intervals require every MCU and the exact marker sequence",
          "[gltf-textures]") {
    AllocationReset reset;
    Fixture input;
    input.materials = {data_material()};
    auto one_block = fixture_file("baseline.jpg");
    const auto frame = one_block.find(std::string{"\xff\xc0", 2});
    REQUIRE(frame != std::string::npos);
    one_block[frame + 7] = 0;
    one_block[frame + 8] = 16;
    const auto scan = one_block.find(std::string{"\xff\xda", 2});
    REQUIRE(scan != std::string::npos);
    one_block.insert(scan, std::string{"\xff\xdd\x00\x04\x00\x01", 6});

    // The original block has entropy 00 followed by six one-bit padding bits.
    // Resetting the DC predictor between two identical blocks preserves gray128.
    auto complete = one_block;
    complete.insert(complete.size() - 2, std::string{"\xff\xd0\x3f", 3});
    input.images[0] = complete;
    const auto result = take(convert(input));
    REQUIRE(result.images.size() == 1);
    const auto& image = result.images[0];
    REQUIRE(image.mips.size() == 5);
    const std::array<unsigned, 5> widths{16, 8, 4, 2, 1};
    const std::array<unsigned, 5> heights{8, 4, 2, 1, 1};
    for (std::size_t level = 0; level < widths.size(); ++level) {
        CHECK(image.mips[level].width == widths[level]);
        CHECK(image.mips[level].height == heights[level]);
        const auto rgba = components(image, level);
        REQUIRE(rgba.size() == static_cast<std::size_t>(widths[level]) * heights[level] * 4);
        for (std::size_t offset = 0; offset < rgba.size(); offset += 4) {
            CHECK(rgba[offset] == 128);
            CHECK(rgba[offset + 1] == 128);
            CHECK(rgba[offset + 2] == 128);
            CHECK(rgba[offset + 3] == 255);
        }
    }

    const auto restart = complete.find(std::string{"\xff\xd0", 2});
    REQUIRE(restart != std::string::npos);
    auto missing_marker = complete;
    missing_marker.erase(restart, 2);
    auto extra_marker = complete;
    extra_marker.insert(extra_marker.size() - 2, std::string{"\xff\xd1\x3f", 3});
    auto out_of_order = complete;
    out_of_order[restart + 1] = static_cast<char>(0xd1);
    const std::array malformed{one_block, missing_marker, extra_marker, out_of_order};
    for (std::size_t i = 0; i < malformed.size(); ++i) {
        INFO(i);
        input.images[0] = malformed[i];
        detail::gltf_image_fail_allocation(std::nullopt);
        rejected(convert(input));
        CHECK(detail::gltf_image_allocation_attempts() == 0);
    }
}

TEST_CASE("glTF texture JPEG subsampled interleaved and component scans count their actual MCUs",
          "[gltf-textures]") {
    const auto baseline = fixture_file("baseline.jpg");
    const auto frame = baseline.find(std::string{"\xff\xc0", 2});
    const auto scan = baseline.find(std::string{"\xff\xda", 2});
    REQUIRE(frame != std::string::npos);
    REQUIRE(scan != std::string::npos);
    // 16x8 YCbCr: Y has horizontal sampling two; Cb and Cr each have one.
    // A single interleaved MCU contains four blocks. A Y-only scan has two MCUs,
    // while each chroma-only scan has one. Every quantized coefficient is zero.
    const std::string subsampled_frame{
        "\xff\xc0\x00\x11\x08\x00\x08\x00\x10\x03\x01\x21\x00\x02\x11\x00\x03\x11\x00", 19};
    const auto header = baseline.substr(0, frame) + subsampled_frame +
                        baseline.substr(frame + 13, scan - frame - 13) +
                        std::string{"\xff\xdd\x00\x04\x00\x01", 6};
    auto separate = header;
    for (unsigned component = 1; component <= 3; ++component) {
        auto component_scan = baseline.substr(scan, 10);
        component_scan[5] = static_cast<char>(component);
        separate += component_scan;
        separate += static_cast<char>(0x3f);
        if (component == 1)
            separate += std::string{"\xff\xd0\x3f", 3};
    }
    separate += std::string{"\xff\xd9", 2};
    const auto interleaved =
        header + std::string{"\xff\xda\x00\x0c\x03\x01\x00\x02\x00\x03\x00\x00\x3f\x00", 14} +
        std::string{"\x00\xff\xd9", 3};
    for (const auto& encoded : std::array{separate, interleaved}) {
        Fixture input;
        input.materials = {data_material()};
        input.images[0] = encoded;
        const auto result = take(convert(input));
        REQUIRE(result.images.size() == 1);
        CHECK(result.images[0].mips[0].width == 16);
        CHECK(result.images[0].mips[0].height == 8);
        const auto rgba = components(result.images[0]);
        REQUIRE(rgba.size() == 16 * 8 * 4);
        for (std::size_t offset = 0; offset < rgba.size(); offset += 4) {
            CHECK(rgba[offset] == 128);
            CHECK(rgba[offset + 1] == 128);
            CHECK(rgba[offset + 2] == 128);
            CHECK(rgba[offset + 3] == 255);
        }
    }
}
