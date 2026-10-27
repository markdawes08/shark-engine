// Original Shark-authored in-memory fixtures; project MIT license.
#include "shark/tools/gltf_asset.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <string>

namespace {
using namespace shark;
using namespace shark::tools;
template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        INFO(result.error().context.operation);
    }
    REQUIRE(result);
    return std::move(result).value();
}
std::span<const std::byte> bytes(std::string_view text) {
    return std::as_bytes(std::span{text.data(), text.size()});
}
AssetId id(std::string_view text = "12000000-0000-4000-8000-000000000012") {
    return AssetId{take(StableId::parse(text))};
}
ProjectPath path(std::string_view text) {
    return take(ProjectPath::parse(text));
}
const AssetTargetProfile target{"fixture-x64", AssetAudience::client};
std::string triangle(bool textured = true,
                     std::string nodes = R"([{"mesh":0,"name":"triangle"}])") {
    std::string source =
        R"({"asset":{"version":"2.0"},"buffers":[{"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/","byteLength":60}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1})";
    if (textured)
        source += R"(,"material":0)";
    source += R"(}]}],"nodes":)" + nodes + R"(,"scenes":[{"nodes":[0]}],"scene":0)";
    if (textured)
        source +=
            R"(,"images":[{"uri":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAE0lEQVR42mNgAIL/QACiHYB0AwA4mwe6OCAfGQAAAABJRU5ErkJggg=="}],"textures":[{"source":0}],"materials":[{"name":"paint","pbrMetallicRoughness":{"baseColorFactor":[0.25,0.5,0.75,0.5],"metallicFactor":0.25,"roughnessFactor":0.75,"baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":0}},"normalTexture":{"index":0,"scale":-0.5},"occlusionTexture":{"index":0,"strength":0.5},"emissiveTexture":{"index":0},"emissiveFactor":[0.1,0.2,0.3],"alphaMode":"MASK","alphaCutoff":0.25,"doubleSided":true}])";
    return source + '}';
}
GltfAsset sample(bool textured = true) {
    const auto source = triangle(textured);
    return take(convert_gltf_asset({path("models/triangle.gltf"), bytes(source)}));
}
AssetArtifact artifact(const AssetImporter& importer, std::vector<std::byte> payload,
                       std::string_view source, std::string_view settings) {
    const auto& info = importer.info();
    const auto digest = take(sha256(payload));
    return {id(),
            {},
            info.kind,
            AssetAudience::shared,
            target,
            info.id,
            info.version,
            info.payload_version,
            take(sha256(bytes(source))),
            take(sha256(bytes(settings))),
            {},
            digest,
            std::move(payload)};
}
void repair_digest(std::vector<std::byte>& payload) {
    const auto hash = take(sha256(std::span<const std::byte>{payload}.subspan(44)));
    std::copy(hash.bytes.begin(), hash.bytes.end(), payload.begin() + 12);
}
void store32(std::vector<std::byte>& payload, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        payload.at(at + i) = static_cast<std::byte>((value >> (i * 8U)) & 255U);
}
} // namespace

TEST_CASE("glTF asset round trip preserves independent geometry material and texture values",
          "[gltf-assets]") {
    const auto asset = sample();
    const auto payload = take(encode_gltf_asset(asset));
    REQUIRE(payload.size() < gltf_asset_maximum_payload_bytes);
    REQUIRE(std::equal(payload.begin(), payload.begin() + 8, bytes("SHKGLT01").begin()));
    CHECK(payload[8] == std::byte{1});
    CHECK(payload[9] == std::byte{0});
    const auto decoded = take(decode_gltf_asset(payload));
    REQUIRE(decoded.geometry.primitives.size() == 1);
    CHECK(decoded.geometry.primitives[0].vertices[1].position == std::array<float, 3>{1, 0, 0});
    CHECK(decoded.geometry.primitives[0].indices == std::vector<std::uint32_t>{0, 1, 2});
    CHECK(decoded.geometry.nodes[0].name == "triangle");
    REQUIRE(decoded.materials.materials.size() == 1);
    const auto& material = decoded.materials.materials[0];
    CHECK(material.base_color == std::array<float, 4>{0.25F, 0.5F, 0.75F, 0.5F});
    CHECK(material.metallic == 0.25F);
    CHECK(material.roughness == 0.75F);
    CHECK(material.normal_scale == -0.5F);
    CHECK(material.occlusion_strength == 0.5F);
    CHECK(material.alpha_mode == GltfAlphaMode::mask);
    CHECK(material.alpha_cutoff == 0.25F);
    CHECK(material.double_sided);
    CHECK(decoded.textures.images == asset.textures.images);
    CHECK(decoded.textures.usages == asset.textures.usages);
    CHECK(decoded.materials.materials == asset.materials.materials);
    CHECK(take(encode_gltf_asset(decoded)) == payload);
    CHECK(take(encode_gltf_asset(sample())) == payload);
}

