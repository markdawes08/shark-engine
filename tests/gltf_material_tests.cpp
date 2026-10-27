// Original Shark-authored offline cases. Embedded inputs are from the existing
// MIT fixtures under tests/fixtures/gltf_textures; no models are downloaded.
#include "shark/tools/gltf_geometry.hpp"
#include "shark/tools/gltf_materials.hpp"

#include "gltf_image_test_port.hpp"
#include "gltf_source_test_port.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

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
ProjectPath path(std::string_view text) {
    return take(ProjectPath::parse(text));
}
std::span<const std::byte> bytes(std::string_view text) {
    return std::as_bytes(std::span<const char>{text.data(), text.size()});
}
std::string document(std::string_view material = "{}") {
    return std::string{
               R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":60,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/"}],"bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}],"images":[{"uri":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAE0lEQVR42mNgAIL/QACiHYB0AwA4mwe6OCAfGQAAAABJRU5ErkJggg=="}],"textures":[{"source":0}],"materials":[)"} +
           std::string{material} +
           R"(],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1,"TEXCOORD_1":1},"material":0}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
}
void replace(std::string& text, std::string_view before, std::string_view after) {
    const auto position = text.find(before);
    REQUIRE(position != std::string::npos);
    text.replace(position, before.size(), after);
}
std::string fallback(std::string_view extension = "KHR_texture_basisu") {
    auto text = document(R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}})");
    replace(text, R"("textures":[{"source":0}])",
            "\"textures\":[{\"source\":0,\"extensions\":{\"" + std::string{extension} +
                "\":{\"source\":1}}}]");
    replace(text, R"("materials":)",
            "\"extensionsUsed\":[\"" + std::string{extension} + "\"],\"materials\":");
    replace(text, R"(}],"textures":)",
            R"(},{"uri":"alternative.ktx2","mimeType":"image/ktx2"}],"textures":)");
    return text;
}
Result<GltfMaterials> convert(std::string_view text, GltfMaterialSettings settings = {},
                              std::stop_token stop = {}) {
    return convert_gltf_materials({path("model.gltf"), bytes(text), {}, stop}, settings);
}
void rejected(const Result<GltfMaterials>& result) {
    REQUIRE_FALSE(result);
    CHECK(result.error().context.operation == "asset.gltf.materials");
    CHECK(result.error().context.path == "model.gltf");
    CHECK_FALSE(result.error().message.empty());
    CHECK(detail::gltf_live_allocations() == 0);
    CHECK(detail::gltf_image_live_allocations() == 0);
}
} // namespace

TEST_CASE("glTF core material defaults are explicit without decoding images", "[gltf-materials]") {
    detail::gltf_image_fail_allocation(std::nullopt);
    const auto output = take(convert(document()));
    REQUIRE(output.materials.size() == 1);
    CHECK(output.selected_scene == 0);
    const auto& material = output.materials.front();
    CHECK(material.source_material == 0);
    CHECK(material.name.empty());
    CHECK(material.base_color == std::array<float, 4>{1, 1, 1, 1});
    CHECK(material.emissive == std::array<float, 3>{0, 0, 0});
    CHECK(material.metallic == 1);
    CHECK(material.roughness == 1);
    CHECK(material.normal_scale == 1);
    CHECK(material.occlusion_strength == 1);
    CHECK(material.alpha_mode == GltfAlphaMode::opaque);
    CHECK(material.alpha_cutoff == 0.5F);
    CHECK_FALSE(material.double_sided);
    for (const auto& texture : material.textures)
        CHECK_FALSE(texture);
    CHECK(detail::gltf_image_allocation_attempts() == 0);
}

TEST_CASE("glTF material factors and all role UV links survive conversion", "[gltf-materials]") {
    auto text = document(
        R"({"name":"paint","pbrMetallicRoughness":{"baseColorFactor":[0.25,0.5,0.75,1],"metallicFactor":0.125,"roughnessFactor":0.875,"baseColorTexture":{"index":0,"texCoord":1},"metallicRoughnessTexture":{"index":0}},"emissiveFactor":[0.25,0.5,1],"normalTexture":{"index":0,"scale":-2},"occlusionTexture":{"index":0,"strength":0.25},"emissiveTexture":{"index":0,"texCoord":1},"alphaMode":"MASK","alphaCutoff":2,"doubleSided":true})");
    const auto materials = take(convert(text));
    const auto textures = take(convert_gltf_textures({path("model.gltf"), bytes(text)}));
    const auto& value = materials.materials.at(0);
    CHECK(value.name == "paint");
    CHECK(value.base_color == std::array<float, 4>{0.25F, 0.5F, 0.75F, 1});
    CHECK(value.emissive == std::array<float, 3>{0.25F, 0.5F, 1});
    CHECK(value.metallic == 0.125F);
    CHECK(value.roughness == 0.875F);
    CHECK(value.normal_scale == -2);
    CHECK(value.occlusion_strength == 0.25F);
    CHECK(value.alpha_mode == GltfAlphaMode::mask);
    CHECK(value.alpha_cutoff == 2);
    CHECK(value.double_sided);
    REQUIRE(textures.usages.size() == 5);
    for (std::size_t i = 0; i < value.textures.size(); ++i) {
        REQUIRE(value.textures[i]);
        CHECK(value.textures[i]->source_texture == 0);
        CHECK(value.textures[i]->texcoord == ((i == 0 || i == 4) ? 1 : 0));
        CHECK(textures.usages[i].source_material == value.source_material);
        CHECK(static_cast<std::size_t>(textures.usages[i].role) == i);
        CHECK(textures.usages[i].texcoord == value.textures[i]->texcoord);
    }
}

