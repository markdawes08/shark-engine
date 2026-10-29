// Original Shark CPU extraction fixtures; project MIT license.
#include "shark/prepared_scene.hpp"
#include "shark/scene_render.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <thread>

namespace {
using namespace shark;

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        INFO(result.error().context.operation);
    }
    REQUIRE(result);
    return std::move(result).value();
}
template <typename Id> Id id(std::uint32_t value) {
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 0xfU];
        value >>= 4U;
    }
    return Id{take(StableId::parse(text))};
}
std::unique_ptr<Scene> scene(SceneLimits limits = {}) {
    return take(Scene::create(AuthoredSceneScope{id<SceneId>(1)}, limits));
}
SceneRevision revision(const Scene& owner) {
    const auto stats = take(owner.stats());
    return {stats.instance, stats.revision};
}
EntityHandle camera(Scene& owner, math::Vec3 position = math::Vec3{0.0}) {
    EntitySeed seed{id<AuthoredEntityId>(1)};
    seed.transform.translation = position;
    seed.camera = Camera{};
    seed.camera->vertical_fov_radians = std::numbers::pi / 2.0;
    seed.camera->near_clip = 1.0;
    seed.camera->far_clip = 10.0;
    return take(owner.create_entity(std::move(seed)));
}
EntityHandle mesh(Scene& owner, std::uint32_t number, math::Vec3 position,
                  AssetId asset = builtin_cube_asset_id(), bool visible = true) {
    EntitySeed seed{id<AuthoredEntityId>(number)};
    seed.transform.translation = position;
    seed.mesh_renderer = MeshRenderer{asset, 0, visible};
    return take(owner.create_entity(std::move(seed)));
}
RenderExtractionOptions options(const Scene& owner, EntityHandle active) {
    return {active, revision(owner), {800, 800}};
}
void equal_vec(const math::Vec3& actual, const math::Vec3& expected) {
    for (int axis = 0; axis < 3; ++axis)
        CHECK(actual[axis] == Catch::Approx(expected[axis]).margin(1e-10));
}
void equal_matrix(const math::Mat4& actual, const math::Mat4& expected) {
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            CHECK(actual[column][row] == Catch::Approx(expected[column][row]).margin(1e-10));
}
void rejected(const Result<RenderScene>& result) {
    REQUIRE_FALSE(result);
    CHECK_FALSE(result.error().context.operation.empty());
}
} // namespace

TEST_CASE("extraction shares geometry computes bounds once and distinguishes hidden and culled",
          "[scene-render]") {
    const auto assets = take(PreparedAssets::create());
    const auto owner = scene();
    const auto active = camera(*owner);
    const auto first = mesh(*owner, 2, {0, 0, -5});
    const auto second = mesh(*owner, 3, {2, 0, -5});
    static_cast<void>(mesh(*owner, 4, {0, 0, -5}, builtin_cube_asset_id(), false));
    const auto outside = mesh(*owner, 5, {20, 0, -5});
    const auto plane = mesh(*owner, 6, {0, -1, -5}, builtin_plane_asset_id());
    auto request = options(*owner, active);
    const auto result = take(extract_render_scene(*owner, *assets, request));
    CHECK(result.revision() == request.expected_revision);
    CHECK(result.camera_entity().handle == active);
    CHECK(result.viewport() == request.viewport);
    REQUIRE(result.records().size() == 3);
    CHECK(result.records()[0].entity.handle == first);
    CHECK(result.records()[1].entity.handle == second);
    CHECK(result.records()[2].entity.handle == plane);
    CHECK(result.records()[0].asset == result.records()[1].asset);
    CHECK(result.records()[0].asset == take(assets->find(builtin_cube_asset_id())));
    equal_vec(result.records()[0].local_bounds.minimum, {-0.5, -0.5, -0.5});
    equal_vec(result.records()[0].local_bounds.maximum, {0.5, 0.5, 0.5});
    equal_vec(result.records()[0].world_bounds.minimum, {-0.5, -0.5, -5.5});
    equal_vec(result.records()[0].world_bounds.maximum, {0.5, 0.5, -4.5});
    equal_vec(result.records()[2].local_bounds.minimum, {-0.5, 0, -0.5});
    equal_vec(result.records()[2].local_bounds.maximum, {0.5, 0, 0.5});
    equal_vec(result.records()[2].world_bounds.minimum, {-0.5, -1, -5.5});
    equal_vec(result.records()[2].world_bounds.maximum, {0.5, -1, -4.5});
    CHECK_FALSE(result.records()[0].mirrored);
    const auto stats = result.stats();
    CHECK(stats.entities == 6);
    CHECK(stats.mesh_instances == 5);
    CHECK(stats.hidden == 1);
    CHECK(stats.culled == 1);
    CHECK(stats.visible == 3);
    CHECK(stats.unique_meshes == 2);
    CHECK(stats.bounds_vertices == 28);
    CHECK(revision(*owner) == request.expected_revision);

    request.frustum_culling = false;
    const auto uncull = take(extract_render_scene(*owner, *assets, request));
    REQUIRE(uncull.records().size() == 4);
    CHECK(uncull.records()[2].entity.handle == outside);
    CHECK(uncull.stats().hidden == 1);
    CHECK(uncull.stats().culled == 0);
    CHECK(uncull.stats().visible == 4);
    CHECK(uncull.stats().bounds_vertices == 28);
}

