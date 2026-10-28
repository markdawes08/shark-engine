#include <shark/scene.hpp>
#include <shark/schema.hpp>

#include "scene_edit_access.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <thread>

namespace {

template <typename T> T take(shark::Result<T> result) {
    REQUIRE(result);
    return std::move(result).value();
}

template <typename Id> Id id(const char* text) {
    return Id{take(shark::StableId::parse(text))};
}

std::unique_ptr<shark::Scene> scene() {
    return take(shark::Scene::create(
        shark::AuthoredSceneScope{id<shark::SceneId>("00000000-0000-4000-8000-000000000001")}));
}

shark::EntitySeed seed() {
    return {id<shark::AuthoredEntityId>("00000000-0000-4000-8000-000000000002")};
}

shark::MeshRenderer renderer() {
    return {id<shark::AssetId>("00000000-0000-4000-8000-000000000003"), 7, false};
}

shark::SceneRevision revision(const shark::Scene& owner) {
    const auto stats = take(owner.stats());
    return {stats.instance, stats.revision};
}

} // namespace

TEST_CASE("camera validates both projections and inactive fields without mutation",
          "[render-components]") {
    using shark::Camera;
    using shark::CameraProjection;
    const auto defaults = take(shark::validate_camera({}));
    CHECK(defaults.projection == CameraProjection::perspective);
    CHECK(defaults.near_clip == 0.1);
    CHECK(defaults.far_clip == 1000.0);
    CHECK(defaults.vertical_fov_radians == 1.0471975511965976);
    CHECK(defaults.orthographic_height == 10.0);
    for (const auto projection : {CameraProjection::perspective, CameraProjection::orthographic}) {
        Camera value;
        value.projection = projection;
        CHECK(shark::validate_camera(value));
        value.near_clip = 0.0;
        CHECK(shark::validate_camera(value).has_value() ==
              (projection == CameraProjection::orthographic));
        value.near_clip = -1.0;
        CHECK_FALSE(shark::validate_camera(value));
        value.near_clip = 0.1;
        for (const auto far : {0.1, 0.0, -1.0}) {
            value.far_clip = far;
            CHECK_FALSE(shark::validate_camera(value));
        }
        value.far_clip = 1000.0;
        for (const auto invalid :
             {0.0, -1.0, std::numbers::pi, std::numeric_limits<double>::infinity(),
              std::numeric_limits<double>::quiet_NaN()}) {
            value.vertical_fov_radians = invalid;
            CHECK_FALSE(shark::validate_camera(value));
        }
        value.vertical_fov_radians = defaults.vertical_fov_radians;
        for (const auto invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                                   std::numeric_limits<double>::quiet_NaN()}) {
            value.orthographic_height = invalid;
            CHECK_FALSE(shark::validate_camera(value));
        }
        value.orthographic_height = defaults.orthographic_height;
        for (auto field : {&Camera::near_clip, &Camera::far_clip}) {
            for (const auto invalid : {std::numeric_limits<double>::infinity(),
                                       std::numeric_limits<double>::quiet_NaN()}) {
                auto candidate = value;
                candidate.*field = invalid;
                CHECK_FALSE(shark::validate_camera(candidate));
            }
        }
    }
    Camera invalid;
    invalid.projection = static_cast<CameraProjection>(2);
    const auto failure = shark::validate_camera(invalid);
    REQUIRE_FALSE(failure);
    CHECK(failure.error().code == shark::ErrorCode::invalid_argument);
    CHECK(failure.error().context.operation == "camera.validate");
    CHECK(shark::validate_mesh_renderer(renderer()));
    auto maximum_mesh = renderer();
    maximum_mesh.mesh_index = std::numeric_limits<std::uint32_t>::max();
    CHECK(shark::validate_mesh_renderer(maximum_mesh));
}