TEST_CASE("glTF asset preserves shared hierarchy reflection and world bounds", "[gltf-assets]") {
    const auto source = triangle(
        false,
        R"([{"children":[1,2],"translation":[10,2,0],"scale":[-2,3,1]},{"mesh":0,"translation":[0,0,4]},{"mesh":0,"translation":[1,0,0]}])");
    const auto asset = take(convert_gltf_asset({path("model.gltf"), bytes(source)}));
    const auto decoded = take(decode_gltf_asset(take(encode_gltf_asset(asset))));
    REQUIRE(decoded.geometry.nodes.size() == 3);
    CHECK(decoded.geometry.meshes.size() == 1);
    CHECK(decoded.geometry.nodes[1].mesh == decoded.geometry.nodes[2].mesh);
    CHECK(decoded.geometry.nodes[1].mirrored);
    CHECK(decoded.geometry.nodes[1].parent == 0);
    CHECK(decoded.geometry.nodes[0].children == std::vector<std::size_t>{1, 2});
    REQUIRE(decoded.geometry.world_bounds);
    CHECK(decoded.geometry.world_bounds->minimum == math::Vec3{6, 2, 0});
    CHECK(decoded.geometry.world_bounds->maximum == math::Vec3{10, 5, 4});
    CHECK(decoded.materials.materials.empty());
    CHECK_FALSE(decoded.geometry.primitives[0].source_material);
}

TEST_CASE("glTF asset accepts the empty static scene and rejects missing scene inventory",
          "[gltf-assets]") {
    const std::string source = R"({"asset":{"version":"2.0"}})";
    const auto asset = take(convert_gltf_asset({path("empty.gltf"), bytes(source)}));
    const auto decoded = take(decode_gltf_asset(take(encode_gltf_asset(asset))));
    CHECK(decoded.geometry.nodes.empty());
    CHECK_FALSE(decoded.geometry.selected_scene);
    auto invalid = sample();
    invalid.geometry.selected_scene.reset();
    invalid.textures.selected_scene.reset();
    invalid.materials.selected_scene.reset();
    CHECK_FALSE(encode_gltf_asset(invalid));
}

TEST_CASE("glTF asset rejects truncated changed and future payloads", "[gltf-assets]") {
    const auto payload = take(encode_gltf_asset(sample(false)));
    for (std::size_t size = 0; size < payload.size(); ++size)
        REQUIRE_FALSE(decode_gltf_asset(std::span<const std::byte>{payload}.first(size)));
    auto changed = payload;
    changed.back() ^= std::byte{1};
    CHECK_FALSE(decode_gltf_asset(changed));
    changed = payload;
    changed[8] = std::byte{2};
    CHECK_FALSE(decode_gltf_asset(changed));
    changed = payload;
    changed.push_back(std::byte{0});
    repair_digest(changed);
    CHECK_FALSE(decode_gltf_asset(changed));
    changed = payload;
    store32(changed, 48, std::numeric_limits<std::uint32_t>::max());
    repair_digest(changed);
    CHECK_FALSE(decode_gltf_asset(changed));
}

TEST_CASE("glTF asset validates crossed primitive material and hierarchy relationships",
          "[gltf-assets]") {
    auto asset = sample();
    SECTION("vertex index") {
        asset.geometry.primitives[0].indices[0] = 3;
    }
    SECTION("missing material") {
        asset.geometry.primitives[0].source_material = 100;
    }
    SECTION("missing UV") {
        asset.geometry.primitives[0].has_texcoord0 = false;
    }
    SECTION("duplicate primitive") {
        asset.geometry.meshes[0].primitives.push_back(0);
    }
    SECTION("parent cycle") {
        asset.geometry.nodes[0].parent = 0;
    }
    SECTION("world matrix") {
        asset.geometry.nodes[0].world_matrix[3][0] = 42;
    }
    SECTION("normal matrix") {
        asset.geometry.nodes[0].normal_matrix[0] = 42;
    }
    SECTION("mirror") {
        asset.geometry.nodes[0].mirrored = true;
    }
    SECTION("bounds") {
        asset.geometry.world_bounds->minimum.x = -42;
    }
    SECTION("factor") {
        asset.materials.materials[0].roughness = 2;
    }
    SECTION("nonfinite") {
        asset.materials.materials[0].normal_scale = std::numeric_limits<float>::infinity();
    }
    CHECK_FALSE(encode_gltf_asset(asset));
}