TEST_CASE("extraction retains hierarchy shear reflection and inverse transpose normals",
          "[scene-render]") {
    const auto assets = take(PreparedAssets::create());
    const auto owner = scene();
    const auto active = camera(*owner);
    Camera orthographic;
    orthographic.projection = CameraProjection::orthographic;
    orthographic.orthographic_height = 20;
    orthographic.near_clip = 0;
    orthographic.far_clip = 20;
    REQUIRE(owner->set_camera(active, orthographic));
    EntitySeed parent{id<AuthoredEntityId>(2)};
    parent.transform.translation = {1, 2, -6};
    parent.transform.scale = {2, 1, 1};
    const auto parent_handle = take(owner->create_entity(std::move(parent)));
    EntitySeed child{id<AuthoredEntityId>(3)};
    child.parent = parent_handle;
    child.transform.rotation =
        math::Quat{std::cos(std::numbers::pi / 8), 0, 0, std::sin(std::numbers::pi / 8)};
    child.transform.scale = {-1, 1, 1};
    child.mesh_renderer = MeshRenderer{builtin_cube_asset_id()};
    const auto child_handle = take(owner->create_entity(std::move(child)));
    const auto result = take(extract_render_scene(*owner, *assets, options(*owner, active)));
    REQUIRE(result.records().size() == 1);
    const auto& record = result.records()[0];
    CHECK(record.entity.handle == child_handle);
    CHECK(record.mirrored);
    const double q = std::sqrt(0.5);
    math::Mat4 expected{1.0};
    expected[0] = {-2 * q, -q, 0, 0};
    expected[1] = {-2 * q, q, 0, 0};
    expected[3] = {1, 2, -6, 1};
    equal_matrix(record.world_matrix, expected);
    math::Mat4 normal{1.0};
    normal[0] = {-q / 2, -q, 0, 0};
    normal[1] = {-q / 2, q, 0, 0};
    equal_matrix(record.normal_matrix, normal);
    equal_vec(record.world_bounds.minimum, {1 - 2 * q, 2 - q, -6.5});
    equal_vec(record.world_bounds.maximum, {1 + 2 * q, 2 + q, -5.5});
    // A transformed X tangent remains perpendicular to the transformed Y normal.
    CHECK(glm::dot(math::Vec3{record.world_matrix[0]}, math::Vec3{record.normal_matrix[1]}) ==
          Catch::Approx(0.0).margin(1e-10));
}