TEST_CASE("render schemas expose optional typed fields and require an explicit asset",
          "[render-components]") {
    const auto camera = take(shark::component_schema(shark::ComponentSchemaId::camera));
    CHECK(camera.stable_id == "shark.camera");
    CHECK_FALSE(camera.required);
    CHECK(camera.removable);
    REQUIRE(camera.properties.size() == 5);
    CHECK(camera.properties[0].id == shark::PropertyId::camera_projection);
    CHECK(std::get<std::uint32_t>(camera.properties[0].default_value) == 0);
    CHECK(std::get<shark::EnumerationConstraints>(camera.properties[0].constraints).values ==
          std::vector<shark::EnumerationValue>{{0, "perspective"}, {1, "orthographic"}});
    CHECK(camera.properties[1].unit == shark::PropertyUnit::radians);
    const auto fov = std::get<shark::ScalarConstraints>(camera.properties[1].constraints);
    CHECK(fov.minimum == 0.0);
    CHECK(fov.maximum == std::numbers::pi);
    CHECK_FALSE(fov.minimum_inclusive);
    CHECK_FALSE(fov.maximum_inclusive);
    const auto mesh = take(shark::component_schema(shark::ComponentSchemaId::mesh_renderer));
    CHECK(mesh.stable_id == "shark.mesh_renderer");
    CHECK_FALSE(mesh.required);
    CHECK(mesh.removable);
    REQUIRE(mesh.properties.size() == 3);
    CHECK(mesh.properties[0].key == "asset_id");
    CHECK(mesh.properties[0].type == shark::PropertyType::asset_id);
    CHECK(std::holds_alternative<std::monostate>(mesh.properties[0].default_value));
    CHECK(std::get<shark::AssetIdConstraints>(mesh.properties[0].constraints).required);
    CHECK(std::get<std::uint32_t>(mesh.properties[1].default_value) == 0);
    CHECK(std::get<bool>(mesh.properties[2].default_value));
    CHECK(shark::validate_component(shark::ComponentSchemaId::camera, 1, shark::Camera{}));
    CHECK(shark::validate_component(shark::ComponentSchemaId::mesh_renderer, 1, renderer()));
    CHECK_FALSE(shark::validate_component(shark::ComponentSchemaId::camera, 1, renderer()));
    CHECK_FALSE(
        shark::validate_component(shark::ComponentSchemaId::mesh_renderer, 1, shark::Camera{}));
    auto invalid = shark::Camera{};
    invalid.far_clip = invalid.near_clip;
    CHECK_FALSE(shark::validate_component(shark::ComponentSchemaId::camera, 1, invalid));
}

TEST_CASE("scene render components attach replace remove and preserve no-op revisions",
          "[render-components]") {
    auto owner = scene();
    const auto handle = take(owner->create_entity(seed()));
    const auto initial = revision(*owner);
    REQUIRE(owner->set_camera(handle, std::nullopt));
    REQUIRE(owner->set_mesh_renderer(handle, std::nullopt));
    CHECK(revision(*owner) == initial);
    REQUIRE(owner->set_camera(handle, shark::Camera{}));
    REQUIRE(owner->set_mesh_renderer(handle, renderer()));
    const auto attached = revision(*owner);
    CHECK(attached.revision == initial.revision + 2);
    REQUIRE(owner->set_camera(handle, shark::Camera{}));
    REQUIRE(owner->set_mesh_renderer(handle, renderer()));
    CHECK(revision(*owner) == attached);
    auto invalid = shark::Camera{};
    invalid.near_clip = -1;
    const auto failure = owner->set_camera(handle, invalid);
    REQUIRE_FALSE(failure);
    CHECK(failure.error().context.operation == "scene.set-camera");
    CHECK(failure.error().context.entity_id == "00000000-0000-4000-8000-000000000002");
    CHECK(revision(*owner) == attached);
    CHECK(take(owner->entity_state(handle)).camera == shark::Camera{});
    auto changed = renderer();
    changed.visible = true;
    changed.mesh_index = 2;
    REQUIRE(owner->set_mesh_renderer(handle, changed));
    CHECK(take(owner->entity_state(handle)).mesh_renderer == changed);
    REQUIRE(owner->set_camera(handle, std::nullopt));
    REQUIRE(owner->set_mesh_renderer(handle, std::nullopt));
    const auto state = take(owner->entity_state(handle));
    CHECK_FALSE(state.camera);
    CHECK_FALSE(state.mesh_renderer);
    REQUIRE(owner->destroy_entity(handle));
    const auto replacement = take(owner->create_entity(seed()));
    CHECK(replacement != handle);
    CHECK_FALSE(owner->set_camera(handle, shark::Camera{}));
    CHECK_FALSE(owner->set_mesh_renderer(handle, renderer()));
    CHECK_FALSE(take(owner->entity_state(replacement)).camera);
    CHECK_FALSE(take(owner->entity_state(replacement)).mesh_renderer);
}

