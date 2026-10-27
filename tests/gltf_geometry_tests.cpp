// Original Shark-authored offline fixtures; MIT, see fixtures/gltf_geometry/LICENSE.
#include "shark/tools/gltf_geometry.hpp"

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
#include <limits>
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
AssetId asset() {
    return AssetId{take(StableId::parse("12000000-0000-4000-8000-000000000012"))};
}
std::span<const std::byte> bytes(std::string_view value) {
    return std::as_bytes(std::span<const char>{value.data(), value.size()});
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
void append_integer(std::string& output, std::uint32_t value, unsigned width) {
    for (unsigned offset = 0; offset < width; ++offset)
        output.push_back(static_cast<char>((value >> (offset * 8U)) & 255U));
}
std::string floats(std::initializer_list<float> values) {
    std::string result;
    for (const auto value : values)
        append_integer(result, std::bit_cast<std::uint32_t>(value), 4);
    return result;
}
std::string integers(std::initializer_list<std::uint32_t> values, unsigned width) {
    std::string result;
    for (const auto value : values)
        append_integer(result, value, width);
    return result;
}

struct Fixture {
    std::string binary;
    std::vector<std::string> views;
    std::vector<std::string> accessors;
    std::vector<std::string> attributes;
    std::string primitive_extra;
    std::string nodes{R"([{"mesh":0}])"};
    std::string scenes{R"([{"nodes":[0]}])"};
    std::string meshes;
    std::string root_extra;
    bool default_scene{true};

    std::size_t view(std::string data, std::size_t stride = 0) {
        binary.append((4 - binary.size() % 4) % 4, '\0');
        const auto offset = binary.size();
        binary += data;
        auto text = "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) +
                    ",\"byteLength\":" + std::to_string(data.size());
        if (stride != 0)
            text += ",\"byteStride\":" + std::to_string(stride);
        views.push_back(text + '}');
        return views.size() - 1;
    }
    std::size_t add(std::string data, std::string_view type, unsigned component, std::size_t count,
                    bool normalized = false, std::size_t stride = 0) {
        const auto position = view(std::move(data), stride);
        auto text = "{\"bufferView\":" + std::to_string(position) +
                    ",\"componentType\":" + std::to_string(component) +
                    ",\"count\":" + std::to_string(count) + ",\"type\":\"" + std::string{type} +
                    '"';
        if (normalized)
            text += ",\"normalized\":true";
        accessors.push_back(text + '}');
        return accessors.size() - 1;
    }
    void attribute(std::string_view name, std::size_t accessor) {
        attributes.push_back('"' + std::string{name} + "\":" + std::to_string(accessor));
    }
    std::string primitive() const {
        return "{\"attributes\":{" + join(attributes) + '}' + primitive_extra + '}';
    }
    std::string document() const {
        std::string result = R"({"asset":{"version":"2.0"})";
        if (!binary.empty())
            result += ",\"buffers\":[{\"uri\":\"data.bin\",\"byteLength\":" +
                      std::to_string(binary.size()) + "}]";
        if (!views.empty())
            result += ",\"bufferViews\":[" + join(views) + ']';
        if (!accessors.empty())
            result += ",\"accessors\":[" + join(accessors) + ']';
        result +=
            ",\"meshes\":" + (meshes.empty() ? "[{\"primitives\":[" + primitive() + "]}]" : meshes);
        result += ",\"nodes\":" + nodes;
        if (!scenes.empty()) {
            result += ",\"scenes\":" + scenes;
            if (default_scene)
                result += ",\"scene\":0";
        }
        return result + root_extra + '}';
    }
};
Fixture triangle(bool with_normals = false) {
    Fixture result;
    result.attribute("POSITION", result.add(floats({0, 0, 0, 1, 0, 0, 0, 1, 0}), "VEC3", 5126, 3));
    if (with_normals)
        result.attribute("NORMAL",
                         result.add(floats({0, 0, 1, 0, 0, 1, 0, 0, 1}), "VEC3", 5126, 3));
    return result;
}
Fixture quad(bool fan = false, bool with_normals = true) {
    Fixture result;
    const auto positions = fan ? floats({0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0})
                               : floats({0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0});
    result.attribute("POSITION", result.add(positions, "VEC3", 5126, 4));
    if (with_normals)
        result.attribute("NORMAL",
                         result.add(floats({0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1}), "VEC3", 5126, 4));
    result.primitive_extra = fan ? ",\"mode\":6" : ",\"mode\":5";
    return result;
}
Result<GltfGeometry> convert(const Fixture& fixture, GltfGeometrySettings settings = {},
                             std::stop_token stop = {}) {
    const auto text = fixture.document();
    const std::array external{GltfExternalSource{path("data.bin"), asset(), bytes(fixture.binary)}};
    return convert_gltf_geometry(
        {path("model.gltf"), bytes(text),
         fixture.binary.empty() ? std::span<const GltfExternalSource>{} : std::span{external},
         stop},
        settings);
}
void rejected(const Result<GltfGeometry>& result) {
    REQUIRE_FALSE(result);
    CHECK_FALSE(result.error().message.empty());
    CHECK(result.error().context.operation == "asset.gltf.convert");
    CHECK(result.error().context.path == "model.gltf");
    CHECK(detail::gltf_live_allocations() == 0);
}
template <typename T, std::size_t Size>
void near(const std::array<T, Size>& actual, const std::array<double, Size>& expected,
          double tolerance = 1e-6) {
    for (std::size_t index = 0; index < Size; ++index) {
        INFO(index);
        CHECK(std::abs(static_cast<double>(actual[index]) - expected[index]) <= tolerance);
    }
}
void bounds(const GltfGeometryBounds& actual, math::Vec3 minimum, math::Vec3 maximum) {
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(std::abs(actual.minimum[axis] - minimum[axis]) <= 1e-8);
        CHECK(std::abs(actual.maximum[axis] - maximum[axis]) <= 1e-8);
    }
}
void normal_texture(Fixture& value, unsigned uv = 0) {
    value.root_extra =
        R"(,"images":[{"uri":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR42mP8/x8AAwMCAO+jRZkAAAAASUVORK5CYII="}],"textures":[{"source":0}],"materials":[{"normalTexture":{"index":0,"texCoord":)" +
        std::to_string(uv) + "}}]";
    value.primitive_extra += ",\"material\":0";
}
std::string fixture_file(const char* name) {
    const auto file = std::filesystem::path{SHARK_GLTF_GEOMETRY_FIXTURE_DIRECTORY} / name;
    std::ifstream input{file, std::ios::binary};
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}
struct AllocationReset {
    ~AllocationReset() {
        detail::gltf_fail_allocation(std::nullopt);
    }
};
} // namespace