TEST_CASE("transform previews recompose descendants before bounds normals and culling",
          "[scene-render]") {
    const auto assets = take(PreparedAssets::create());
    const auto owner = scene();
    const auto active = camera(*owner);
    Camera orthographic;
    orthographic.projection = CameraProjection::orthographic;
    orthographic.orthographic_height = 20;
    orthographic.near_clip = 0;
    orthographic.far_clip = 20;
    REQUIRE(owner->set_camera(active, orthographic));
    EntitySeed parent{id<AuthoredEntityId>(2)};
    parent.transform.translation = {100, 2, -6};
    parent.transform.scale = {2, 1, 1};
    const auto root = take(owner->create_entity(parent));
    EntitySeed child{id<AuthoredEntityId>(3)};
    child.parent = root;
    child.transform.rotation =
        math::Quat{std::cos(std::numbers::pi / 8), 0, 0, std::sin(std::numbers::pi / 8)};
    child.transform.scale = {-1, 1, 1};
    child.mesh_renderer = MeshRenderer{builtin_cube_asset_id()};
    const auto object = take(owner->create_entity(child));
    auto request = options(*owner, active);
    const auto baseline = take(extract_render_scene(*owner, *assets, request));
    REQUIRE(baseline.records().empty());
    CHECK_FALSE(baseline.transform_preview());
    auto candidate = parent.transform;
    candidate.translation.x = 1;
    request.transform_preview = RenderTransformOverride{root, candidate};
    request.origin.scene_local_offset = {1, 2, 0};
    request.origin.generation = 7;
    const auto preview = take(extract_render_scene(*owner, *assets, request));
    REQUIRE(preview.records().size() == 1);
    REQUIRE(preview.transform_preview());
    CHECK(*preview.transform_preview() == *request.transform_preview);
    CHECK(preview.revision() == request.expected_revision);
    CHECK(preview.origin() == request.origin);
    CHECK(preview.records()[0].entity.handle == object);
    CHECK(preview.records()[0].mirrored);
    const auto q = std::sqrt(0.5);
    math::Mat4 expected{1.0};
    expected[0] = {-2 * q, -q, 0, 0};
    expected[1] = {-2 * q, q, 0, 0};
    expected[3] = {0, 0, -6, 1};
    equal_matrix(preview.records()[0].world_matrix, expected);
    math::Mat4 normal{1.0};
    normal[0] = {-q / 2, -q, 0, 0};
    normal[1] = {-q / 2, q, 0, 0};
    equal_matrix(preview.records()[0].normal_matrix, normal);
    equal_vec(preview.records()[0].world_bounds.minimum, {-2 * q, -q, -6.5});
    equal_vec(preview.records()[0].world_bounds.maximum, {2 * q, q, -5.5});
    CHECK(take(owner->entity_state(root)).local_transform == parent.transform);
    CHECK(revision(*owner) == request.expected_revision);
    REQUIRE(owner->set_transform(root, candidate));
    request.expected_revision = revision(*owner);
    request.transform_preview.reset();
    const auto committed = take(extract_render_scene(*owner, *assets, request));
    REQUIRE(committed.records().size() == 1);
    equal_matrix(committed.records()[0].world_matrix, preview.records()[0].world_matrix);
    equal_matrix(committed.records()[0].normal_matrix, preview.records()[0].normal_matrix);
    CHECK(committed.records()[0].world_bounds == preview.records()[0].world_bounds);
    CHECK_FALSE(committed.transform_preview());
}