TEST_CASE("scene render seed validation is atomic and optional state survives candidate copies",
          "[render-components]") {
    auto owner = scene();
    auto input = seed();
    input.camera = shark::Camera{};
    input.camera->far_clip = 0.0;
    input.mesh_renderer = renderer();
    CHECK_FALSE(owner->create_entity(input));
    CHECK(take(owner->stats()).live_entities == 0);
    CHECK(take(owner->stats()).revision == 0);
    input.camera = shark::Camera{};
    const auto handle = take(owner->create_entity(input));
    const auto expected = revision(*owner);
    auto candidate = take(shark::scene_detail::SceneEditAccess::prepare(*owner, expected));
    CHECK(take(candidate->entity_state(handle)).camera == input.camera);
    CHECK(take(candidate->entity_state(handle)).mesh_renderer == input.mesh_renderer);
    REQUIRE(candidate->set_camera(handle, std::nullopt));
    auto changed = renderer();
    changed.visible = true;
    REQUIRE(candidate->set_mesh_renderer(handle, changed));
    CHECK(take(owner->entity_state(handle)).camera == input.camera);
    CHECK(take(owner->entity_state(handle)).mesh_renderer == input.mesh_renderer);
    const auto committed =
        take(shark::scene_detail::SceneEditAccess::commit(*owner, *candidate, expected));
    CHECK(committed.revision == expected.revision + 1);
    CHECK(take(owner->entity_state(handle)).reference.handle == handle);
    CHECK_FALSE(take(owner->entity_state(handle)).camera);
    CHECK(take(owner->entity_state(handle)).mesh_renderer == changed);
}

TEST_CASE("scene render queries are opt-in owned snapshots with revision invalidation",
          "[render-components]") {
    auto owner = scene();
    auto input = seed();
    input.camera = shark::Camera{};
    input.mesh_renderer = renderer();
    const auto handle = take(owner->create_entity(input));
    const auto identity = take(owner->inspect_entity(handle, {false, false, false}));
    CHECK_FALSE(identity.entities()[0].camera);
    CHECK_FALSE(identity.entities()[0].mesh_renderer);
    const auto snapshot = take(owner->inspect_entity(handle, {false, false, false, true, true}));
    CHECK(snapshot.entities()[0].camera == input.camera);
    CHECK(snapshot.entities()[0].mesh_renderer == input.mesh_renderer);
    shark::SceneQueryOptions options;
    options.fields = {false, false, false, true, true};
    options.expected_revision = snapshot.revision();
    const auto all = take(owner->query_entities(options));
    REQUIRE(all.entities().size() == 1);
    CHECK(all.entities()[0].camera == input.camera);
    REQUIRE(owner->set_camera(handle, std::nullopt));
    CHECK_FALSE(owner->query_entities(options));
    CHECK(snapshot.entities()[0].camera == input.camera);
    CHECK(snapshot.entities()[0].mesh_renderer == input.mesh_renderer);
}

TEST_CASE("deferred render edits respect visits FIFO failures and owner lifetime",
          "[render-components]") {
    auto owner = scene();
    const auto handle = take(owner->create_entity(seed()));
    auto invalid = shark::Camera{};
    invalid.near_clip = 0.0;
    CHECK_FALSE(owner->defer_set_camera(handle, invalid));
    CHECK(take(owner->stats()).pending_mutations == 0);
    REQUIRE(owner->visit_entities([&](const shark::EntityReference&) -> shark::Result<void> {
        CHECK_FALSE(owner->set_camera(handle, shark::Camera{}));
        CHECK_FALSE(owner->set_mesh_renderer(handle, renderer()));
        REQUIRE(owner->defer_set_camera(handle, shark::Camera{}));
        REQUIRE(owner->defer_set_mesh_renderer(handle, renderer()));
        return {};
    }));
    CHECK_FALSE(owner->set_camera(handle, shark::Camera{}));
    CHECK_FALSE(take(owner->entity_state(handle)).camera);
    const auto applied = take(owner->apply_deferred());
    REQUIRE(applied.outcomes.size() == 2);
    CHECK(applied.outcomes[0].kind == shark::SceneMutationKind::set_camera);
    CHECK(applied.outcomes[1].kind == shark::SceneMutationKind::set_mesh_renderer);
    CHECK(applied.outcomes[0].result);
    CHECK(applied.outcomes[1].result);
    CHECK(applied.revision_after == applied.revision_before + 2);
    REQUIRE(owner->defer_destroy(handle));
    REQUIRE(owner->defer_set_camera(handle, std::nullopt));
    REQUIRE(owner->defer_set_mesh_renderer(handle, std::nullopt));
    const auto removed = take(owner->apply_deferred());
    REQUIRE(removed.outcomes.size() == 3);
    CHECK(removed.outcomes[0].result);
    CHECK_FALSE(removed.outcomes[1].result);
    CHECK_FALSE(removed.outcomes[2].result);
    const auto replacement = take(owner->create_entity(seed()));
    bool camera_rejected = false;
    bool mesh_rejected = false;
    const auto mesh = renderer();
    std::thread other([&] {
        camera_rejected = !owner->set_camera(replacement, shark::Camera{});
        mesh_rejected = !owner->defer_set_mesh_renderer(replacement, mesh);
    });
    other.join();
    CHECK(camera_rejected);
    CHECK(mesh_rejected);
    REQUIRE(owner->close());
    CHECK_FALSE(owner->set_camera(replacement, shark::Camera{}));
    CHECK_FALSE(owner->set_mesh_renderer(replacement, renderer()));
}
