#include "shark/client/scene_viewport.hpp"

#include "gpu_shader_fixture.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <numbers>
#include <string>

namespace {
using namespace shark;
using namespace shark::client;

template <typename T> T take(Result<T> result) {
    if (!result)
        INFO(result.error().message);
    REQUIRE(result);
    return std::move(result).value();
}
template <typename Id> Id id(unsigned value) {
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < 8; ++i) {
        text[35 - i] = digits[value & 15U];
        value >>= 4U;
    }
    return Id{take(StableId::parse(text))};
}
ShaderArtifact vertex_shader() {
    const auto& words = shark::test::shaders::scene_vs_words;
    return {ShaderStage::vertex, "vsScene", {words.begin(), words.end()}};
}
ShaderArtifact pixel_shader() {
    const auto& words = shark::test::shaders::scene_ps_words;
    return {ShaderStage::pixel, "psScene", {words.begin(), words.end()}};
}
SceneRevision revision(const Scene& owner) {
    const auto stats = take(owner.stats());
    return {stats.instance, stats.revision};
}
EntityHandle add_camera(Scene& scene, bool mirrored = false) {
    EntitySeed seed{id<AuthoredEntityId>(1)};
    seed.camera = Camera{};
    seed.camera->vertical_fov_radians = std::numbers::pi / 2;
    seed.camera->near_clip = 1;
    seed.camera->far_clip = 10;
    if (mirrored)
        seed.transform.scale.x = -1;
    return take(scene.create_entity(seed));
}
PreparedPrimitive triangle(std::array<float, 4> color) {
    PreparedPrimitive primitive;
    for (const auto& position :
         std::array<std::array<float, 3>, 3>{{{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}}}) {
        PreparedVertex vertex;
        vertex.position = position;
        vertex.normal = {0, 0, 1};
        vertex.color = color;
        primitive.vertices.push_back(vertex);
    }
    primitive.indices = {0, 1, 2};
    return primitive;
}
SceneViewportRequest simple_request() {
    SceneViewportRequest request;
    request.extent = {64, 48};
    request.scene_revision = {123, 4};
    request.origin.generation = 7;
    request.vertices = {
        {{-1, -1, 0}, {1, 0, 0, 1}}, {{1, -1, 0}, {0, 1, 0, 1}}, {{0, 1, 0}, {0, 0, 1, 1}}};
    request.indices = {0, 1, 2};
    SceneViewportDraw draw;
    draw.index_count = 3;
    draw.transform = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    request.draws.push_back(draw);
    request.vertex_shader = vertex_shader();
    request.pixel_shader = pixel_shader();
    return request;
}
} // namespace

namespace shark::client::test {
SceneViewportRequest scene_viewport_request_fixture() {
    return simple_request();
}
} // namespace shark::client::test

TEST_CASE("Scene viewport preparation shares primitive uploads and preserves draw semantics",
          "[scene-viewport]") {
    auto first = triangle({.25F, .5F, .75F, 1});
    first.material.base_color = {.5F, 1, .25F, 1};
    auto second = triangle({1, 0, 0, .5F});
    second.material.base_color = {1, .5F, .75F, .25F};
    second.material.double_sided = true;
    PreparedMeshAsset asset{id<AssetId>(9), {{std::vector{first, second}}}};
    const auto bytes = take(encode_prepared_mesh(asset));
    const std::array inputs{PreparedAssetInput{asset.asset, bytes}};
    const auto assets = take(PreparedAssets::create(inputs));
    const auto scene = take(Scene::create(AuthoredSceneScope{id<SceneId>(1)}));
    const auto camera = add_camera(*scene, true);
    for (unsigned number : {2U, 3U}) {
        EntitySeed entity{id<AuthoredEntityId>(number)};
        entity.transform.translation = {number == 2 ? 0.0 : 2.0, 0, -5};
        entity.transform.scale.x = number == 2 ? 1 : -1;
        entity.mesh_renderer = MeshRenderer{asset.asset};
        static_cast<void>(take(scene->create_entity(entity)));
    }
    const auto before = revision(*scene);
    RenderExtractionOptions options{camera, before, {128, 64}};
    options.origin.world_anchor =
        WorldPosition{id<WorldId>(2), {world_cell_max, -3, 0}, {.25, 1, 0}};
    options.origin.generation = 9;
    const auto extracted = take(extract_render_scene(*scene, *assets, options));
    const auto request =
        take(make_scene_viewport_request(extracted, vertex_shader(), pixel_shader()));
    CHECK(revision(*scene) == before);
    CHECK(request.scene_revision == before);
    CHECK(request.origin == options.origin);
    CHECK(request.extent == WindowExtent{128, 64});
    REQUIRE(request.vertices.size() == 6);
    CHECK(request.indices == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5});
    REQUIRE(request.draws.size() == 4);
    for (std::size_t i = 0; i < request.draws.size(); ++i) {
        CHECK(request.draws[i].first_index == (i % 2) * 3);
        CHECK(request.draws[i].index_count == 3);
        CHECK(request.draws[i].base_color ==
              (i % 2 == 0 ? first.material.base_color : second.material.base_color));
        CHECK(request.draws[i].double_sided == (i % 2 != 0));
        CHECK(request.draws[i].mirrored == (i < 2));
    }
    CHECK(request.vertices[0].position == first.vertices[0].position);
    CHECK(request.vertices[0].color == first.vertices[0].color);
    CHECK(request.vertices[3].color == second.vertices[0].color);
    // Independent camera/model expectation: mirror-X camera, aspect2, FOV90,
    // near1/far10, model z=-5. Projection Y has already been flipped once.
    CHECK(request.draws[0].transform[0] == Catch::Approx(-.5));
    CHECK(request.draws[0].transform[5] == Catch::Approx(-1));
    CHECK(request.draws[0].transform[10] == Catch::Approx(-10.0 / 9.0));
    CHECK(request.draws[0].transform[14] == Catch::Approx(40.0 / 9.0));
    CHECK(request.draws[0].transform[15] == Catch::Approx(5));
    CHECK(request.draws[2].transform[0] == Catch::Approx(.5));
    CHECK(request.draws[2].transform[12] == Catch::Approx(-1));
    REQUIRE(validate_scene_viewport_request(request));
}