TEST_CASE("transform previews move descendant cameras unless a display view is explicit",
          "[scene-render]") {
    const auto assets = take(PreparedAssets::create());
    const auto owner = scene();
    const auto active = camera(*owner);
    const auto root = take(owner->create_entity(EntitySeed{id<AuthoredEntityId>(2)}));
    REQUIRE(owner->reparent(active, root, ReparentMode::keep_local));
    const auto object = mesh(*owner, 3, {20, 0, -5});
    auto request = options(*owner, active);
    const auto baseline = take(extract_render_scene(*owner, *assets, request));
    CHECK(baseline.records().empty());
    Transform candidate;
    candidate.translation.x = 20;
    request.transform_preview = RenderTransformOverride{root, candidate};
    const auto preview = take(extract_render_scene(*owner, *assets, request));
    REQUIRE(preview.records().size() == 1);
    CHECK(preview.records()[0].entity.handle == object);
    CHECK(preview.camera().view[3].x == -20);
    CHECK(preview.camera_entity() == baseline.camera_entity());
    CHECK(take(owner->entity_state(active)).world_matrix[3].x == 0);
    CHECK(revision(*owner) == request.expected_revision);
    request.view_camera =
        RenderViewCamera{*take(owner->entity_state(active)).camera, math::Mat4{1}};
    const auto fixed_view = take(extract_render_scene(*owner, *assets, request));
    CHECK(fixed_view.records().empty());
    CHECK(fixed_view.camera().view[3].x == 0);
    CHECK(fixed_view.transform_preview());
}

TEST_CASE("transform previews reject stale targets and invalid hidden descendant worlds",
          "[scene-render]") {
    const auto assets = take(PreparedAssets::create());
    const auto owner = scene();
    const auto active = camera(*owner);
    const auto root = take(owner->create_entity(EntitySeed{id<AuthoredEntityId>(2)}));
    EntitySeed descendant{id<AuthoredEntityId>(3)};
    descendant.parent = root;
    descendant.transform.scale = {1e200, 1e200, 1e200};
    const auto child = take(owner->create_entity(descendant));
    auto request = options(*owner, active);
    request.transform_preview = RenderTransformOverride{root, Transform{}};
    SECTION("non-renderable descendant still rejects overflow") {
        request.transform_preview->local.scale = {1e200, 1e200, 1e200};
    }
    SECTION("hidden descendant still rejects overflow") {
        REQUIRE(owner->set_mesh_renderer(child, MeshRenderer{builtin_cube_asset_id(), 0, false}));
        request.expected_revision = revision(*owner);
        request.transform_preview->local.scale = {1e200, 1e200, 1e200};
    }
    SECTION("singular local override") {
        request.transform_preview->local.scale.x = 0;
    }
    SECTION("foreign target") {
        auto foreign = scene();
        request.transform_preview->entity = camera(*foreign);
    }
    SECTION("stale generation") {
        ++request.transform_preview->entity.generation;
    }
    rejected(extract_render_scene(*owner, *assets, request));
    CHECK(revision(*owner) == request.expected_revision);
    CHECK(take(owner->entity_state(root)).local_transform == Transform{});
}