TEST_CASE("glTF geometry converts the original saved shared strip with independent expectations",
          "[gltf-geometry]") {
    const auto source = fixture_file("shared-strip.gltf");
    const auto binary = fixture_file("shared-strip.bin");
    REQUIRE(binary.size() == 104);
    const std::array external{GltfExternalSource{path("shared-strip.bin"), asset(), bytes(binary)}};
    const auto result = take(convert_gltf_geometry({path("model.gltf"), bytes(source), external}));
    REQUIRE(result.meshes.size() == 1);
    REQUIRE(result.primitives.size() == 1);
    REQUIRE(result.nodes.size() == 2);
    CHECK(result.meshes[0].name == "Shared strip");
    CHECK(result.nodes[0].name == "Mirrored");
    CHECK(result.nodes[1].name == "Offset");
    CHECK(result.nodes[0].mesh == 0);
    CHECK(result.nodes[1].mesh == 0);
    CHECK(result.nodes[0].mirrored);
    CHECK_FALSE(result.nodes[1].mirrored);
    CHECK((result.primitives[0].indices == std::vector<std::uint32_t>{0, 1, 2, 2, 1, 3}));
    CHECK(result.primitives[0].vertices.size() == 4);
    bounds(result.primitives[0].bounds, {0, 0, 0}, {1, 1, 0});
    REQUIRE(result.nodes[0].world_bounds);
    bounds(*result.nodes[0].world_bounds, {-2, 0, 0}, {0, 3, 0});
    REQUIRE(result.world_bounds);
    bounds(*result.world_bounds, {-2, 0, 0}, {5, 3, 0});
    REQUIRE(result.source.resources.size() == 1);
    CHECK(result.source.resources[0].asset == asset());
}

TEST_CASE("glTF geometry embedded JSON and GLB produce independently expected owned geometry",
          "[gltf-geometry]") {
    for (const auto* file : {"embedded-strip.gltf", "shared-strip.glb"}) {
        const auto source = fixture_file(file);
        const auto converted = take(convert_gltf_geometry({path(file), bytes(source), {}}));
        REQUIRE(converted.primitives.size() == 1);
        REQUIRE(converted.nodes.size() == 2);
        const auto& primitive = converted.primitives[0];
        REQUIRE(primitive.vertices.size() == 4);
        CHECK((primitive.indices == std::vector<std::uint32_t>{0, 1, 2, 2, 1, 3}));
        near(primitive.vertices[0].position, std::array{0.0, 0.0, 0.0});
        near(primitive.vertices[3].position, std::array{1.0, 1.0, 0.0});
        near(primitive.vertices[3].normal, std::array{0.0, 0.0, 1.0});
        REQUIRE(converted.world_bounds);
        bounds(*converted.world_bounds, {-2, 0, 0}, {5, 3, 0});
        REQUIRE(converted.source.resources.size() == 1);
        CHECK_FALSE(converted.source.resources[0].asset);
        CHECK_FALSE(converted.source.resources[0].path);
        CHECK(converted.source.resources[0].byte_count == 104);
        CHECK(converted.source.container == (std::string_view{file}.ends_with(".glb")
                                                 ? GltfContainer::binary
                                                 : GltfContainer::json));
    }
}

TEST_CASE("glTF geometry triangle owns positions flat normals defaults and tight bounds",
          "[gltf-geometry]") {
    const auto result = take(convert(triangle()));
    REQUIRE(result.primitives.size() == 1);
    const auto& primitive = result.primitives[0];
    REQUIRE(primitive.vertices.size() == 3);
    CHECK((primitive.indices == std::vector<std::uint32_t>{0, 1, 2}));
    near(primitive.vertices[0].position, std::array{0.0, 0.0, 0.0});
    near(primitive.vertices[1].position, std::array{1.0, 0.0, 0.0});
    near(primitive.vertices[2].position, std::array{0.0, 1.0, 0.0});
    for (const auto& vertex : primitive.vertices) {
        near(vertex.normal, std::array{0.0, 0.0, 1.0});
        near(vertex.color, std::array{1.0, 1.0, 1.0, 1.0});
    }
    CHECK(primitive.generated_normals);
    CHECK_FALSE(primitive.generated_tangents);
    CHECK_FALSE(primitive.has_tangents);
    CHECK_FALSE(primitive.has_texcoord0);
    CHECK_FALSE(primitive.has_color);
    bounds(primitive.bounds, {0, 0, 0}, {1, 1, 0});
}