TEST_CASE("Scene viewport preparation supports empty clears and rejects unrepresentable matrices",
          "[scene-viewport]") {
    const auto scene = take(Scene::create(AuthoredSceneScope{id<SceneId>(1)}));
    const auto camera = add_camera(*scene);
    const auto assets = take(PreparedAssets::create());
    auto extracted =
        take(extract_render_scene(*scene, *assets, {camera, revision(*scene), {2048, 1}}));
    const auto empty =
        take(make_scene_viewport_request(extracted, vertex_shader(), pixel_shader()));
    CHECK(empty.vertices.empty());
    CHECK(empty.indices.empty());
    CHECK(empty.draws.empty());
    REQUIRE(validate_scene_viewport_request(empty));
    EntitySeed entity{id<AuthoredEntityId>(2)};
    entity.transform.translation.z = -5;
    entity.mesh_renderer = MeshRenderer{builtin_cube_asset_id()};
    static_cast<void>(take(scene->create_entity(entity)));
    auto value = take(scene->entity_state(camera)).camera.value();
    SECTION("finite double overflows float") {
        value.vertical_fov_radians = 1e-40;
    }
    SECTION("finite double narrows to a GPU denormal") {
        value.projection = CameraProjection::orthographic;
        value.orthographic_height = 1e40;
    }
    SECTION("finite double narrows to zero") {
        value.projection = CameraProjection::orthographic;
        value.orthographic_height = 1e100;
    }
    SECTION("float projection collapses the finite far plane") {
        value.far_clip = 33554432;
    }
    SECTION("float projection retains a finite but materially shifted far plane") {
        value.near_clip = .01;
        value.far_clip = 60000;
    }
    REQUIRE(scene->set_camera(camera, value));
    RenderExtractionOptions options{camera, revision(*scene), {64, 64}};
    options.frustum_culling = false;
    extracted = take(extract_render_scene(*scene, *assets, options));
    REQUIRE(extracted.records().size() == 1);
    const auto before = revision(*scene);
    const auto result = make_scene_viewport_request(extracted, vertex_shader(), pixel_shader());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::invalid_argument);
    CHECK_FALSE(result.error().context.operation.empty());
    CHECK(revision(*scene) == before);
}