TEST_CASE("glTF preserves each alpha mode and cutoff even outside MASK", "[gltf-materials]") {
    const std::array names{"OPAQUE", "MASK", "BLEND"};
    const std::array values{GltfAlphaMode::opaque, GltfAlphaMode::mask, GltfAlphaMode::blend};
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto value = take(convert(
            document(std::string{"{\"alphaMode\":\""} + names[i] + "\",\"alphaCutoff\":1.5}")));
        CHECK(value.materials[0].alpha_mode == values[i]);
        CHECK(value.materials[0].alpha_cutoff == 1.5F);
    }
}

TEST_CASE("glTF rejects malformed or out-of-range material fields", "[gltf-materials]") {
    const std::array materials{R"({"pbrMetallicRoughness":[]})",
                               R"({"pbrMetallicRoughness":{"baseColorFactor":[1,1,1]}})",
                               R"({"pbrMetallicRoughness":{"baseColorFactor":[1,1,1,2]}})",
                               R"({"pbrMetallicRoughness":{"baseColorFactor":[1,1,1,false]}})",
                               R"({"pbrMetallicRoughness":{"metallicFactor":-0.01}})",
                               R"({"pbrMetallicRoughness":{"metallicFactor":1.01}})",
                               R"({"pbrMetallicRoughness":{"roughnessFactor":"1"}})",
                               R"({"pbrMetallicRoughness":{"roughnessFactor":2}})",
                               R"({"emissiveFactor":[0,0]})",
                               R"({"emissiveFactor":[0,0,1.01]})",
                               R"({"emissiveFactor":[0,-1,0]})",
                               R"({"normalTexture":{"index":0,"scale":false}})",
                               R"({"normalTexture":{"index":0,"scale":1e100}})",
                               R"({"occlusionTexture":{"index":0,"strength":-1}})",
                               R"({"occlusionTexture":{"index":0,"strength":1.01}})",
                               R"({"alphaMode":"mask"})",
                               R"({"alphaMode":1})",
                               R"({"alphaCutoff":-0.1})",
                               R"({"alphaCutoff":"0.5"})",
                               R"({"doubleSided":1})",
                               R"({"name":null})"};
    for (const auto* material : materials) {
        INFO(material);
        rejected(convert(document(material)));
    }
}

TEST_CASE("glTF implicit primitive material has no invented source record", "[gltf-materials]") {
    auto text = document();
    replace(text, R"(,"material":0)", "");
    const auto materials = take(convert(text));
    const auto geometry = take(convert_gltf_geometry({path("model.gltf"), bytes(text)}));
    CHECK(materials.materials.empty());
    REQUIRE(geometry.primitives.size() == 1);
    CHECK_FALSE(geometry.primitives[0].source_material);
}

TEST_CASE("glTF material inventory follows selected scene and source order", "[gltf-materials]") {
    auto text = document(R"({"name":"zero"},{"name":"one"},{"name":"two"})");
    replace(text, R"("material":0)", R"("material":2)");
    replace(
        text, R"("meshes":[)",
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"material":0}]},)");
    replace(text, R"("nodes":[{"mesh":0}])", R"("nodes":[{"mesh":0},{"mesh":1},{"mesh":1}])");
    replace(text, R"("scenes":[{"nodes":[0]}])", R"("scenes":[{"nodes":[1,0,2]},{"nodes":[1]}])");
    const auto all = take(convert(text));
    REQUIRE(all.materials.size() == 2);
    CHECK(all.materials[0].source_material == 0);
    CHECK(all.materials[1].source_material == 2);
    const auto selected = take(convert(text, {1}));
    REQUIRE(selected.materials.size() == 1);
    CHECK(selected.materials[0].source_material == 2);
    CHECK(selected.selected_scene == 1);
    rejected(convert(text, {2}));
}