TEST_CASE("glTF geometry retained values survive destruction of caller snapshots",
          "[gltf-geometry]") {
    auto result = [&] {
        auto input = triangle(true);
        return take(convert(input));
    }();
    REQUIRE(result.primitives[0].vertices.size() == 3);
    near(result.primitives[0].vertices[1].position, std::array{1.0, 0.0, 0.0});
    CHECK(result.source.resources[0].byte_count == 72);
    CHECK_FALSE(result.source.resources[0].digest.to_hex().empty());
}

TEST_CASE("glTF geometry explicit normals retain indexed shared vertices", "[gltf-geometry]") {
    auto input = quad();
    const auto indices = input.add(integers({0, 1, 2, 2, 1, 3}, 2), "SCALAR", 5123, 6);
    input.primitive_extra = ",\"indices\":" + std::to_string(indices);
    const auto result = take(convert(input));
    const auto& primitive = result.primitives[0];
    CHECK(primitive.vertices.size() == 4);
    CHECK((primitive.indices == std::vector<std::uint32_t>{0, 1, 2, 2, 1, 3}));
    CHECK_FALSE(primitive.generated_normals);
}

TEST_CASE("glTF geometry strip parity and fan expansion have independently known winding",
          "[gltf-geometry]") {
    const auto strip = take(convert(quad()));
    CHECK((strip.primitives[0].indices == std::vector<std::uint32_t>{0, 1, 2, 2, 1, 3}));
    const auto fan = take(convert(quad(true)));
    CHECK((fan.primitives[0].indices == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3}));
}

TEST_CASE("glTF geometry flat normals split shared corners at a real crease", "[gltf-geometry]") {
    Fixture input;
    input.attribute("POSITION",
                    input.add(floats({0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1}), "VEC3", 5126, 4));
    const auto indices = input.add(integers({0, 1, 2, 0, 3, 1}, 1), "SCALAR", 5121, 6);
    input.primitive_extra = ",\"indices\":" + std::to_string(indices);
    const auto result = take(convert(input));
    const auto& primitive = result.primitives[0];
    REQUIRE(primitive.vertices.size() == 6);
    CHECK((primitive.indices == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5}));
    for (std::size_t i = 0; i < 3; ++i)
        near(primitive.vertices[i].normal, std::array{0.0, 0.0, 1.0});
    for (std::size_t i = 3; i < 6; ++i)
        near(primitive.vertices[i].normal, std::array{0.0, 1.0, 0.0});
}

TEST_CASE("glTF geometry interleaved accessors respect stride and padding bytes",
          "[gltf-geometry]") {
    Fixture input;
    input.attribute("POSITION", input.add(floats({0, 0, 0, 99, 1, 0, 0, 99, 0, 1, 0, 99}), "VEC3",
                                          5126, 3, false, 16));
    const auto result = take(convert(input));
    near(result.primitives[0].vertices[1].position, std::array{1.0, 0.0, 0.0});
    near(result.primitives[0].vertices[2].position, std::array{0.0, 1.0, 0.0});
}

TEST_CASE("glTF geometry normalized unsigned UV and color values decode exactly",
          "[gltf-geometry]") {
    auto input = triangle(true);
    input.attribute("TEXCOORD_0", input.add(integers({0, 0, 99, 99, 255, 0, 99, 99, 0, 255}, 1),
                                            "VEC2", 5121, 3, true, 4));
    input.attribute("TEXCOORD_1",
                    input.add(integers({0, 0, 65535, 0, 0, 32768}, 2), "VEC2", 5123, 3, true, 4));
    input.attribute("COLOR_0",
                    input.add(integers({255, 0, 128, 255, 0, 255, 0, 128, 0, 0, 255, 0}, 1), "VEC4",
                              5121, 3, true, 4));
    const auto result = take(convert(input));
    const auto& primitive = result.primitives[0];
    CHECK(primitive.has_texcoord0);
    CHECK(primitive.has_texcoord1);
    CHECK(primitive.has_color);
    near(primitive.vertices[1].texcoord0, std::array{1.0, 0.0});
    near(primitive.vertices[2].texcoord1, std::array{0.0, 32768.0 / 65535.0});
    near(primitive.vertices[0].color, std::array{1.0, 0.0, 128.0 / 255.0, 1.0});
    near(primitive.vertices[1].color, std::array{0.0, 1.0, 0.0, 128.0 / 255.0});
}

TEST_CASE("glTF geometry RGB vertex color receives linear opaque alpha", "[gltf-geometry]") {
    auto input = triangle();
    input.attribute("COLOR_0",
                    input.add(floats({0.25F, 0.5F, 1, 1, 0, 0, 0, 1, 0}), "VEC3", 5126, 3));
    const auto result = take(convert(input));
    near(result.primitives[0].vertices[0].color, std::array{0.25, 0.5, 1.0, 1.0});
}

