#include "../editor/viewport_navigation.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>

namespace {
using namespace shark;
using namespace shark::editor;

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        INFO(result.error().context.operation);
    }
    REQUIRE(result);
    return std::move(result).value();
}
template <typename Id> Id id(std::uint32_t value) {
    std::string text = "17a00000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 0xfU];
        value >>= 4U;
    }
    return Id{take(StableId::parse(text))};
}
SceneRevision revision(const Scene& scene) {
    const auto stats = take(scene.stats());
    return {stats.instance, stats.revision};
}
template <typename T> void rejected(const Result<T>& result, ErrorCode code) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().context.operation.empty());
}
PreparedMeshAsset triangle(std::uint32_t identity, bool double_sided = false,
                           std::size_t count = 1) {
    PreparedPrimitive primitive;
    primitive.material.double_sided = double_sided;
    for (const auto& position : std::array<std::array<float, 3>, 3>{
             std::array<float, 3>{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}}) {
        PreparedVertex vertex;
        vertex.position = position;
        vertex.normal = {0, 0, 1};
        primitive.vertices.push_back(vertex);
    }
    primitive.indices.reserve(count * 3);
    for (std::size_t index = 0; index < count; ++index)
        primitive.indices.insert(primitive.indices.end(), {0, 1, 2});
    return {id<AssetId>(identity), {{std::vector<PreparedPrimitive>{std::move(primitive)}}}};
}
class Fixture {
  public:
    explicit Fixture(bool double_sided = false, std::size_t count = 1) {
        const auto payload = take(encode_prepared_mesh(triangle(99, double_sided, count)));
        const std::array input{PreparedAssetInput{id<AssetId>(99), payload}};
        assets = take(PreparedAssets::create(input));
        owner = take(Scene::create(AuthoredSceneScope{id<SceneId>(1)}));
        camera.vertical_fov_radians = std::numbers::pi / 2;
        camera.orthographic_height = 4;
        camera.near_clip = 1;
        camera.far_clip = 10;
        EntitySeed seed{id<AuthoredEntityId>(1)};
        seed.camera = camera;
        active = take(owner->create_entity(seed));
    }
    EntityHandle mesh(std::uint32_t number, double depth = -5, bool visible = true) {
        EntitySeed seed{id<AuthoredEntityId>(number)};
        seed.transform.translation.z = depth;
        seed.mesh_renderer = MeshRenderer{id<AssetId>(99), 0, visible};
        return take(owner->create_entity(seed));
    }
    RenderScene snapshot(RenderOrigin origin = {}) const {
        return take(
            extract_render_scene(*owner, *assets, {active, revision(*owner), {800, 800}, origin}));
    }
    void orthographic() {
        camera.projection = CameraProjection::orthographic;
        REQUIRE(owner->set_camera(active, camera));
    }
    std::unique_ptr<Scene> owner;
    std::shared_ptr<const PreparedAssets> assets;
    Camera camera;
    EntityHandle active;
};
constexpr ViewportRectangle image{100, 50, 400, 400};
std::optional<ViewportHit> pick(const RenderScene& scene, double x = 300, double y = 250) {
    return take(pick_viewport(scene, scene.revision(), image, x, y));
}
void vector_equal(math::Vec3 actual, math::Vec3 expected) {
    for (int axis = 0; axis < 3; ++axis)
        CHECK(actual[axis] == Catch::Approx(expected[axis]).margin(1e-10));
}
} // namespace

TEST_CASE("Viewport coordinates use actual logical image offsets and half-open edges",
          "[editor-viewport-navigation]") {
    const auto center = take(viewport_coordinates(image, 300, 250));
    REQUIRE(center);
    CHECK(center->u == .5);
    CHECK(center->v == .5);
    // Same texture displayed at another DPI/size must produce the same ray UV.
    const auto scaled = take(viewport_coordinates({-20, 70, 800, 800}, 380, 470));
    REQUIRE(scaled);
    CHECK(scaled->u == center->u);
    CHECK(scaled->v == center->v);
    const auto top_left = take(viewport_coordinates(image, 100, 50));
    REQUIRE(top_left);
    CHECK(top_left->u == 0);
    CHECK(top_left->v == 0);
    CHECK_FALSE(take(viewport_coordinates(image, 500, 250)));
    CHECK_FALSE(take(viewport_coordinates(image, 300, 450)));
    CHECK_FALSE(take(viewport_coordinates(image, 99, 250)));
    rejected(viewport_coordinates({0, 0, 0, 1}, 0, 0), ErrorCode::invalid_argument);
    rejected(viewport_coordinates(image, std::numeric_limits<double>::quiet_NaN(), 0),
             ErrorCode::invalid_argument);
}