TEST_CASE("Scene viewport validates mutable geometry draw ranges and finite values",
          "[scene-viewport]") {
    auto request = simple_request();
    REQUIRE(validate_scene_viewport_request(request));
    SECTION("unused vertices in empty request") {
        request.draws.clear();
    }
    SECTION("too few vertices") {
        request.vertices.pop_back();
    }
    SECTION("incomplete index triangle") {
        request.indices.pop_back();
    }
    SECTION("index beyond vertex storage") {
        request.indices[0] = 3;
    }
    SECTION("empty draw") {
        request.draws[0].index_count = 0;
    }
    SECTION("draw starts inside triangle") {
        request.draws[0].first_index = 1;
    }
    SECTION("draw extends beyond uploaded indices") {
        request.draws[0].index_count = 6;
    }
    SECTION("draw range overflow") {
        request.draws[0].first_index = std::numeric_limits<std::uint32_t>::max();
    }
    SECTION("position outside render range") {
        request.vertices[0].position[0] = 65537;
    }
    SECTION("nonfinite vertex") {
        request.vertices[0].position[0] = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("color outside factor range") {
        request.vertices[0].color[0] = -1;
    }
    SECTION("nonfinite color") {
        request.vertices[0].color[0] = std::numeric_limits<float>::infinity();
    }
    SECTION("base factor outside range") {
        request.draws[0].base_color[0] = 2;
    }
    SECTION("nonfinite transform") {
        request.draws[0].transform[0] = std::numeric_limits<float>::infinity();
    }
    SECTION("denormal transform") {
        request.draws[0].transform[0] = std::numeric_limits<float>::denorm_min();
    }
    SECTION("finite coefficients overflow shader dot products") {
        request.draws[0].transform[0] = std::numeric_limits<float>::max();
        request.draws[0].transform[4] = std::numeric_limits<float>::max();
    }
    REQUIRE_FALSE(validate_scene_viewport_request(request));
}

TEST_CASE("Scene viewport preparation charges every primitive before draw publication",
          "[scene-viewport]") {
    PreparedMeshAsset asset{id<AssetId>(9),
                            {{std::vector<PreparedPrimitive>(scene_viewport_maximum_draws + 1,
                                                             triangle({1, 1, 1, 1}))}}};
    const auto bytes = take(encode_prepared_mesh(asset));
    const std::array inputs{PreparedAssetInput{asset.asset, bytes}};
    const auto assets = take(PreparedAssets::create(inputs));
    const auto scene = take(Scene::create(AuthoredSceneScope{id<SceneId>(1)}));
    const auto camera = add_camera(*scene);
    EntitySeed entity{id<AuthoredEntityId>(2)};
    entity.transform.translation.z = -5;
    entity.mesh_renderer = MeshRenderer{asset.asset};
    static_cast<void>(take(scene->create_entity(entity)));
    const auto before = revision(*scene);
    const auto extracted = take(extract_render_scene(*scene, *assets, {camera, before, {64, 64}}));
    REQUIRE(extracted.records().size() == 1);
    REQUIRE_FALSE(make_scene_viewport_request(extracted, vertex_shader(), pixel_shader()));
    CHECK(revision(*scene) == before);

    auto request = simple_request();
    request.extent = {scene_viewport_maximum_extent, scene_viewport_maximum_extent};
    const auto draw = request.draws.front();
    request.draws.resize(scene_viewport_maximum_draws, draw);
    REQUIRE(validate_scene_viewport_request(request));
}

TEST_CASE("Scene viewport enforces metadata shader selection and admission budgets",
          "[scene-viewport]") {
    auto request = simple_request();
    SECTION("zero extent") {
        request.extent.width = 0;
    }
    SECTION("excess extent") {
        request.extent.height = scene_viewport_maximum_extent + 1;
    }
    SECTION("foreign unbound scene identity") {
        request.scene_revision.scene_instance = 0;
    }
    SECTION("origin generation") {
        request.origin.generation = 0;
    }
    SECTION("origin finite offset") {
        request.origin.scene_local_offset.x = std::numeric_limits<double>::infinity();
    }
    SECTION("global anchor normalization") {
        request.origin.world_anchor = WorldPosition{id<WorldId>(2), {}, {1024, 0, 0}};
    }
    SECTION("shader stages") {
        request.pixel_shader = vertex_shader();
    }
    SECTION("cube shader cannot be selected for scene draws") {
        const auto& words = shark::test::shaders::vs_words;
        request.vertex_shader = {ShaderStage::vertex, "vsMain", {words.begin(), words.end()}};
    }
    SECTION("mutated artifact") {
        request.vertex_shader.words[0] = 0;
    }
    SECTION("draw budget") {
        request.draws.resize(scene_viewport_maximum_draws + 1, request.draws.front());
    }
    SECTION("vertex budget") {
        request.vertices.resize(scene_viewport_maximum_vertices + 1);
    }
    SECTION("index budget") {
        request.indices.resize(scene_viewport_maximum_indices + 1);
    }
    SECTION("shared ranges still charge every submitted index") {
        request.indices.resize(scene_viewport_maximum_indices, 0);
        auto draw = request.draws.front();
        draw.index_count = static_cast<std::uint32_t>(scene_viewport_maximum_indices);
        request.draws.assign(4, draw);
        REQUIRE(validate_scene_viewport_request(request));
        request.draws.push_back(draw);
    }
    REQUIRE_FALSE(validate_scene_viewport_request(request));
}