TEST_CASE("glTF geometry sparse positions replace an implicit zero base", "[gltf-geometry]") {
    Fixture input;
    const auto indices = input.view(integers({1, 2}, 1));
    const auto values = input.view(floats({1, 0, 0, 0, 1, 0}));
    input.accessors.push_back(
        "{\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"sparse\":{\"count\":2,\"indices\":"
        "{\"bufferView\":" +
        std::to_string(indices) +
        ",\"componentType\":5121},\"values\":{\"bufferView\":" + std::to_string(values) + "}}}");
    input.attribute("POSITION", 0);
    const auto result = take(convert(input));
    near(result.primitives[0].vertices[0].position, std::array{0.0, 0.0, 0.0});
    near(result.primitives[0].vertices[1].position, std::array{1.0, 0.0, 0.0});
    near(result.primitives[0].vertices[2].position, std::array{0.0, 1.0, 0.0});
}

TEST_CASE("glTF geometry effective sparse overrides repair invalid base positions and indices",
          "[gltf-geometry]") {
    auto input = triangle(true);
    const auto invalid_position =
        std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
    for (unsigned i = 0; i < 4; ++i)
        input.binary[12 + i] = static_cast<char>((invalid_position >> (8U * i)) & 255U);
    const auto position_index = input.view(integers({1}, 1));
    const auto position_value = input.view(floats({1, 0, 0}));
    input.accessors[0].pop_back();
    input.accessors[0] +=
        ",\"sparse\":{\"count\":1,\"indices\":{\"bufferView\":" + std::to_string(position_index) +
        ",\"componentType\":5121},\"values\":{\"bufferView\":" + std::to_string(position_value) +
        "}}}";
    const auto indices_accessor = input.add(integers({0, 255, 2}, 1), "SCALAR", 5121, 3);
    const auto sparse_index = input.view(integers({1}, 1));
    const auto sparse_value = input.view(integers({1}, 1));
    input.accessors[indices_accessor].pop_back();
    input.accessors[indices_accessor] +=
        ",\"sparse\":{\"count\":1,\"indices\":{\"bufferView\":" + std::to_string(sparse_index) +
        ",\"componentType\":5121},\"values\":{\"bufferView\":" + std::to_string(sparse_value) +
        "}}}";
    input.primitive_extra = ",\"indices\":" + std::to_string(indices_accessor);
    const auto result = take(convert(input));
    CHECK((result.primitives[0].indices == std::vector<std::uint32_t>{0, 1, 2}));
    near(result.primitives[0].vertices[1].position, std::array{1.0, 0.0, 0.0});
}

TEST_CASE("glTF geometry sparse normalized UV replacements use the same component conversion",
          "[gltf-geometry]") {
    auto input = triangle(true);
    const auto sparse_index = input.view(integers({1, 2}, 1));
    const auto sparse_values = input.view(integers({255, 0, 0, 255}, 1));
    input.accessors.push_back(
        "{\"componentType\":5121,\"count\":3,\"type\":\"VEC2\",\"normalized\":true,\"sparse\":{"
        "\"count\":2,\"indices\":{\"bufferView\":" +
        std::to_string(sparse_index) + ",\"componentType\":5121},\"values\":{\"bufferView\":" +
        std::to_string(sparse_values) + "}}}");
    input.attribute("TEXCOORD_0", input.accessors.size() - 1);
    const auto result = take(convert(input));
    near(result.primitives[0].vertices[0].texcoord0, std::array{0.0, 0.0});
    near(result.primitives[0].vertices[1].texcoord0, std::array{1.0, 0.0});
    near(result.primitives[0].vertices[2].texcoord0, std::array{0.0, 1.0});
}

TEST_CASE("glTF geometry generated normal-map tangents use the selected UV0 basis",
          "[gltf-geometry]") {
    auto input = triangle(true);
    input.attribute("TEXCOORD_0", input.add(floats({0, 0, 1, 0, 0, 1}), "VEC2", 5126, 3));
    normal_texture(input);
    const auto result = take(convert(input));
    const auto& primitive = result.primitives[0];
    CHECK(primitive.has_tangents);
    CHECK(primitive.generated_tangents);
    for (const auto& vertex : primitive.vertices)
        near(vertex.tangent, std::array{1.0, 0.0, 0.0, 1.0});
}

TEST_CASE("glTF geometry normal-map UV1 and mirrored UVs control tangent direction and sign",
          "[gltf-geometry]") {
    auto input = triangle(true);
    input.attribute("TEXCOORD_0", input.add(floats({0, 0, 1, 0, 0, 1}), "VEC2", 5126, 3));
    input.attribute("TEXCOORD_1", input.add(floats({0, 0, 0, 1, 1, 0}), "VEC2", 5126, 3));
    normal_texture(input, 1);
    const auto result = take(convert(input));
    for (const auto& vertex : result.primitives[0].vertices)
        near(vertex.tangent, std::array{0.0, 1.0, 0.0, -1.0});
}

TEST_CASE("glTF geometry provided tangents preserve handedness with explicit normals",
          "[gltf-geometry]") {
    auto input = triangle(true);
    input.attribute("TANGENT",
                    input.add(floats({1, 0, 0, -1, 1, 0, 0, -1, 1, 0, 0, -1}), "VEC4", 5126, 3));
    const auto result = take(convert(input));
    CHECK(result.primitives[0].has_tangents);
    CHECK_FALSE(result.primitives[0].generated_tangents);
    near(result.primitives[0].vertices[0].tangent, std::array{1.0, 0.0, 0.0, -1.0});
}

