// Original Shark-authored in-memory fixtures; project MIT license.
#include "shark/tools/prepared_content.hpp"

#include "shark/tools/gltf_asset.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <thread>

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

template <typename T> T id(char digit) {
    std::string value = "13000000-0000-4000-8000-000000000001";
    value.back() = digit;
    return T{take(StableId::parse(value))};
}

std::span<const std::byte> bytes(std::string_view text) {
    return std::as_bytes(std::span{text.data(), text.size()});
}

std::string triangle(bool material = true, bool unused_mesh = false) {
    std::string source =
        R"({"asset":{"version":"2.0"},"buffers":[{"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/","byteLength":60}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}],"meshes":[)";
    if (unused_mesh)
        source += R"({"primitives":[{"attributes":{"POSITION":0}}]},)";
    source += R"({"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1})";
    if (material)
        source += R"(,"material":0)";
    source +=
        R"(}]}],"nodes":[{"name":"Root","translation":[10,2,0],"scale":[-2,3,1],"children":[1,2]},{"mesh":)";
    source += unused_mesh ? "1" : "0";
    source += R"(,"translation":[0,0,4]},{"name":"Shared instance","mesh":)";
    source += unused_mesh ? "1" : "0";
    source += R"(,"translation":[1,0,0]}],"scenes":[{"nodes":[0]}],"scene":0)";
    if (material)
        source +=
            R"(,"images":[{"uri":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAE0lEQVR42mNgAIL/QACiHYB0AwA4mwe6OCAfGQAAAABJRU5ErkJggg=="}],"textures":[{"source":0}],"materials":[{"name":"Private import material name","pbrMetallicRoughness":{"baseColorFactor":[0.25,0.5,0.75,0.5],"baseColorTexture":{"index":0},"metallicFactor":0.25,"roughnessFactor":0.75},"alphaMode":"MASK","alphaCutoff":0.25,"doubleSided":true}])";
    return source + '}';
}

std::vector<std::byte> cached(bool material = true, bool unused_mesh = false) {
    const auto source = triangle(material, unused_mesh);
    const auto asset = take(convert_gltf_asset(
        {take(ProjectPath::parse("models/private-source.gltf")), bytes(source)}));
    return take(encode_gltf_asset(asset));
}

std::unique_ptr<AuthoredScene> authored() {
    const auto scene = id<SceneId>('3');
    const auto project = id<ProjectId>('1');
    const auto world = id<WorldId>('2');
    ProjectDocument project_document{project,
                                     world,
                                     Name{"Prepared fixture"},
                                     scene,
                                     {{scene, take(ProjectPath::parse("world.sharkscene"))}},
                                     "{\"private_source_metadata\":true}"};
    Camera camera;
    camera.projection = CameraProjection::orthographic;
    camera.near_clip = 0;
    camera.orthographic_height = 12;
    Transform transform;
    transform.translation = {1, 2, 3};
    SceneDocument document{project,
                           scene,
                           WorldPosition{world, {1000000000, -7, 2}, {0.5, 3.0, 1023.0}},
                           {{id<AuthoredEntityId>('4'), {}, Name{"Camera"}, {}, "{}", camera},
                            {id<AuthoredEntityId>('5'),
                             id<AuthoredEntityId>('4'),
                             Name{"Plane"},
                             transform,
                             "{\"private_entity_metadata\":true}",
                             {},
                             MeshRenderer{builtin_plane_asset_id(), 0, false}}},
                           "{\"private_scene_metadata\":true}"};
    return take(AuthoredScene::create(project_document, document));
}

bool contains(std::span<const std::byte> payload, std::string_view text) {
    const auto value = bytes(text);
    return std::search(payload.begin(), payload.end(), value.begin(), value.end()) != payload.end();
}
} // namespace

TEST_CASE("prepared mesh bridge retains geometry and unlit factors without source records",
          "[prepared-content]") {
    const auto source = cached();
    const auto before = source;
    const auto payload = take(prepare_mesh_asset(id<AssetId>('a'), source));
    const auto prepared = take(decode_prepared_mesh(payload));
    REQUIRE(prepared.meshes.size() == 1);
    REQUIRE(prepared.meshes[0].primitives.size() == 1);
    const auto& primitive = prepared.meshes[0].primitives[0];
    REQUIRE(primitive.vertices.size() == 3);
    CHECK(primitive.vertices[1].position == std::array<float, 3>{1, 0, 0});
    CHECK(primitive.vertices[1].normal == std::array<float, 3>{0, 0, 1});
    CHECK(primitive.vertices[1].texcoord0 == std::array<float, 2>{1, 0});
    CHECK(primitive.vertices[1].texcoord1 == std::array<float, 2>{0, 0});
    CHECK(primitive.vertices[1].color == std::array<float, 4>{1, 1, 1, 1});
    CHECK(primitive.indices == std::vector<std::uint32_t>{0, 1, 2});
    CHECK(primitive.material.base_color == std::array<float, 4>{0.25F, 0.5F, 0.75F, 0.5F});
    CHECK(primitive.material.double_sided);
    CHECK(prepared.asset == id<AssetId>('a'));
    CHECK_FALSE(contains(payload, "Private import material name"));
    CHECK_FALSE(contains(payload, "private-source.gltf"));
    CHECK_FALSE(contains(payload, "SHKGLT01"));
    CHECK(take(prepare_mesh_asset(id<AssetId>('a'), source)) == payload);
    CHECK(source == before);
    const auto original = take(decode_gltf_asset(source));
    CHECK(original.materials.materials[0].alpha_mode == GltfAlphaMode::mask);
    CHECK(original.materials.materials[0].metallic == 0.25F);
    CHECK_FALSE(original.textures.images.empty());
}