TEST_CASE("glTF asset validates image chains and semantic links", "[gltf-assets]") {
    auto asset = sample();
    SECTION("mip bytes") {
        asset.textures.images[0].mips[0].rgba.pop_back();
    }
    SECTION("missing mip") {
        asset.textures.images[0].mips.pop_back();
    }
    SECTION("extra mip") {
        asset.textures.images[0].mips.push_back(asset.textures.images[0].mips.back());
    }
    SECTION("image link") {
        asset.textures.usages[0].image = 100;
    }
    SECTION("semantic") {
        asset.textures.images[0].semantic = GltfTextureSemantic::linear_data;
    }
    SECTION("missing usage") {
        asset.textures.usages.erase(asset.textures.usages.begin());
    }
    SECTION("UV link") {
        asset.materials.materials[0].textures[0]->texcoord = 1;
    }
    SECTION("sampler") {
        asset.textures.usages[0].sampler.mip = static_cast<GltfTextureMipFilter>(99);
    }
    CHECK_FALSE(encode_gltf_asset(asset));
}

TEST_CASE("glTF asset conversion and codecs respect cancellation", "[gltf-assets]") {
    auto asset = sample(false);
    const auto payload = take(encode_gltf_asset(asset));
    std::stop_source stopped;
    stopped.request_stop();
    const auto source = triangle(false);
    CHECK_FALSE(convert_gltf_asset({path("model.gltf"), bytes(source), {}, stopped.get_token()}));
    CHECK_FALSE(encode_gltf_asset(asset, stopped.get_token()));
    CHECK_FALSE(decode_gltf_asset(payload, stopped.get_token()));
}

TEST_CASE("glTF typed resource preserves exact bytes and checks descriptor identity and digest",
          "[gltf-assets]") {
    GltfResourceImporter importer;
    const std::string source{"aBc\0xyz", 7};
    auto payload = take(importer.import({id(), "gltf-resource", bytes(source), "{}", target, {}}));
    REQUIRE(payload.size() == 84 + source.size());
    CHECK(std::ranges::equal(std::span<const std::byte>{payload}.subspan(84), bytes(source)));
    auto valid = artifact(importer, payload, source, "{}");
    REQUIRE(importer.validate_payload(valid));
    auto bad = valid;
    SECTION("body") {
        bad.payload.back() ^= std::byte{1};
    }
    SECTION("identity") {
        bad.asset = id("12000000-0000-4000-8000-000000000013");
    }
    SECTION("source digest") {
        bad.source_digest.bytes[0] ^= std::byte{1};
    }
    SECTION("settings") {
        bad.settings_digest = take(sha256(bytes("{\"transform\":\"identity\"}")));
    }
    SECTION("descriptor") {
        bad.importer = "shark.bytes";
    }
    CHECK_FALSE(importer.validate_payload(bad));
    CHECK_FALSE(importer.normalize_settings(R"({"transform":"identity"})"));
    CHECK_FALSE(importer.import({id(), "bytes", bytes(source), "{}", target, {}}));
}

TEST_CASE("glTF settings normalize explicit aliases and reject ambiguous mappings",
          "[gltf-assets]") {
    GltfAssetImporter importer;
    CHECK(take(importer.normalize_settings(R"({"path":"model.gltf"})")) ==
          R"({"path":"model.gltf","resources":[]})");
    CHECK(take(importer.normalize_settings(R"({"scene":0,"resources":[],"path":"model.glb"})")) ==
          R"({"path":"model.glb","resources":[],"scene":0})");
    for (
        const auto invalid :
        {R"({})", R"({"path":"../model.gltf"})", R"({"path":"model.png"})",
         R"({"path":"model.gltf","scene":-1})", R"({"path":"model.gltf","scene":0.5})",
         R"({"path":"model.gltf","scene":4096})", R"({"path":"model.gltf","extra":1})",
         R"({"path":"model.gltf","path":"other.gltf"})",
         R"({"path":"model.gltf","resources":[{"path":"A.bin","asset":"12000000-0000-4000-8000-000000000012"},{"path":"a.bin","asset":"12000000-0000-4000-8000-000000000013"}]})",
         R"({"path":"model.gltf","resources":[{"path":"a.bin","asset":"12000000-0000-4000-8000-000000000012"},{"path":"b.bin","asset":"12000000-0000-4000-8000-000000000012"}]})"}) {
        INFO(invalid);
        CHECK_FALSE(importer.normalize_settings(invalid));
    }
}