TEST_CASE("glTF geometry missing normals discard provided tangents before generating a basis",
          "[gltf-geometry]") {
    auto input = triangle();
    input.attribute("TANGENT",
                    input.add(floats({0, 1, 0, -1, 0, 1, 0, -1, 0, 1, 0, -1}), "VEC4", 5126, 3));
    input.attribute("TEXCOORD_0", input.add(floats({0, 0, 1, 0, 0, 1}), "VEC2", 5126, 3));
    normal_texture(input);
    const auto result = take(convert(input));
    CHECK(result.primitives[0].generated_normals);
    CHECK(result.primitives[0].generated_tangents);
    near(result.primitives[0].vertices[0].tangent, std::array{1.0, 0.0, 0.0, 1.0});
}

TEST_CASE("glTF geometry constant UVs generate a finite deterministic fallback tangent",
          "[gltf-geometry]") {
    auto input = triangle(true);
    input.attribute("TEXCOORD_0", input.add(floats({0, 0, 0, 0, 0, 0}), "VEC2", 5126, 3));
    normal_texture(input);
    const auto first = take(convert(input));
    const auto second = take(convert(input));
    CHECK_FALSE(first.source.diagnostics.empty());
    REQUIRE(first.primitives[0].vertices.size() == second.primitives[0].vertices.size());
    for (std::size_t i = 0; i < first.primitives[0].vertices.size(); ++i) {
        const auto& tangent = first.primitives[0].vertices[i].tangent;
        CHECK(tangent == second.primitives[0].vertices[i].tangent);
        CHECK(std::isfinite(tangent[0]));
        CHECK(std::abs(tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2] -
                       1.0F) < 1e-6F);
        CHECK(std::abs(tangent[2]) < 1e-6F);
    }
}

TEST_CASE("glTF geometry material core texture uses require their UV sets", "[gltf-geometry]") {
    for (const auto* field : {"baseColorTexture", "metallicRoughnessTexture", "normalTexture",
                              "occlusionTexture", "emissiveTexture"}) {
        auto input = triangle(true);
        normal_texture(input, 1);
        auto& text = input.root_extra;
        const auto start = text.find("\"normalTexture\"");
        REQUIRE(start != std::string::npos);
        const auto replacement = std::string{"\""} + field + '"';
        text.replace(start, std::string{"\"normalTexture\""}.size(), replacement);
        if (std::string_view{field} == "baseColorTexture" ||
            std::string_view{field} == "metallicRoughnessTexture") {
            const auto material = text.find("\"materials\":[{");
            REQUIRE(material != std::string::npos);
            text.insert(material + std::string{"\"materials\":[{"}.size(),
                        "\"pbrMetallicRoughness\":{");
            text.insert(text.size() - 1, "}");
        }
        INFO(field);
        rejected(convert(input));
    }
}

TEST_CASE("glTF geometry normal texture coordinate sets above one reject", "[gltf-geometry]") {
    auto input = triangle(true);
    normal_texture(input, 2);
    rejected(convert(input));
}

TEST_CASE("glTF geometry floating vertex NaN infinity and color range violations reject",
          "[gltf-geometry]") {
    for (const auto value :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()}) {
        auto input = triangle();
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned i = 0; i < 4; ++i)
            input.binary[i] = static_cast<char>((bits >> (8U * i)) & 255U);
        rejected(convert(input));
    }
    auto color = triangle(true);
    color.attribute("COLOR_0", color.add(floats({-0.1F, 0, 1, 0, 1, 0, 0, 0, 1}), "VEC3", 5126, 3));
    rejected(convert(color));
}

TEST_CASE("glTF geometry malformed normal tangent and attribute storage domains reject",
          "[gltf-geometry]") {
    for (const auto* type : {"VEC2", "VEC4"}) {
        auto input = triangle();
        input.attribute("NORMAL",
                        input.add(floats({0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1}), type, 5126, 3));
        rejected(convert(input));
    }
    for (const auto& values :
         {floats({0, 0, 0, 0, 0, 1, 0, 0, 1}), floats({0, 0, 2, 0, 0, 1, 0, 0, 1})}) {
        auto input = triangle();
        input.attribute("NORMAL", input.add(values, "VEC3", 5126, 3));
        rejected(convert(input));
    }
    auto tangent = triangle(true);
    tangent.attribute("TANGENT",
                      tangent.add(floats({1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1}), "VEC4", 5126, 3));
    rejected(convert(tangent));
    auto uv = triangle();
    uv.attribute("TEXCOORD_0", uv.add(integers({0, 0, 255, 0, 0, 255}, 1), "VEC2", 5121, 3));
    rejected(convert(uv));
}

TEST_CASE("glTF geometry unit tolerance canonicalizes small source rounding", "[gltf-geometry]") {
    auto input = triangle();
    input.attribute("NORMAL", input.add(floats({0, 0, 1.00001F, 0, 0, 1.00001F, 0, 0, 1.00001F}),
                                        "VEC3", 5126, 3));
    const auto result = take(convert(input));
    near(result.primitives[0].vertices[0].normal, std::array{0.0, 0.0, 1.0});
}

TEST_CASE("glTF geometry triangle topology count and unsupported mode errors remain explicit",
          "[gltf-geometry]") {
    auto count = quad();
    count.primitive_extra.clear();
    rejected(convert(count));
    for (const auto mode : {0, 1, 2, 3, 7}) {
        auto input = triangle();
        input.primitive_extra = ",\"mode\":" + std::to_string(mode);
        rejected(convert(input));
    }
}