TEST_CASE("prepared imported hierarchy preserves dense meshes IDs poses and sharing",
          "[prepared-content]") {
    const auto source = cached(false, true);
    const auto mesh_bytes = take(prepare_mesh_asset(id<AssetId>('a'), source));
    const auto ids =
        std::array{id<AuthoredEntityId>('4'), id<AuthoredEntityId>('5'), id<AuthoredEntityId>('6')};
    const auto scene_bytes =
        take(prepare_gltf_scene(id<AssetId>('a'), id<SceneId>('3'), source, ids));
    const auto document = take(decode_prepared_scene(scene_bytes));
    REQUIRE(document.entities.size() == 3);
    CHECK_FALSE(document.world_origin);
    CHECK(document.entities[0].id == ids[0]);
    CHECK(document.entities[1].name == Name{"Node"});
    CHECK(document.entities[2].name == Name{"Shared instance"});
    CHECK(document.entities[1].parent == ids[0]);
    CHECK(document.entities[2].parent == ids[0]);
    CHECK_FALSE(document.entities[0].camera);
    CHECK_FALSE(document.entities[0].mesh_renderer);
    REQUIRE(document.entities[1].mesh_renderer);
    REQUIRE(document.entities[2].mesh_renderer);
    CHECK(document.entities[1].mesh_renderer->mesh_index == 0); // source mesh was one
    CHECK(document.entities[2].mesh_renderer == document.entities[1].mesh_renderer);
    const std::array inputs{PreparedAssetInput{id<AssetId>('a'), mesh_bytes}};
    auto assets = take(PreparedAssets::create(inputs));
    auto loaded = take(LoadedPreparedScene::load(scene_bytes, assets));
    auto& scene = loaded->scene();
    const auto a = take(scene.entity_state(take(scene.find(ids[1]))));
    const auto b = take(scene.entity_state(take(scene.find(ids[2]))));
    CHECK(math::Vec3{a.world_matrix[3]} == math::Vec3{10, 2, 4});
    CHECK(math::Vec3{b.world_matrix[3]} == math::Vec3{8, 2, 0});
    CHECK(a.local_transform.translation == math::Vec3{0, 0, 4});
    const auto first = take(assets->resolve(*a.mesh_renderer));
    const auto second = take(assets->resolve(*b.mesh_renderer));
    CHECK(first == second);
    CHECK(first->meshes[0].primitives[0].material == PreparedMaterial{});
    loaded.reset();
    assets.reset();
    CHECK(first->meshes[0].primitives[0].vertices[2].position == std::array<float, 3>{0, 1, 0});
    CHECK(take(prepare_gltf_scene(id<AssetId>('a'), id<SceneId>('3'), source, ids)) == scene_bytes);
}

TEST_CASE("prepared authoring capture preserves components anchor and revision without saving",
          "[prepared-content]") {
    const auto owner = authored();
    const auto initial = take(owner->status());
    const auto changed = take(owner->execute(
        SetNameCommand{id<AuthoredEntityId>('5'), Name{"Edited plane"}}, initial.revision));
    REQUIRE(changed.outcome);
    const auto before = take(owner->capture());
    const auto history = take(owner->history());
    const auto payload = take(prepare_scene(*owner, before.revision));
    const auto prepared = take(decode_prepared_scene(payload));
    CHECK(prepared.scene == before.scene.scene);
    CHECK(prepared.world_origin == before.scene.world_origin);
    REQUIRE(prepared.entities.size() == 2);
    CHECK(prepared.entities[0].camera == before.scene.entities[0].camera);
    CHECK(prepared.entities[1].mesh_renderer == before.scene.entities[1].mesh_renderer);
    CHECK(prepared.entities[1].name == Name{"Edited plane"});
    CHECK(prepared.entities[1].parent == id<AuthoredEntityId>('4'));
    CHECK_FALSE(contains(payload, "private_source_metadata"));
    CHECK_FALSE(contains(payload, "private_scene_metadata"));
    CHECK_FALSE(contains(payload, "private_entity_metadata"));
    CHECK_FALSE(contains(payload, "world.sharkscene"));
    auto loaded = take(LoadedPreparedScene::load(payload, take(PreparedAssets::create())));
    CHECK(loaded->world_origin() == before.scene.world_origin);
    const auto handle = take(loaded->scene().find(id<AuthoredEntityId>('5')));
    CHECK(take(loaded->scene().entity_state(handle)).local_transform.translation ==
          math::Vec3{1, 2, 3});
    const auto after = take(owner->capture());
    CHECK(after.revision == before.revision);
    CHECK(after.scene == before.scene);
    CHECK(after.dirty);
    CHECK(take(owner->history()).next_undo->ticket == history.next_undo->ticket);
    const auto stale = prepare_scene(*owner, initial.revision);
    REQUIRE_FALSE(stale);
    CHECK(stale.error().code == ErrorCode::conflict);
    CHECK(stale.error().context.operation == "content.scene.prepare");
    bool rejected = false;
    std::thread worker([&] { rejected = !prepare_scene(*owner); });
    worker.join();
    CHECK(rejected);
}

