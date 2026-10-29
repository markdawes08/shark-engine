#include "../editor/viewport_gizmo.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <numbers>

namespace {
using namespace shark;
using namespace shark::editor;
using namespace shark::tools;
template <typename T> T take(Result<T> value) {
    if (!value) {
        INFO(value.error().message);
    }
    REQUIRE(value);
    return std::move(value).value();
}
void accepted(Result<void> value) {
    if (!value) {
        INFO(value.error().message);
    }
    REQUIRE(value);
}
template <typename T> void rejected(const Result<T>& value, ErrorCode code) {
    REQUIRE_FALSE(value);
    CHECK(value.error().code == code);
}
StableId stable(const char* value) {
    return take(StableId::parse(value));
}
class Fixture final {
  public:
    Fixture() {
        owner = take(Scene::create(
            AuthoredSceneScope{SceneId{stable("17c00000-0000-4000-8000-000000000001")}}));
        EntitySeed seed{AuthoredEntityId{stable("17c00000-0000-4000-8000-000000000002")}};
        camera.projection = CameraProjection::orthographic;
        camera.orthographic_height = 4;
        camera.near_clip = 1;
        camera.far_clip = 100;
        seed.camera = camera;
        camera_handle = take(owner->create_entity(seed));
        assets = take(PreparedAssets::create());
    }
    RenderScene snapshot(RenderOrigin origin = {}) const {
        const auto stats = take(owner->stats());
        return take(extract_render_scene(
            *owner, *assets,
            {camera_handle, {stats.instance, stats.revision}, {800, 800}, origin}));
    }
    TransformObservation observation(const RenderScene& scene,
                                     Transform local = {{0, 0, -5}, {}, {1, 1, 1}},
                                     math::Mat4 parent = math::Mat4{1.0}) const {
        return {
            {AuthoredEntityId{stable("17c00000-0000-4000-8000-000000000003")}, scene.revision()},
            local,
            parent};
    }
    std::unique_ptr<Scene> owner;
    std::shared_ptr<const PreparedAssets> assets;
    Camera camera;
    EntityHandle camera_handle;
};
constexpr ViewportRectangle image{100, 50, 400, 400};
void near(double actual, double expected) {
    CHECK(actual == Catch::Approx(expected).margin(1e-9));
}
math::Quat around_z(double angle) {
    return {std::cos(angle * .5), 0, 0, std::sin(angle * .5)};
}
} // namespace

TEST_CASE("Gizmo projection uses actual logical image rectangle and preserves pointer units",
          "[editor-gizmo]") {
    Fixture fixture;
    const auto scene = fixture.snapshot();
    const auto observed = fixture.observation(scene);
    const auto geometry = take(make_viewport_gizmo(scene, observed, TransformOperation::translate,
                                                   TransformSpace::world, image));
    near(geometry.pivot().x, 300);
    near(geometry.pivot().y, 250);
    REQUIRE(geometry.axes()[0].visible);
    REQUIRE(geometry.axes()[1].visible);
    CHECK_FALSE(geometry.axes()[2].visible);
    near(geometry.axes()[0].points[1].x, 380);
    near(geometry.axes()[1].points[1].y, 170);
    CHECK(take(hit_viewport_gizmo(geometry, 370, 250)) == 0);
    CHECK(take(hit_viewport_gizmo(geometry, 300, 180)) == 1);
    CHECK_FALSE(take(hit_viewport_gizmo(geometry, 300, 250)));
    CHECK_FALSE(take(hit_viewport_gizmo(geometry, 500, 250)));
    ViewportGizmoGesture gesture;
    accepted(gesture.begin(geometry, 0, 340, 250));
    const auto move = take(gesture.update(440, 250));
    near(move.translation.x, 1.0);
    near(move.translation.y, 0);
    near(take(gesture.update(390, 250)).translation.x, .5);
    gesture.cancel();
    CHECK_FALSE(gesture.active());
    rejected(gesture.update(390, 250), ErrorCode::conflict);

    // Same camera, differently stretched logical image (nonuniform framebuffer
    // scale): proportional pointer movement must yield the same world motion.
    const ViewportRectangle stretched{-20, 70, 800, 200};
    const auto scaled = take(make_viewport_gizmo(scene, observed, TransformOperation::translate,
                                                 TransformSpace::world, stretched));
    near(scaled.pivot().x, 380);
    near(scaled.pivot().y, 170);
    accepted(gesture.begin(scaled, 0, 420, 170));
    near(take(gesture.update(620, 170)).translation.x, 1.0);
}