TEST_CASE("glTF geometry reserved maximum unsigned indices reject even within vertex count",
          "[gltf-geometry]") {
    Fixture input;
    const auto values = input.view(integers({0, 1, 255}, 1));
    input.accessors.push_back("{\"componentType\":5126,\"count\":256,\"type\":\"VEC3\"}");
    input.accessors.push_back("{\"bufferView\":" + std::to_string(values) +
                              ",\"componentType\":5121,\"count\":3,\"type\":\"SCALAR\"}");
    input.attribute("POSITION", 0);
    input.primitive_extra = ",\"indices\":1";
    rejected(convert(input));
}

TEST_CASE("glTF geometry degenerates are removed without changing strip parity",
          "[gltf-geometry]") {
    auto input = quad();
    const auto accessor = input.add(integers({0, 0, 1, 2, 3}, 1), "SCALAR", 5121, 5);
    input.primitive_extra += ",\"indices\":" + std::to_string(accessor);
    const auto result = take(convert(input));
    CHECK((result.primitives[0].indices == std::vector<std::uint32_t>{1, 0, 2, 1, 2, 3}));
    CHECK_FALSE(result.source.diagnostics.empty());
    auto all = triangle();
    all.binary = floats({0, 0, 0, 1, 0, 0, 2, 0, 0});
    rejected(convert(all));
}

TEST_CASE("glTF geometry selected scenes exclude loose nodes and share reachable meshes",
          "[gltf-geometry]") {
    auto input = triangle(true);
    input.nodes =
        R"([{"mesh":0,"translation":[1,0,0]},{"mesh":0,"translation":[7,0,0]},{"mesh":0}])";
    input.scenes = R"([{"nodes":[0]},{"nodes":[1]}])";
    const auto first = take(convert(input));
    const auto second = take(convert(input, {{1}}));
    REQUIRE(first.nodes.size() == 1);
    REQUIRE(second.nodes.size() == 1);
    CHECK(first.nodes[0].source_node == 0);
    CHECK(second.nodes[0].source_node == 1);
    CHECK(second.selected_scene == 1);
    bounds(*second.world_bounds, {7, 0, 0}, {8, 1, 0});
    rejected(convert(input, {{2}}));
}

TEST_CASE("glTF geometry default scene fallback and no-scene conversion are deterministic",
          "[gltf-geometry]") {
    auto input = triangle();
    input.default_scene = false;
    CHECK(take(convert(input)).selected_scene == 0);
    input.scenes.clear();
    const auto result = take(convert(input));
    CHECK_FALSE(result.selected_scene);
    CHECK(result.nodes.empty());
    CHECK(result.meshes.empty());
    CHECK(result.primitives.empty());
    CHECK_FALSE(result.world_bounds);
    rejected(convert(input, {{0}}));
}

TEST_CASE("glTF geometry traversal preserves root child order and dense parent references",
          "[gltf-geometry]") {
    auto input = triangle();
    input.nodes = R"([{"children":[3,1]},{"mesh":0},{"mesh":0},{"children":[4]},{"mesh":0}])";
    input.scenes = R"([{"nodes":[2,0]}])";
    const auto result = take(convert(input));
    REQUIRE(result.nodes.size() == 5);
    CHECK((result.roots == std::vector<std::size_t>{0, 1}));
    const std::array<std::size_t, 5> sources{2, 0, 3, 4, 1};
    for (std::size_t i = 0; i < sources.size(); ++i)
        CHECK(result.nodes[i].source_node == sources[i]);
    CHECK((result.nodes[1].children == std::vector<std::size_t>{2, 4}));
    CHECK(result.nodes[2].parent == 1);
    CHECK(result.nodes[3].parent == 2);
    CHECK(result.nodes[4].parent == 1);
    CHECK(result.meshes.size() == 1);
}

TEST_CASE("glTF geometry source mesh indices remain distinct from dense shared output references",
          "[gltf-geometry]") {
    auto input = triangle(true);
    const auto primitive = input.primitive();
    input.meshes = "[{\"primitives\":[" + primitive + "]},{\"primitives\":[" + primitive +
                   "]},{\"primitives\":[" + primitive + "]}]";
    input.nodes = R"([{"mesh":2},{"mesh":1},{"mesh":2}])";
    input.scenes = R"([{"nodes":[0,1,2]}])";
    const auto result = take(convert(input));
    REQUIRE(result.meshes.size() == 2);
    CHECK(result.meshes[0].source_mesh == 1);
    CHECK(result.meshes[1].source_mesh == 2);
    CHECK(result.nodes[0].mesh == 1);
    CHECK(result.nodes[1].mesh == 0);
    CHECK(result.nodes[2].mesh == 1);
    CHECK(result.primitives.size() == 2);
}

TEST_CASE("glTF geometry column-major matrix transforms preserve negative nonuniform scale",
          "[gltf-geometry]") {
    auto input = triangle(true);
    input.nodes = R"([{"mesh":0,"matrix":[-2,0,0,0,0,3,0,0,0,0,4,0,5,6,7,1]}])";
    const auto result = take(convert(input));
    const auto& node = result.nodes[0];
    CHECK(node.mirrored);
    CHECK(node.local.translation == math::Vec3(5, 6, 7));
    CHECK(node.local.scale == math::Vec3(-2, 3, 4));
    CHECK(node.world_matrix[3] == glm::dvec4(5, 6, 7, 1));
    near(node.normal_matrix, std::array{-0.5, 0.0, 0.0, 0.0, 1.0 / 3.0, 0.0, 0.0, 0.0, 0.25});
    bounds(*node.world_bounds, {3, 6, 7}, {5, 9, 7});
}