TEST_CASE("display camera overrides visibility before culling without changing the authored view",
          "[scene-render]") {
    const auto assets = take(PreparedAssets::create());
    const auto owner = scene();
    const auto active = camera(*owner);
    const auto original_object = mesh(*owner, 2, {0, 0, -5});
    const auto navigated_object = mesh(*owner, 3, {20, 0, -5});
    auto request = options(*owner, active);
    const auto original = take(extract_render_scene(*owner, *assets, request));
    REQUIRE(original.records().size() == 1);
    CHECK(original.records()[0].entity.handle == original_object);
    const auto authored = take(owner->entity_state(active));

    Camera display;
    display.projection = CameraProjection::orthographic;
    display.orthographic_height = 4;
    display.near_clip = 1;
    display.far_clip = 10;
    math::Mat4 display_world{1.0};
    display_world[0].x = -1;
    display_world[3].x = 20;
    request.view_camera = RenderViewCamera{display, display_world};
    request.viewport = {800, 400};
    request.origin = {{19, 0, 0}, 3, WorldPosition{id<WorldId>(99), {8, -2, 4}, {0, 0, 0}}};
    const auto navigated = take(extract_render_scene(*owner, *assets, request));
    REQUIRE(navigated.records().size() == 1);
    CHECK(navigated.records()[0].entity.handle == navigated_object);
    CHECK(navigated.stats().culled == 1);
    CHECK(navigated.camera_entity() == original.camera_entity());
    CHECK(navigated.revision() == original.revision());
    CHECK(navigated.origin() == request.origin);
    CHECK(navigated.viewport() == request.viewport);
    CHECK(navigated.camera().mirrored);
    CHECK(navigated.camera().view[0].x == -1);
    CHECK(navigated.camera().view[3].x == 1);
    CHECK(navigated.camera().projection[0].x == Catch::Approx(0.25));
    CHECK(navigated.camera().projection[1].y == Catch::Approx(-0.5));
    const auto clip = navigated.camera().view_projection * navigated.records()[0].world_matrix *
                      glm::dvec4{0, 0, 0, 1};
    equal_vec(math::Vec3{clip}, {0, 0, 4.0 / 9.0});
    CHECK(clip.w == 1);
    equal_vec(math::Vec3{navigated.records()[0].world_matrix[3]}, {1, 0, -5});

    request.origin.scene_local_offset.x += 3;
    ++request.origin.generation;
    const auto shifted = take(extract_render_scene(*owner, *assets, request));
    REQUIRE(shifted.records().size() == 1);
    equal_matrix(shifted.camera().view_projection * shifted.records()[0].world_matrix,
                 navigated.camera().view_projection * navigated.records()[0].world_matrix);
    const auto unchanged = take(owner->entity_state(active));
    CHECK(unchanged.camera == authored.camera);
    CHECK(unchanged.local_transform == authored.local_transform);
    CHECK(revision(*owner) == request.expected_revision);
    request.view_camera.reset();
    request.origin = {};
    request.viewport = original.viewport();
    const auto restored = take(extract_render_scene(*owner, *assets, request));
    REQUIRE(restored.records().size() == 1);
    CHECK(restored.records()[0].entity.handle == original_object);
    equal_matrix(restored.camera().view_projection, original.camera().view_projection);
}

TEST_CASE("display camera rejects invalid values and cannot bypass source camera ownership",
          "[scene-render]") {
    const auto assets = take(PreparedAssets::create());
    const auto owner = scene();
    const auto active = camera(*owner);
    const auto object = mesh(*owner, 2, {0, 0, -5});
    auto request = options(*owner, active);
    request.view_camera = RenderViewCamera{};
    SECTION("invalid projection") {
        request.view_camera->camera.projection = static_cast<CameraProjection>(99);
    }
    SECTION("singular world matrix") {
        request.view_camera->scene_local_world[0] = glm::dvec4{0};
    }
    SECTION("nonfinite world matrix") {
        request.view_camera->scene_local_world[3].x = std::numeric_limits<double>::infinity();
    }
    SECTION("nonaffine world matrix") {
        request.view_camera->scene_local_world[0].w = 1;
    }
    SECTION("unbounded camera relative translation") {
        request.view_camera->scene_local_world[3].x = render_coordinate_limit + 1;
    }
    SECTION("source camera remains required") {
        request.camera = object;
    }
    SECTION("stale source revision remains rejected") {
        ++request.expected_revision.revision;
    }
    const auto before = revision(*owner);
    const auto result = extract_render_scene(*owner, *assets, request);
    rejected(result);
    CHECK(revision(*owner) == before);
}