TEST_CASE("glTF native importer emits the engine payload and rejects undeclared dependencies",
          "[gltf-assets]") {
    GltfAssetImporter importer;
    const auto source = triangle(false);
    const auto settings = take(importer.normalize_settings(R"({"path":"model.gltf"})"));
    const auto payload = take(importer.import({id(), "gltf", bytes(source), settings, target, {}}));
    CHECK(take(decode_gltf_asset(payload)).geometry.primitives.size() == 1);
    REQUIRE(importer.validate_payload(artifact(importer, payload, source, settings)));
    CHECK_FALSE(importer.import({id(), "bytes", bytes(source), settings, target, {}}));
    CHECK_FALSE(importer.import(
        {id(),
         "gltf",
         bytes(source),
         R"({"path":"model.gltf","resources":[{"path":"a.bin","asset":"12000000-0000-4000-8000-000000000012"}]})",
         target,
         {}}));
}

TEST_CASE("glTF payload cap is independent from valid CPU texture output", "[gltf-assets]") {
    auto asset = sample();
    asset.textures.images.resize(1);
    asset.textures.usages.resize(1);
    for (std::size_t role = 1; role < 5; ++role)
        asset.materials.materials[0].textures[role].reset();
    auto& image = asset.textures.images[0];
    image.mips.clear();
    for (std::uint32_t size = 1024;; size /= 2) {
        image.mips.push_back(
            {size, size, std::vector<std::byte>(static_cast<std::size_t>(size) * size * 4)});
        if (size == 1)
            break;
    }
    const auto encoded = encode_gltf_asset(asset);
    REQUIRE_FALSE(encoded);
    CHECK(encoded.error().message.find("2 MiB") != std::string::npos);
    const std::vector<std::byte> oversized(gltf_asset_maximum_payload_bytes + 1);
    CHECK_FALSE(decode_gltf_asset(oversized));
    const std::string empty = R"({"asset":{"version":"2.0"}})";
    auto forged =
        take(encode_gltf_asset(take(convert_gltf_asset({path("empty.gltf"), bytes(empty)}))));
    // Fixed schema-1 empty report ends at byte 92; scene selection occupies four
    // bytes and the following primitive count must be bounded before allocation.
    store32(forged, 96, std::numeric_limits<std::uint32_t>::max());
    repair_digest(forged);
    CHECK_FALSE(decode_gltf_asset(forged));
}

TEST_CASE("glTF resource inputs require typed provenance and original source bytes",
          "[gltf-assets]") {
    GltfAssetImporter importer;
    GltfResourceImporter resource;
    ByteAssetImporter transformed;
    const auto resource_id = id("12000000-0000-4000-8000-000000000013");
    const std::string data = "abcd";
    const std::string source =
        R"({"asset":{"version":"2.0"},"buffers":[{"uri":"a.bin","byteLength":4}]})";
    const std::string settings =
        R"({"path":"model.gltf","resources":[{"path":"a.bin","asset":"12000000-0000-4000-8000-000000000013"}]})";
    auto raw = take(resource.import({resource_id, "gltf-resource", bytes(data), "{}", target, {}}));
    std::array<AssetImportDependency, 1> dependencies{{{resource_id,
                                                        {},
                                                        raw,
                                                        "gltf-resource",
                                                        "shark.gltf-resource",
                                                        1,
                                                        1,
                                                        take(sha256(bytes(data)))}}};
    const auto payload =
        take(importer.import({id(), "gltf", bytes(source), settings, target, dependencies}));
    const auto decoded = take(decode_gltf_asset(payload));
    REQUIRE(decoded.geometry.source.resources.size() == 1);
    CHECK(decoded.geometry.source.resources[0].asset == resource_id);
    CHECK(decoded.geometry.source.resources[0].digest == take(sha256(bytes(data))));
    auto a = artifact(importer, payload, source, take(importer.normalize_settings(settings)));
    CHECK_FALSE(importer.validate_payload(a));
    a.dependencies.push_back({resource_id, {}});
    REQUIRE(importer.validate_payload(a));
    dependencies[0].importer = "shark.bytes";
    CHECK_FALSE(importer.import({id(), "gltf", bytes(source), settings, target, dependencies}));
    dependencies[0].importer = "shark.gltf-resource";
    auto upper = take(transformed.import(
        {resource_id, "bytes", bytes(data), R"({"transform":"uppercase-ascii"})", target, {}}));
    dependencies[0].payload = upper;
    CHECK_FALSE(importer.import({id(), "gltf", bytes(source), settings, target, dependencies}));
    dependencies[0].payload = raw;
    dependencies[0].source_digest.bytes[0] ^= std::byte{1};
    CHECK_FALSE(importer.import({id(), "gltf", bytes(source), settings, target, dependencies}));
}