TEST_CASE("prepared bridges reject raw source corrupt cache and mismatched imported identities",
          "[prepared-content]") {
    const auto source = triangle();
    CHECK_FALSE(prepare_mesh_asset(id<AssetId>('a'), bytes(source)));
    auto payload = cached();
    const std::array ids{id<AuthoredEntityId>('4'), id<AuthoredEntityId>('5'),
                         id<AuthoredEntityId>('6')};
    payload.back() ^= std::byte{1};
    const auto corrupt = prepare_mesh_asset(id<AssetId>('a'), payload);
    REQUIRE_FALSE(corrupt);
    CHECK(corrupt.error().context.asset_id == id<AssetId>('a').value().to_string());
    CHECK(corrupt.error().context.operation == "content.mesh.prepare");
    CHECK_FALSE(prepare_gltf_scene(id<AssetId>('a'), id<SceneId>('3'), payload, ids));
    payload = cached();
    CHECK_FALSE(prepare_mesh_asset(builtin_cube_asset_id(), payload));
    CHECK_FALSE(prepare_gltf_scene(builtin_plane_asset_id(), id<SceneId>('3'), payload, ids));
    CHECK_FALSE(
        prepare_gltf_scene(id<AssetId>('a'), id<SceneId>('3'), payload, std::span{ids}.first(2)));
    auto duplicates = ids;
    duplicates[2] = duplicates[1];
    CHECK_FALSE(prepare_gltf_scene(id<AssetId>('a'), id<SceneId>('3'), payload, duplicates));
    auto valid_asset = take(decode_gltf_asset(payload));
    valid_asset.geometry.nodes[1].name = "invalid\nname";
    const auto invalid_name = take(encode_gltf_asset(valid_asset));
    CHECK_FALSE(prepare_gltf_scene(id<AssetId>('a'), id<SceneId>('3'), invalid_name, ids));
    const auto valid_mesh = take(prepare_mesh_asset(id<AssetId>('a'), payload));
    const auto valid_scene =
        take(prepare_gltf_scene(id<AssetId>('a'), id<SceneId>('3'), payload, ids));
    CHECK_FALSE(LoadedPreparedScene::load(valid_scene, take(PreparedAssets::create())));
    const std::array inputs{PreparedAssetInput{id<AssetId>('a'), valid_mesh}};
    CHECK(LoadedPreparedScene::load(valid_scene, take(PreparedAssets::create(inputs))));
}

TEST_CASE("prepared source bridge retains all supported vertex attributes exactly",
          "[prepared-content]") {
    auto asset = take(decode_gltf_asset(cached(false)));
    auto& primitive = asset.geometry.primitives[0];
    primitive.has_texcoord1 = true;
    primitive.has_color = true;
    primitive.has_tangents = true;
    for (auto& vertex : primitive.vertices) {
        vertex.texcoord1 = {0.25F, 0.75F};
        vertex.color = {0.125F, 0.25F, 0.5F, 0.75F};
        vertex.tangent = {1, 0, 0, -1};
    }
    const auto original = take(encode_gltf_asset(asset));
    const auto prepared =
        take(decode_prepared_mesh(take(prepare_mesh_asset(id<AssetId>('a'), original))));
    const auto& vertex = prepared.meshes[0].primitives[0].vertices[0];
    CHECK(vertex.position == std::array<float, 3>{0, 0, 0});
    CHECK(vertex.normal == std::array<float, 3>{0, 0, 1});
    CHECK(vertex.texcoord0 == std::array<float, 2>{0, 0});
    CHECK(vertex.texcoord1 == std::array<float, 2>{0.25F, 0.75F});
    CHECK(vertex.color == std::array<float, 4>{0.125F, 0.25F, 0.5F, 0.75F});
    CHECK(vertex.tangent == std::array<float, 4>{1, 0, 0, -1});
}