TEST_CASE("glTF geometry TRS quaternion input follows shared canonicalization", "[gltf-geometry]") {
    auto input = triangle(true);
    input.nodes = R"([{"mesh":0,"translation":[2,3,4],"rotation":[0,0,2,2],"scale":[2,3,4]}])";
    const auto result = take(convert(input));
    CHECK(std::abs(result.nodes[0].local.rotation.z - std::sqrt(0.5)) < 1e-12);
    CHECK(std::abs(result.nodes[0].local.rotation.w - std::sqrt(0.5)) < 1e-12);
    bounds(*result.world_bounds, {-1, 3, 4}, {2, 5, 4});
}

TEST_CASE("glTF geometry world shear retains a full matrix and inverse-transpose normals",
          "[gltf-geometry]") {
    auto input = triangle(true);
    input.nodes =
        R"([{"scale":[2,1,1],"children":[1]},{"mesh":0,"rotation":[0,0,0.3826834323650898,0.9238795325112867]}])";
    const auto result = take(convert(input));
    REQUIRE(result.nodes.size() == 2);
    const auto root_half = std::sqrt(0.5);
    const auto& node = result.nodes[1];
    CHECK(std::abs(node.world_matrix[0][0] - 2 * root_half) < 1e-9);
    CHECK(std::abs(node.world_matrix[0][1] - root_half) < 1e-9);
    CHECK(std::abs(node.world_matrix[1][0] + 2 * root_half) < 1e-9);
    near(node.normal_matrix,
         std::array{root_half / 2, root_half, 0.0, -root_half / 2, root_half, 0.0, 0.0, 0.0, 1.0});
    bounds(*node.world_bounds, {-2 * root_half, 0, 0}, {2 * root_half, 2 * root_half, 0});
}

TEST_CASE("glTF geometry mirror metadata never mutates shared winding or tangent handedness",
          "[gltf-geometry]") {
    auto input = triangle(true);
    input.attribute("TANGENT",
                    input.add(floats({1, 0, 0, -1, 1, 0, 0, -1, 1, 0, 0, -1}), "VEC4", 5126, 3));
    input.nodes = R"([{"mesh":0,"scale":[-1,1,1]},{"mesh":0},{"mesh":0,"scale":[-1,-1,1]}])";
    input.scenes = R"([{"nodes":[0,1,2]}])";
    const auto result = take(convert(input));
    REQUIRE(result.primitives.size() == 1);
    CHECK(result.nodes[0].mirrored);
    CHECK_FALSE(result.nodes[1].mirrored);
    CHECK_FALSE(result.nodes[2].mirrored);
    CHECK((result.primitives[0].indices == std::vector<std::uint32_t>{0, 1, 2}));
    CHECK(result.primitives[0].vertices[0].tangent[3] == -1);
}

TEST_CASE("glTF geometry local singular shear perspective and zero quaternion transforms reject",
          "[gltf-geometry]") {
    for (const auto* node :
         {R"({"mesh":0,"scale":[0,1,1]})", R"({"mesh":0,"scale":[0.0000001,1,1]})",
          R"({"mesh":0,"rotation":[0,0,0,0]})",
          R"({"mesh":0,"matrix":[1,0,0,0,0.5,1,0,0,0,0,1,0,0,0,0,1]})",
          R"({"mesh":0,"matrix":[1,0,0,0.1,0,1,0,0,0,0,1,0,0,0,0,1]})"}) {
        auto input = triangle();
        input.nodes = '[' + std::string{node} + ']';
        rejected(convert(input));
    }
}

TEST_CASE("glTF geometry hierarchical overflow and near-singular composed bases reject",
          "[gltf-geometry]") {
    auto input = triangle();
    input.nodes.clear();
    input.nodes += '[';
    for (std::size_t i = 0; i < 12; ++i) {
        if (i != 0)
            input.nodes += ',';
        input.nodes += "{\"scale\":[1e30,1e30,1e30]";
        input.nodes += i == 11 ? ",\"mesh\":0}" : ",\"children\":[" + std::to_string(i + 1) + "]}";
    }
    input.nodes += ']';
    rejected(convert(input));
}

TEST_CASE("glTF geometry iterative hierarchy handles a deep valid chain without recursion",
          "[gltf-geometry]") {
    auto input = triangle();
    input.nodes = '[';
    constexpr std::size_t depth = 512;
    for (std::size_t i = 0; i < depth; ++i) {
        if (i != 0)
            input.nodes += ',';
        input.nodes += i + 1 == depth ? "{\"mesh\":0}"
                                      : "{\"translation\":[1,0,0],\"children\":[" +
                                            std::to_string(i + 1) + "]}";
    }
    input.nodes += ']';
    const auto result = take(convert(input));
    REQUIRE(result.nodes.size() == depth);
    CHECK(result.nodes.back().world_matrix[3][0] == static_cast<double>(depth - 1));
    bounds(*result.world_bounds, {511, 0, 0}, {512, 1, 0});
}

TEST_CASE("glTF geometry cancellation is an owned failure without parser allocations",
          "[gltf-geometry]") {
    std::stop_source cancellation;
    cancellation.request_stop();
    const auto input = triangle();
    rejected(convert(input, {}, cancellation.get_token()));
    CHECK(detail::gltf_live_allocations() == 0);
}