TEST_CASE("Gizmo local axes preserve rotated mirrored nonuniform parent units", "[editor-gizmo]") {
    Fixture fixture;
    const auto scene = fixture.snapshot();
    const auto parent =
        take(transform_matrix({{0, 0, -5}, around_z(std::numbers::pi / 2), {-2, 3, 1}}));
    const auto observed = fixture.observation(scene, Transform{}, parent);
    const auto local = take(make_viewport_gizmo(scene, observed, TransformOperation::translate,
                                                TransformSpace::local, image));
    const auto global = take(make_viewport_gizmo(scene, observed, TransformOperation::translate,
                                                 TransformSpace::world, image));
    REQUIRE(local.axes()[0].visible);
    near(local.axes()[0].points[1].x, 300);
    CHECK(local.axes()[0].points[1].y > 250);
    CHECK(global.axes()[0].points[1].x > 300);
    ViewportGizmoGesture gesture;
    accepted(gesture.begin(local, 0, 300, 280));
    const auto move = take(gesture.update(300, 380));
    near(move.translation.x, .5);
    const auto changed =
        take(calculate_manipulated_transform(observed.local, observed.parent_world, move));
    near(changed.translation.x, .5);
    const auto world = parent * take(transform_matrix(changed));
    near(world[3].x, 0);
    near(world[3].y, -1);
    near(world[3].z, -5);
    // The entity's own negative scale does not reverse its translation axes.
    auto mirrored = observed;
    mirrored.local.scale = {-5, 2, 3};
    const auto other = take(make_viewport_gizmo(scene, mirrored, TransformOperation::translate,
                                                TransformSpace::local, image));
    near(other.axes()[0].points[1].x, local.axes()[0].points[1].x);
    near(other.axes()[0].points[1].y, local.axes()[0].points[1].y);
}

TEST_CASE("Gizmo scale and snapping use shared cumulative manipulation", "[editor-gizmo]") {
    Fixture fixture;
    const auto scene = fixture.snapshot();
    auto observed = fixture.observation(scene);
    observed.local.scale = {-2, 3, 4};
    const auto geometry = take(make_viewport_gizmo(scene, observed, TransformOperation::scale,
                                                   TransformSpace::local, image));
    ViewportGizmoGesture gesture;
    TransformSnap snap;
    snap.scale = .5;
    accepted(gesture.begin(geometry, 0, 380, 250, snap));
    auto scale = take(gesture.update(410, 250));
    near(scale.scale_factors.x, 1.375);
    CHECK(scale.snap.scale == .5);
    auto changed =
        take(calculate_manipulated_transform(observed.local, observed.parent_world, scale));
    near(changed.scale.x, -3);
    scale = take(gesture.update(220, 250));
    near(scale.scale_factors.x, -1);
    changed = take(calculate_manipulated_transform(observed.local, observed.parent_world, scale));
    near(changed.scale.x, 2);
    scale = take(gesture.update(300, 250));
    rejected(calculate_manipulated_transform(observed.local, observed.parent_world, scale),
             ErrorCode::invalid_argument);
}