TEST_CASE("extraction subtracts the explicit local origin without flattening global cells",
          "[scene-render]") {
    const auto assets = take(PreparedAssets::create());
    const auto owner = scene();
    const auto active = camera(*owner, {1'000'000'000.25, -2, 3});
    const auto object = mesh(*owner, 2, {1'000'000'000.25, -2, -2});
    auto request = options(*owner, active);
    request.origin = {
        {1'000'000'000.0, -2, 3},
        7,
        WorldPosition{id<WorldId>(99), {world_cell_max, world_cell_min, 42}, {0.125, 1023.75, 0}}};
    const auto result = take(extract_render_scene(*owner, *assets, request));
    CHECK(result.origin() == request.origin);
    REQUIRE(result.records().size() == 1);
    equal_vec(math::Vec3{result.records()[0].world_matrix[3]}, {0.25, 0, -5});
    equal_vec(result.records()[0].world_bounds.minimum, {-0.25, -0.5, -5.5});
    equal_vec(result.records()[0].world_bounds.maximum, {0.75, 0.5, -4.5});
    equal_vec(math::Vec3{result.camera().view[3]}, {-0.25, 0, 0});
    equal_vec(take(owner->entity_state(object)).local_transform.translation,
              {1'000'000'000.25, -2, -2});

    request.origin.scene_local_offset.x += 4;
    ++request.origin.generation;
    const auto shifted = take(extract_render_scene(*owner, *assets, request));
    REQUIRE(shifted.records().size() == 1);
    CHECK(shifted.origin().generation == 8);
    CHECK(result.origin().generation == 7);
    equal_matrix(shifted.camera().view_projection * shifted.records()[0].world_matrix,
                 result.camera().view_projection * result.records()[0].world_matrix);
    CHECK(revision(*owner) == request.expected_revision);
}

TEST_CASE("extraction rejects stale revisions invalid camera ownership and inaccessible scenes",
          "[scene-render]") {
    const auto assets = take(PreparedAssets::create());
    const auto owner = scene();
    const auto active = camera(*owner);
    const auto object = mesh(*owner, 2, {0, 0, -5});
    auto request = options(*owner, active);
    SECTION("stale revision") {
        REQUIRE(owner->set_name(object, Name{"Changed"}));
        const auto result = extract_render_scene(*owner, *assets, request);
        rejected(result);
        CHECK(result.error().code == ErrorCode::conflict);
    }
    SECTION("foreign revision") {
        ++request.expected_revision.scene_instance;
        rejected(extract_render_scene(*owner, *assets, request));
    }
    SECTION("camera is required") {
        request.camera = {};
        rejected(extract_render_scene(*owner, *assets, request));
    }
    SECTION("entity without Camera is not selected implicitly") {
        request.camera = object;
        rejected(extract_render_scene(*owner, *assets, request));
    }
    SECTION("foreign camera handle") {
        const auto other = scene();
        request.camera = camera(*other);
        rejected(extract_render_scene(*owner, *assets, request));
    }
    SECTION("removed camera generation") {
        REQUIRE(owner->destroy_entity(active));
        request.expected_revision = revision(*owner);
        rejected(extract_render_scene(*owner, *assets, request));
    }
    SECTION("wrong thread") {
        std::optional<Result<RenderScene>> result;
        std::thread worker([&] { result.emplace(extract_render_scene(*owner, *assets, request)); });
        worker.join();
        REQUIRE(result);
        rejected(*result);
    }
    SECTION("closed owner") {
        REQUIRE(owner->close());
        request.expected_revision = revision(*owner);
        rejected(extract_render_scene(*owner, *assets, request));
    }
}

TEST_CASE("post-load invalid references reject atomically including invisible renderers",
          "[scene-render]") {
    const auto assets = take(PreparedAssets::create());
    PreparedEntity viewpoint{id<AuthoredEntityId>(1)};
    viewpoint.camera = Camera{};
    PreparedEntity object{id<AuthoredEntityId>(2)};
    object.transform.translation = {0, 0, -5};
    object.mesh_renderer = MeshRenderer{builtin_cube_asset_id()};
    const auto bytes = take(encode_prepared_scene({id<SceneId>(1), {}, {viewpoint, object}}));
    const auto loaded = take(LoadedPreparedScene::load(bytes, assets));
    auto& owner = loaded->scene();
    const auto active = take(owner.find(viewpoint.id));
    const auto handle = take(owner.find(object.id));
    const auto prior = take(extract_render_scene(owner, *assets, options(owner, active)));
    REQUIRE(prior.records().size() == 1);
    MeshRenderer invalid{builtin_cube_asset_id(), 1, false};
    SECTION("missing asset") {
        invalid.asset = id<AssetId>(99);
    }
    SECTION("mesh index outside retained asset") {}
    REQUIRE(owner.set_mesh_renderer(handle, invalid));
    const auto before = revision(owner);
    const auto result = extract_render_scene(owner, *assets, options(owner, active));
    rejected(result);
    CHECK(result.error().context.entity_id == object.id.value().to_string());
    CHECK(result.error().context.asset_id == invalid.asset.value().to_string());
    CHECK(revision(owner) == before);
    CHECK(take(owner.entity_state(handle)).mesh_renderer == invalid);
    CHECK(prior.records()[0].asset->meshes[0].primitives[0].vertices.size() == 24);
    REQUIRE(owner.set_mesh_renderer(handle, MeshRenderer{builtin_cube_asset_id()}));
    const auto recovered = take(extract_render_scene(owner, *assets, options(owner, active)));
    CHECK(recovered.records().size() == 1);
}

TEST_CASE("extraction reads committed state through deferred work and visits", "[scene-render]") {
    const auto assets = take(PreparedAssets::create());
    const auto owner = scene();
    const auto active = camera(*owner);
    const auto object = mesh(*owner, 2, {0, 0, -5});
    const auto request = options(*owner, active);
    Transform outside;
    outside.translation = {20, 0, -5};
    const auto ticket = take(owner->defer_set_transform(object, outside));
    const auto pending = take(extract_render_scene(*owner, *assets, request));
    CHECK(pending.records().size() == 1);
    CHECK(revision(*owner) == request.expected_revision);
    CHECK(take(owner->stats()).pending_mutations == 1);
    std::size_t visits = 0;
    REQUIRE(owner->visit_entities([&](const EntityReference&) -> Result<void> {
        const auto inspected = take(extract_render_scene(*owner, *assets, request));
        CHECK(inspected.records().size() == 1);
        ++visits;
        return {};
    }));
    CHECK(visits == 2);
    const auto applied = take(owner->apply_deferred());
    REQUIRE(applied.outcomes.size() == 1);
    CHECK(applied.outcomes[0].ticket == ticket);
    REQUIRE(applied.outcomes[0].result);
    rejected(extract_render_scene(*owner, *assets, request));
    const auto after = take(extract_render_scene(*owner, *assets, options(*owner, active)));
    CHECK(after.records().empty());
    CHECK(after.stats().culled == 1);
    CHECK(pending.records().size() == 1);
}

TEST_CASE("extracted values and immutable geometry survive destruction and fresh scene loading",
          "[scene-render]") {
    auto assets = take(PreparedAssets::create());
    PreparedEntity viewpoint{id<AuthoredEntityId>(1)};
    viewpoint.camera = Camera{};
    PreparedEntity object{id<AuthoredEntityId>(2)};
    object.transform.translation = {0, 0, -5};
    object.mesh_renderer = MeshRenderer{builtin_cube_asset_id()};
    const auto bytes = take(encode_prepared_scene({id<SceneId>(1), {}, {viewpoint, object}}));
    auto loaded = take(LoadedPreparedScene::load(bytes, assets));
    const auto active = take(loaded->scene().find(viewpoint.id));
    auto extracted =
        take(extract_render_scene(loaded->scene(), *assets, options(loaded->scene(), active)));
    REQUIRE(extracted.records().size() == 1);
    const auto old_reference = extracted.records()[0].entity;
    const std::weak_ptr<const PreparedMeshAsset> retained = extracted.records()[0].asset;
    REQUIRE(loaded->scene().destroy_entity(old_reference.handle));
    CHECK(extracted.records()[0].entity == old_reference);
    CHECK(extracted.records()[0].asset->meshes[0].primitives[0].indices.size() == 36);
    loaded.reset();
    assets.reset();
    CHECK_FALSE(retained.expired());

    const auto replacement_assets = take(PreparedAssets::create());
    const auto replacement = take(LoadedPreparedScene::load(bytes, replacement_assets));
    CHECK_FALSE(replacement->scene().resolve(old_reference));
    const auto new_camera = take(replacement->scene().find(viewpoint.id));
    const auto fresh = take(extract_render_scene(replacement->scene(), *replacement_assets,
                                                 options(replacement->scene(), new_camera)));
    REQUIRE(fresh.records().size() == 1);
    CHECK(fresh.records()[0].entity.identity == old_reference.identity);
    CHECK(fresh.records()[0].entity.handle.scene_instance != old_reference.handle.scene_instance);
    CHECK(fresh.records()[0].asset != extracted.records()[0].asset);
    equal_vec(extracted.records()[0].world_bounds.minimum, {-0.5, -0.5, -5.5});
}

TEST_CASE("extraction validates local render range before culling", "[scene-render]") {
    auto assets = take(PreparedAssets::create());
    const auto owner = scene();
    const auto active = camera(*owner);
    const auto object = mesh(*owner, 2, {0, 0, -5});
    auto request = options(*owner, active);
    SECTION("outside mesh origin cannot be silently culled") {
        Transform beyond;
        beyond.translation = {render_coordinate_limit + 1, 0, -5};
        REQUIRE(owner->set_transform(object, beyond));
    }
    SECTION("bounds outside range despite admitted object origin") {
        Transform beyond;
        beyond.translation = {render_coordinate_limit, 0, -5};
        REQUIRE(owner->set_transform(object, beyond));
    }
    SECTION("local mesh vertices outside range") {
        auto large = *take(assets->find(builtin_cube_asset_id()));
        large.asset = id<AssetId>(99);
        for (auto& vertex : large.meshes[0].primitives[0].vertices)
            vertex.position[0] += static_cast<float>(render_coordinate_limit + 1);
        const auto payload = take(encode_prepared_mesh(large));
        const std::array inputs{PreparedAssetInput{large.asset, payload}};
        assets = take(PreparedAssets::create(inputs));
        REQUIRE(owner->set_mesh_renderer(object, MeshRenderer{large.asset}));
    }
    SECTION("unusable viewport") {
        request.viewport.width = 0;
    }
    SECTION("nonfinite origin") {
        request.origin.scene_local_offset.x = std::numeric_limits<double>::infinity();
    }
    SECTION("zero origin generation") {
        request.origin.generation = 0;
    }
    request.expected_revision = revision(*owner);
    rejected(extract_render_scene(*owner, *assets, request));
    CHECK(revision(*owner) == request.expected_revision);
}

TEST_CASE("extraction spans query pages up to its entity cap without duplicating shared work",
          "[scene-render]") {
    const auto assets = take(PreparedAssets::create());
    const auto owner = scene({render_scene_maximum_entities + 1, 1024, 256});
    const auto active = camera(*owner);
    for (std::uint32_t index = 2; index <= render_scene_maximum_entities; ++index)
        static_cast<void>(mesh(*owner, index, {0, 0, -5}));
    const auto exact = take(extract_render_scene(*owner, *assets, options(*owner, active)));
    REQUIRE(exact.records().size() == render_scene_maximum_entities - 1);
    CHECK(exact.stats().entities == render_scene_maximum_entities);
    CHECK(exact.stats().visible == render_scene_maximum_entities - 1);
    CHECK(exact.stats().unique_meshes == 1);
    CHECK(exact.stats().bounds_vertices == 24);
    CHECK(exact.records().front().entity.identity == EntityIdentity{id<AuthoredEntityId>(2)});
    CHECK(exact.records().back().entity.identity == EntityIdentity{id<AuthoredEntityId>(4096)});
    static_cast<void>(mesh(*owner, 4097, {0, 0, -5}));
    const auto before = revision(*owner);
    rejected(extract_render_scene(*owner, *assets, options(*owner, active)));
    CHECK(revision(*owner) == before);
    CHECK(exact.records().size() == render_scene_maximum_entities - 1);
}