TEST_CASE("glTF geometry every parser allocation failure is contained and fully released",
          "[gltf-geometry]") {
    AllocationReset reset;
    const auto input = triangle(true);
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
    CHECK(detail::gltf_live_allocations() == 0);
}

TEST_CASE("glTF geometry repeated primitive references cannot amplify converted vertices beyond "
          "their bound",
          "[gltf-geometry]") {
    Fixture input;
    std::string positions;
    std::string normals;
    for (std::size_t i = 0; i < 32; ++i) {
        positions += floats({0, 0, 0, 1, 0, 0, 0, 1, 0});
        normals += floats({0, 0, 1, 0, 0, 1, 0, 0, 1});
    }
    input.attribute("POSITION", input.add(positions, "VEC3", 5126, 96));
    input.attribute("NORMAL", input.add(normals, "VEC3", 5126, 96));
    std::string many;
    constexpr std::size_t count = 4096;
    const auto primitive = input.primitive();
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0)
            many += ',';
        many += primitive;
    }
    input.meshes = "[{\"primitives\":[" + many + "]}]";
    const auto result = convert(input);
    rejected(result);
    CHECK(result.error().message.find("geometry") != std::string::npos);
    CHECK(result.error().message.find("bound") != std::string::npos);
}

TEST_CASE("glTF geometry repeated instances retain one mesh and bounded independent world metadata",
          "[gltf-geometry]") {
    auto input = triangle(true);
    input.nodes = '[';
    std::string roots;
    constexpr std::size_t count = 256;
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) {
            input.nodes += ',';
            roots += ',';
        }
        input.nodes += "{\"mesh\":0,\"translation\":[" + std::to_string(i) + ",0,0]}";
        roots += std::to_string(i);
    }
    input.nodes += ']';
    input.scenes = "[{\"nodes\":[" + roots + "]}]";
    const auto result = take(convert(input));
    REQUIRE(result.nodes.size() == count);
    CHECK(result.meshes.size() == 1);
    CHECK(result.primitives.size() == 1);
    CHECK(result.primitives[0].vertices.size() == 3);
    bounds(*result.world_bounds, {0, 0, 0}, {256, 1, 0});
}

TEST_CASE("glTF geometry repeated accessor decoding has an independent aggregate work bound",
          "[gltf-geometry]") {
    Fixture input;
    auto positions = floats({0, 0, 0, 1, 0, 0, 0, 1, 0});
    positions.resize(16384 * 12, '\0');
    input.attribute("POSITION", input.add(positions, "VEC3", 5126, 16384));
    const auto indices = input.add(integers({0, 1, 2}, 2), "SCALAR", 5123, 3);
    input.primitive_extra = ",\"indices\":" + std::to_string(indices);
    const auto primitive = input.primitive();
    std::vector<std::string> repeated(256, primitive);
    input.meshes = "[{\"primitives\":[" + join(repeated) + "]}]";
    const auto result = convert(input);
    rejected(result);
    CHECK(result.error().message.find("work bound") != std::string::npos);
}

TEST_CASE("glTF geometry buffer target and normalized index usage must match their semantics",
          "[gltf-geometry]") {
    auto vertex = triangle();
    vertex.views[0].insert(vertex.views[0].size() - 1, ",\"target\":34963");
    rejected(convert(vertex));
    auto index_target = triangle();
    const auto index = index_target.add(integers({0, 1, 2}, 2), "SCALAR", 5123, 3);
    index_target.views.back().insert(index_target.views.back().size() - 1, ",\"target\":34962");
    index_target.primitive_extra = ",\"indices\":" + std::to_string(index);
    rejected(convert(index_target));
    auto normalized = triangle();
    const auto normalized_index = normalized.add(integers({0, 1, 2}, 2), "SCALAR", 5123, 3, true);
    normalized.primitive_extra = ",\"indices\":" + std::to_string(normalized_index);
    rejected(convert(normalized));
    auto valid = triangle();
    valid.views[0].insert(valid.views[0].size() - 1, ",\"target\":34962");
    const auto valid_index = valid.add(integers({0, 1, 2}, 2), "SCALAR", 5123, 3);
    valid.views.back().insert(valid.views.back().size() - 1, ",\"target\":34963");
    valid.primitive_extra = ",\"indices\":" + std::to_string(valid_index);
    REQUIRE(convert(valid));
}

TEST_CASE("glTF geometry source tangent signs must agree across one face", "[gltf-geometry]") {
    auto input = triangle(true);
    input.attribute("TANGENT",
                    input.add(floats({1, 0, 0, 1, 1, 0, 0, -1, 1, 0, 0, 1}), "VEC4", 5126, 3));
    rejected(convert(input));
}

TEST_CASE("glTF geometry retains inherited root source and external path rejection",
          "[gltf-geometry]") {
    const auto text =
        R"({"asset":{"version":"2.0"},"buffers":[{"uri":"../escape.bin","byteLength":4}]})";
    const auto result = convert_gltf_geometry({path("model.gltf"), bytes(text), {}});
    rejected(result);
    auto input = triangle();
    input.root_extra = R"(,"extensionsRequired":["KHR_draco_mesh_compression"] )";
    rejected(convert(input));
    input.root_extra = R"(,"animations":[{}])";
    rejected(convert(input));
}