TEST_CASE("Viewport picking chooses visible clipped triangles and later equal-depth draws",
          "[editor-viewport-navigation]") {
    Fixture fixture;
    static_cast<void>(fixture.mesh(2));
    static_cast<void>(fixture.mesh(3, -3));
    static_cast<void>(fixture.mesh(4, -3));
    static_cast<void>(fixture.mesh(5, -2, false));
    static_cast<void>(fixture.mesh(6, -.5));
    static_cast<void>(fixture.mesh(7, -12));
    for (const bool orthographic : {false, true}) {
        if (orthographic)
            fixture.orthographic();
        const auto snapshot = fixture.snapshot();
        const auto hit = pick(snapshot);
        REQUIRE(hit);
        CHECK(hit->entity == id<AuthoredEntityId>(4));
        CHECK(hit->revision == snapshot.revision());
        CHECK(hit->distance == Catch::Approx(2).margin(1e-10));
        CHECK_FALSE(pick(snapshot, 499, 449));
        CHECK_FALSE(pick(snapshot, 500, 250));
        auto stale = snapshot.revision();
        ++stale.revision;
        rejected(pick_viewport(snapshot, stale, image, 300, 250), ErrorCode::conflict);
    }
    Fixture empty;
    CHECK_FALSE(pick(empty.snapshot()));
}

TEST_CASE("Viewport picking respects triangle holes winding and full affine hierarchy",
          "[editor-viewport-navigation]") {
    Fixture fixture;
    fixture.orthographic();
    const auto model = fixture.mesh(2);
    auto snapshot = fixture.snapshot();
    REQUIRE(pick(snapshot));
    // World(.8,.8) lies in the bounds but outside the triangular silhouette.
    CHECK_FALSE(pick(snapshot, 380, 170));
    // Upper image point maps to positive camera Y; there is only one projection flip.
    REQUIRE(pick(snapshot, 300, 170));
    CHECK_FALSE(pick(snapshot, 380, 370));

    Transform mirror;
    mirror.translation.z = -5;
    mirror.scale = {-2, 1, 1};
    REQUIRE(fixture.owner->set_transform(model, mirror));
    REQUIRE(pick(fixture.snapshot()));

    EntitySeed parent{id<AuthoredEntityId>(3)};
    parent.transform.translation = {0, 0, -5};
    parent.transform.scale = {2, 1, 1};
    const auto parent_handle = take(fixture.owner->create_entity(parent));
    Transform local;
    local.rotation = glm::angleAxis(std::numbers::pi / 4, math::Vec3{0, 0, 1});
    local.scale = {-1, 1, 1};
    REQUIRE(fixture.owner->set_transform(model, local));
    REQUIRE(fixture.owner->reparent(model, parent_handle, ReparentMode::keep_local));
    snapshot = fixture.snapshot({{7, -2, 1}, 4, {}});
    const auto hit = pick(snapshot);
    REQUIRE(hit);
    CHECK(hit->entity == id<AuthoredEntityId>(2));
    CHECK(hit->distance == Catch::Approx(4).margin(1e-10));

    // Mirrored camera parity is compensated by the existing renderer as well.
    Transform camera_transform;
    camera_transform.scale.x = -1;
    REQUIRE(fixture.owner->set_transform(fixture.active, camera_transform));
    REQUIRE(pick(fixture.snapshot()));

    Fixture back;
    const auto back_model = back.mesh(2);
    Transform reversed;
    reversed.translation.z = -5;
    reversed.rotation = glm::angleAxis(std::numbers::pi, math::Vec3{0, 1, 0});
    REQUIRE(back.owner->set_transform(back_model, reversed));
    CHECK_FALSE(pick(back.snapshot()));
    Fixture double_sided{true};
    const auto double_model = double_sided.mesh(2);
    REQUIRE(double_sided.owner->set_transform(double_model, reversed));
    REQUIRE(pick(double_sided.snapshot()));
}