TEST_CASE("Gizmo rotation follows affine rings and unwraps full turns", "[editor-gizmo]") {
    Fixture fixture;
    const auto scene = fixture.snapshot();
    const auto parent = take(transform_matrix({{0, 0, -5}, {}, {-2, 1, 1}}));
    const auto observed = fixture.observation(scene, Transform{}, parent);
    const auto geometry = take(make_viewport_gizmo(scene, observed, TransformOperation::rotate,
                                                   TransformSpace::local, image));
    CHECK_FALSE(geometry.axes()[0].visible);
    CHECK_FALSE(geometry.axes()[1].visible);
    REQUIRE(geometry.axes()[2].visible);
    const auto& ring = geometry.axes()[2];
    CHECK(ring.point_count == 65);
    near(ring.points[0].x, 220);
    near(ring.points[16].y, 210);
    CHECK(take(hit_viewport_gizmo(geometry, 220, 250)) == 2);
    ViewportGizmoGesture gesture;
    accepted(gesture.begin(geometry, 2, ring.points[0].x, ring.points[0].y));
    double expected = 0;
    for (std::size_t step = 1; step <= 8; ++step) {
        const auto index = (step * 16) % 64;
        const auto input = take(gesture.update(ring.points[index].x, ring.points[index].y));
        expected += std::numbers::pi / 2;
        near(input.rotation_radians, expected);
        CHECK(input.rotation_axis == math::Vec3{0, 0, 1});
        CHECK(input.space == TransformSpace::local);
    }
    // A rejected point at the pivot cannot add an invented rotation increment.
    rejected(gesture.update(geometry.pivot().x, geometry.pivot().y), ErrorCode::invalid_argument);
    near(take(gesture.update(ring.points[16].x, ring.points[16].y)).rotation_radians,
         expected + std::numbers::pi / 2);
    gesture.cancel();
    rejected(gesture.begin(geometry, 0, 300, 250), ErrorCode::invalid_argument);
}

TEST_CASE("Gizmo geometry respects origin perspective and invalid observations", "[editor-gizmo]") {
    Fixture fixture;
    fixture.camera.projection = CameraProjection::perspective;
    fixture.camera.vertical_fov_radians = std::numbers::pi / 2;
    REQUIRE(fixture.owner->set_camera(fixture.camera_handle, fixture.camera));
    const auto scene = fixture.snapshot();
    auto observed = fixture.observation(scene);
    const auto geometry = take(make_viewport_gizmo(scene, observed, TransformOperation::translate,
                                                   TransformSpace::world, image));
    ViewportGizmoGesture gesture;
    accepted(gesture.begin(geometry, 0, 340, 250));
    near(take(gesture.update(380, 250)).translation.x, 1.0);
    const auto shifted = fixture.snapshot({{100, -20, 30}, 2});
    const auto shifted_geometry = take(make_viewport_gizmo(
        shifted, observed, TransformOperation::translate, TransformSpace::world, image));
    near(shifted_geometry.pivot().x, geometry.pivot().x);
    near(shifted_geometry.axes()[0].points[1].x, geometry.axes()[0].points[1].x);
    ++observed.selection.revision.revision;
    rejected(make_viewport_gizmo(scene, observed, TransformOperation::translate,
                                 TransformSpace::world, image),
             ErrorCode::conflict);
    observed = fixture.observation(scene);
    rejected(make_viewport_gizmo(scene, observed, static_cast<TransformOperation>(99),
                                 TransformSpace::world, image),
             ErrorCode::invalid_argument);
    rejected(make_viewport_gizmo(scene, observed, TransformOperation::translate,
                                 TransformSpace::world, {0, 0, 0, 1}),
             ErrorCode::invalid_argument);
    rejected(hit_viewport_gizmo(geometry, std::numeric_limits<double>::infinity(), 0),
             ErrorCode::invalid_argument);
    rejected(gesture.update(99, 250), ErrorCode::conflict);
    rejected(gesture.update(std::numeric_limits<double>::quiet_NaN(), 250),
             ErrorCode::invalid_argument);
    gesture.cancel();
    TransformSnap invalid;
    invalid.translation = 0;
    rejected(gesture.begin(geometry, 0, 340, 250, invalid), ErrorCode::invalid_argument);
}