TEST_CASE("glTF absent scenes produce no material conversion", "[gltf-materials]") {
    auto text = document();
    replace(text, R"(,"scenes":[{"nodes":[0]}],"scene":0)", "");
    const auto output = take(convert(text));
    CHECK_FALSE(output.selected_scene);
    CHECK(output.materials.empty());
    rejected(convert(text, {0}));
}

TEST_CASE("glTF material links require existing textures and available UV sets",
          "[gltf-materials]") {
    rejected(convert(document(R"({"emissiveTexture":{"index":1}})")));
    rejected(convert(document(R"({"emissiveTexture":{"index":0,"texCoord":2}})")));
    auto text = document(R"({"normalTexture":{"index":0,"texCoord":1}})");
    replace(text, R"(,"TEXCOORD_1":1)", "");
    rejected(convert(text));
}

TEST_CASE("glTF material values own their data and canonicalize negative zero",
          "[gltf-materials]") {
    auto text = document(R"({"name":"retained","pbrMetallicRoughness":{"metallicFactor":-0.0}})");
    const auto output = take(convert(text));
    text.assign(text.size(), 'x');
    CHECK(output.materials[0].name == "retained");
    CHECK(output.materials[0].metallic == 0);
}

TEST_CASE("glTF material cancellation and parser allocation failures stay in Result",
          "[gltf-materials]") {
    const auto text = document();
    std::stop_source stop;
    stop.request_stop();
    rejected(convert(text, {}, stop.get_token()));
    detail::gltf_fail_allocation(0);
    const auto result = convert(text);
    detail::gltf_fail_allocation(std::nullopt);
    rejected(result);
    CHECK(detail::gltf_live_allocations() == 0);
    REQUIRE(convert(text));
}

TEST_CASE("glTF optional compressed image alternatives retain usable core fallback",
          "[gltf-materials]") {
    for (const auto* extension : {"KHR_texture_basisu", "EXT_texture_webp", "MSFT_texture_dds"}) {
        INFO(extension);
        const auto text = fallback(extension);
        const GltfSourceInput input{path("model.gltf"), bytes(text)};
        const auto inspected = take(inspect_gltf_source(input));
        const auto materials = take(convert_gltf_materials(input));
        const auto geometry = take(convert_gltf_geometry(input));
        const auto textures = take(convert_gltf_textures(input));
        CHECK(inspected.resources.size() == 2);
        CHECK(inspected.diagnostics.size() == 2);
        REQUIRE(materials.materials.size() == 1);
        REQUIRE(materials.materials[0].textures[0]);
        CHECK(materials.materials[0].textures[0]->source_texture == 0);
        CHECK(geometry.primitives.size() == 1);
        REQUIRE(textures.images.size() == 1);
        CHECK(textures.images[0].source_image == 0);
        REQUIRE(textures.images[0].mips.size() == 2);
        CHECK(textures.images[0].mips[1].width == 1);
    }
}

TEST_CASE("glTF supplied ignored alternative is bounded but not decoded or reported as core",
          "[gltf-materials]") {
    const auto text = fallback();
    const std::string compressed = "unsupported alternative bytes";
    const auto id = AssetId{take(StableId::parse("12000000-0000-4000-8000-000000000003"))};
    const std::array mappings{GltfExternalSource{path("alternative.ktx2"), id, bytes(compressed)}};
    const auto result = take(convert_gltf_materials({path("model.gltf"), bytes(text), mappings}));
    CHECK(result.source.resources.size() == 2);
    for (const auto& resource : result.source.resources)
        CHECK_FALSE(resource.asset == id);
}

TEST_CASE("glTF optional fallback never hides core references or missing fallback",
          "[gltf-materials]") {
    auto text = fallback();
    replace(text, R"("source":0,"extensions":)", R"("source":1,"extensions":)");
    rejected(convert(text));
    text = fallback();
    replace(text, R"("textures":[)", R"("textures":[{"source":1},)");
    rejected(convert(text));
    text = fallback();
    replace(text, R"("source":0,"extensions":)", R"("extensions":)");
    rejected(convert(text));
    text = fallback();
    replace(text, R"("extensionsUsed":)",
            R"("extensionsRequired":["KHR_texture_basisu"],"extensionsUsed":)");
    rejected(convert(text));
}

TEST_CASE("glTF arbitrary unused unsupported images still reject", "[gltf-materials]") {
    auto text = document();
    replace(text, R"(}],"textures":)", R"(},{"uri":"data:image/ktx2;base64,AAAA"}],"textures":)");
    rejected(convert(text));
}