TEST_CASE("Viewport navigation retains affine seed and publishes only complete display candidates",
          "[editor-viewport-navigation]") {
    Fixture fixture;
    static_cast<void>(fixture.mesh(2));
    ViewportNavigation navigation;
    const auto before = revision(*fixture.owner);
    const auto authored = take(fixture.owner->entity_state(fixture.active));
    REQUIRE(navigation.reset(fixture.snapshot(), fixture.camera));
    REQUIRE(navigation.view());
    CHECK_FALSE(take(navigation.navigate({}, {800, 800}, image)));
    REQUIRE(take(navigation.navigate({0, 0, 40, 20, 0}, {800, 800}, image)));
    vector_equal(math::Vec3{navigation.view()->scene_local_world[3]}, {-.45, .225, 0});
    REQUIRE(take(navigation.navigate({0, 0, 0, 0, 1}, {800, 800}, image)));
    CHECK(navigation.view()->scene_local_world[3].z ==
          Catch::Approx(-2.25 * (1 - std::exp(-.1))).margin(1e-10));
    const auto retained = *navigation.view();
    rejected(navigation.navigate({0, 0, 0, 0, 100}, {800, 800}, image),
             ErrorCode::invalid_argument);
    CHECK(navigation.view()->camera == retained.camera);
    CHECK(navigation.view()->scene_local_world == retained.scene_local_world);
    rejected(navigation.navigate({0, 0, std::numeric_limits<double>::infinity(), 0, 0}, {800, 800},
                                 image),
             ErrorCode::invalid_argument);
    CHECK(revision(*fixture.owner) == before);
    CHECK(take(fixture.owner->entity_state(fixture.active)).local_transform ==
          authored.local_transform);

    fixture.orthographic();
    REQUIRE(navigation.reset(fixture.snapshot(), fixture.camera));
    REQUIRE(take(navigation.navigate({0, 0, 40, 20, 1}, {800, 800}, image)));
    vector_equal(math::Vec3{navigation.view()->scene_local_world[3]}, {-.4, .2, 0});
    CHECK(navigation.view()->camera.orthographic_height == Catch::Approx(4 * std::exp(-.1)));

    EntitySeed parent{id<AuthoredEntityId>(3)};
    parent.transform.translation = {5, 1, 2};
    parent.transform.scale = {-2, 1, 1};
    const auto parent_handle = take(fixture.owner->create_entity(parent));
    Transform local;
    local.rotation = glm::angleAxis(std::numbers::pi / 4, math::Vec3{0, 0, 1});
    REQUIRE(fixture.owner->set_transform(fixture.active, local));
    REQUIRE(fixture.owner->reparent(fixture.active, parent_handle, ReparentMode::keep_local));
    const auto affine = fixture.snapshot({{4, 1, 2}, 7, {}});
    REQUIRE(navigation.reset(affine, fixture.camera));
    const auto original_world = navigation.view()->scene_local_world;
    CHECK(transform_matrices_equivalent(
        original_world, take(fixture.owner->entity_state(fixture.active)).world_matrix));
    REQUIRE(take(navigation.navigate({30, -15, 0, 0, 0}, {800, 800}, image)));
    for (int left = 0; left < 3; ++left)
        for (int right = 0; right < 3; ++right)
            CHECK(glm::dot(math::Vec3{navigation.view()->scene_local_world[left]},
                           math::Vec3{navigation.view()->scene_local_world[right]}) ==
                  Catch::Approx(
                      glm::dot(math::Vec3{original_world[left]}, math::Vec3{original_world[right]}))
                      .margin(1e-10));
    CHECK(glm::determinant(navigation.view()->scene_local_world) < 0);
    navigation.clear();
    CHECK_FALSE(navigation.view());
    rejected(navigation.navigate({1, 0, 0, 0, 0}, {800, 800}, image), ErrorCode::conflict);
}

TEST_CASE("Viewport picking charges all instance triangles before returning a hit",
          "[editor-viewport-navigation]") {
    // One compact shared primitive repeated across instances exceeds submitted
    // work without duplicating vertices or allocating a giant source fixture.
    Fixture fixture{false, 87'382};
    for (std::uint32_t index = 0; index < 13; ++index)
        static_cast<void>(fixture.mesh(index + 2));
    const auto snapshot = fixture.snapshot();
    REQUIRE(snapshot.records().size() == 13);
    rejected(pick_viewport(snapshot, snapshot.revision(), image, 300, 250),
             ErrorCode::invalid_argument);
}
