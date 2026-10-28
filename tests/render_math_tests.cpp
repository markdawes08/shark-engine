#include "render_math.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

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

void close(double actual, double expected) {
    CHECK(actual == Catch::Approx(expected).epsilon(1e-12).margin(1e-11));
}

void vector_close(const math::Vec3& actual, const math::Vec3& expected) {
    for (int axis = 0; axis < 3; ++axis)
        close(actual[axis], expected[axis]);
}

Camera perspective() {
    Camera camera;
    camera.vertical_fov_radians = std::numbers::pi / 2.0;
    camera.near_clip = 1.0;
    camera.far_clip = 11.0;
    return camera;
}

math::Vec3 ndc(const RenderCameraMatrices& camera, math::Vec3 point) {
    const auto clip = camera.view_projection * glm::dvec4{point, 1.0};
    REQUIRE(clip.w != 0.0);
    return math::Vec3{clip} / clip.w;
}

RenderBounds point_bounds(math::Vec3 point) {
    return {point, point};
}

math::Mat4 shear_mirror() {
    // x' = -2x + y + 5; y' = 3y + z/2 + 6; z' = 4z + 7.
    math::Mat4 matrix{1.0};
    matrix[0] = glm::dvec4{-2, 0, 0, 0};
    matrix[1] = glm::dvec4{1, 3, 0, 0};
    matrix[2] = glm::dvec4{0, .5, 4, 0};
    matrix[3] = glm::dvec4{5, 6, 7, 1};
    return matrix;
}
} // namespace

TEST_CASE("render perspective has finite Vulkan depth aspect and one Y flip", "[render-math]") {
    const auto camera = take(make_render_camera(perspective(), math::Mat4{1.0}, {800, 400}));
    close(camera.projection[0][0], .5);
    close(camera.projection[1][1], -1.0);
    close(camera.projection[2][2], -1.1);
    close(camera.projection[3][2], -1.1);
    close(camera.projection[2][3], -1.0);
    close(camera.projection[3][3], 0.0);
    vector_close(ndc(camera, {0, 0, -1}), {0, 0, 0});
    vector_close(ndc(camera, {0, 0, -11}), {0, 0, 1});
    vector_close(ndc(camera, {2, 1, -1}), {1, -1, 0});
    CHECK(ndc(camera, {0, 0, -.5}).z < 0.0);
    CHECK(ndc(camera, {0, 0, -12}).z > 1.0);
    CHECK((camera.view_projection * glm::dvec4{0, 0, 1, 1}).w < 0.0);
    CHECK_FALSE(camera.mirrored);
}

TEST_CASE("render orthographic projection admits zero near and preserves scale", "[render-math]") {
    Camera source;
    source.projection = CameraProjection::orthographic;
    source.orthographic_height = 4;
    source.near_clip = 0;
    source.far_clip = 20;
    const auto camera = take(make_render_camera(source, math::Mat4{1.0}, {800, 400}));
    vector_close(ndc(camera, {4, 2, 0}), {1, -1, 0});
    vector_close(ndc(camera, {-4, -2, -20}), {-1, 1, 1});
    vector_close(ndc(camera, {4, 2, -10}), {1, -1, .5});
    close(camera.projection[3][3], 1.0);
    const std::array<math::Vec3, 6> contacts{
        {{-4, 0, -10}, {4, 0, -10}, {0, 2, -10}, {0, -2, -10}, {0, 0, 0}, {0, 0, -20}}};
    const std::array<math::Vec3, 6> outward{
        {{-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
    for (std::size_t index = 0; index < contacts.size(); ++index) {
        CAPTURE(index);
        CHECK(render_detail::visible(point_bounds(contacts[index]), camera));
        CHECK_FALSE(
            render_detail::visible(point_bounds(contacts[index] + .01 * outward[index]), camera));
    }
}

TEST_CASE("render camera uses full translated rotated and sheared inverse", "[render-math]") {
    math::Mat4 rotated{1.0};
    rotated[0] = glm::dvec4{0, 0, -1, 0};
    rotated[2] = glm::dvec4{1, 0, 0, 0};
    rotated[3] = glm::dvec4{1003, 2004, 3005, 1};
    const RenderOrigin origin{{1000, 2000, 3000}, 7};
    const auto camera = take(make_render_camera(perspective(), rotated, {400, 400}, origin));
    vector_close(math::Vec3{camera.view * glm::dvec4{1, 4, 5, 1}}, {0, 0, -2});
    vector_close(math::Vec3{camera.view * glm::dvec4{3, 5, 4, 1}}, {1, 1, 0});
    CHECK(render_detail::visible(point_bounds({1, 4, 5}), camera));
    CHECK_FALSE(render_detail::visible(point_bounds({5, 4, 5}), camera));

    const auto world = shear_mirror();
    const auto sheared = take(make_render_camera(perspective(), world, {400, 400}));
    const glm::dvec4 local{.25, -.5, -2, 1};
    vector_close(math::Vec3{sheared.view * (world * local)}, math::Vec3{local});
    vector_close(ndc(sheared, math::Vec3{world * local}), {.125, .25, .55});
    CHECK(sheared.mirrored);
    CHECK(render_detail::visible(point_bounds(math::Vec3{world * local}), sheared));
}

TEST_CASE("render transforms preserve shear normals and determinant parity", "[render-math]") {
    const auto transform = take(render_detail::relative_transform(shear_mirror(), {}));
    CHECK(transform.mirrored);
    CHECK(transform.world == shear_mirror());
    vector_close(math::Vec3{transform.normal[0]}, {-.5, 1.0 / 6.0, -1.0 / 48.0});
    close(transform.normal[3][3], 1.0);
    for (int axis = 0; axis < 3; ++axis) {
        close(transform.normal[3][axis], 0.0);
        close(transform.normal[axis][3], 0.0);
    }
    const auto normal = math::Vec3{transform.normal * glm::dvec4{1, 0, 0, 0}};
    close(glm::dot(normal, math::Vec3{transform.world[1]}), 0.0);
    close(glm::dot(normal, math::Vec3{transform.world[2]}), 0.0);
    auto huge = math::Mat4{1.0};
    huge[0][0] = -1e200;
    huge[1][1] = 1e200;
    huge[2][2] = 1e200;
    CHECK(take(render_detail::relative_transform(huge, {})).mirrored);
    huge[1][1] = -huge[1][1];
    CHECK_FALSE(take(render_detail::relative_transform(huge, {})).mirrored);
}

TEST_CASE("render affine bounds conservatively contain independent extrema", "[render-math]") {
    const auto bounds =
        take(render_detail::transformed_bounds({{-1, -2, -3}, {2, 1, 4}}, shear_mirror()));
    const math::Vec3 minimum{-1, -1.5, -5};
    const math::Vec3 maximum{8, 11, 23};
    vector_close(bounds.minimum, minimum);
    vector_close(bounds.maximum, maximum);
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(bounds.minimum[axis] <= minimum[axis]);
        CHECK(bounds.maximum[axis] >= maximum[axis]);
    }
    const auto plane =
        take(render_detail::transformed_bounds({{-.5, 0, -.5}, {.5, 0, .5}}, math::Mat4{1}));
    CHECK(plane.minimum.y <= 0.0);
    CHECK(plane.maximum.y >= 0.0);
    vector_close(plane.minimum, {-.5, 0, -.5});
    vector_close(plane.maximum, {.5, 0, .5});
}

TEST_CASE("render frustum rejects each exterior plane but preserves contact and crossing",
          "[render-math]") {
    const auto camera = take(make_render_camera(perspective(), math::Mat4{1.0}, {400, 400}));
    const double diagonal = 1.0 / std::sqrt(2.0);
    const std::array<math::Vec3, 6> normals{{{diagonal, 0, -diagonal},
                                             {-diagonal, 0, -diagonal},
                                             {0, -diagonal, -diagonal},
                                             {0, diagonal, -diagonal},
                                             {0, 0, -1},
                                             {0, 0, 1}}};
    const std::array<math::Vec3, 6> contacts{
        {{-5, 0, -5}, {5, 0, -5}, {0, 5, -5}, {0, -5, -5}, {0, 0, -1}, {0, 0, -11}}};
    for (std::size_t index = 0; index < contacts.size(); ++index) {
        CAPTURE(index);
        vector_close(camera.frustum[index].normal, normals[index]);
        CHECK(render_detail::visible(point_bounds(contacts[index]), camera));
        CHECK_FALSE(
            render_detail::visible(point_bounds(contacts[index] - .01 * normals[index]), camera));
        CHECK(render_detail::visible(point_bounds(contacts[index] + .01 * normals[index]), camera));
    }
    close(camera.frustum[4].distance, -1.0);
    close(camera.frustum[5].distance, 11.0);
    CHECK(render_detail::visible({{-.1, -.1, -1.1}, {.1, .1, -.9}}, camera));
    CHECK(render_detail::visible({{-20, -20, -20}, {20, 20, 20}}, camera));
    CHECK_FALSE(render_detail::visible({{-.1, -.1, .9}, {.1, .1, 1.1}}, camera));
}

TEST_CASE("render origins retain local precision without using global cells", "[render-math]") {
    const auto world_id = WorldId{take(StableId::parse("00000000-0000-4000-8000-000000000001"))};
    RenderOrigin origin{{1e12, -1e12, 1e12},
                        42,
                        WorldPosition{world_id, {world_cell_max, world_cell_min, 0}, {.125, 0, 0}}};
    math::Mat4 world{1.0};
    world[3] = glm::dvec4{origin.scene_local_offset + math::Vec3{.125, -.25, .5}, 1};
    const auto local = take(render_detail::relative_transform(world, origin));
    vector_close(math::Vec3{local.world[3]}, {.125, -.25, .5});
    origin.world_anchor->cell = {0, 0, 0};
    CHECK(take(render_detail::relative_transform(world, origin)).world == local.world);
    origin.world_anchor->offset[0] = 1024;
    CHECK_FALSE(render_detail::relative_transform(world, origin));
    origin.world_anchor.reset();
    origin.generation = 0;
    CHECK_FALSE(render_detail::relative_transform(world, origin));
    origin.generation = 1;
    origin.scene_local_offset.x = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(render_detail::relative_transform(world, origin));
}

TEST_CASE("render frustum retains far contact after translated plane cancellation",
          "[render-math]") {
    Camera source;
    source.projection = CameraProjection::orthographic;
    source.orthographic_height = 10;
    source.near_clip = 0;
    source.far_clip = 60000;
    for (const double yaw : {.001, .002, .005, .01, .02}) {
        CAPTURE(yaw);
        const double cosine = std::cos(yaw);
        const double sine = std::sin(yaw);
        // Camera is 60000 units along its +Z column. Its exact far center is zero.
        math::Mat4 world{1};
        world[0] = glm::dvec4{cosine, 0, -sine, 0};
        world[2] = glm::dvec4{sine, 0, cosine, 0};
        world[3] = glm::dvec4{60000 * sine, 0, 60000 * cosine, 1};
        const auto camera = take(make_render_camera(source, world, {100, 100}));
        CHECK(render_detail::visible(point_bounds({0, 0, 0}), camera));
        CHECK(camera.frustum[5].uncertainty > std::abs(camera.frustum[5].distance));
        CHECK(camera.frustum[5].uncertainty < 1e-6);
        CHECK_FALSE(render_detail::visible(point_bounds(-.01 * math::Vec3{world[2]}), camera));
    }
}

TEST_CASE("render math rejects invalid bounds and numerically collapsed projections",
          "[render-math]") {
    for (const auto viewport :
         std::array<RenderViewport, 4>{{{0, 1}, {1, 0}, {16385, 1}, {1, 16385}}})
        CHECK_FALSE(make_render_camera(perspective(), math::Mat4{1}, viewport));
    auto camera = perspective();
    camera.vertical_fov_radians = 0;
    CHECK_FALSE(make_render_camera(camera, math::Mat4{1}, {1, 1}));
    camera = perspective();
    camera.far_clip = 1e20; // Finite component, but rounded projection loses the far plane.
    REQUIRE(validate_camera(camera));
    CHECK_FALSE(make_render_camera(camera, math::Mat4{1}, {1, 1}));
    camera.far_clip = 1e10; // Far-plane reconstruction exceeds existing transform tolerance.
    CHECK_FALSE(make_render_camera(camera, math::Mat4{1}, {1, 1}));
    camera = perspective();
    camera.projection = CameraProjection::orthographic;
    camera.orthographic_height = std::numeric_limits<double>::denorm_min();
    REQUIRE(validate_camera(camera));
    CHECK_FALSE(make_render_camera(camera, math::Mat4{1}, {1, 1}));

    math::Mat4 world{1};
    world[0][0] = 0;
    CHECK_FALSE(render_detail::relative_transform(world, {}));
    world = math::Mat4{1};
    world[3][0] = render_coordinate_limit + 1;
    CHECK_FALSE(render_detail::relative_transform(world, {}));
    world[3][0] = render_coordinate_limit;
    REQUIRE(render_detail::relative_transform(world, {}));
    // Even a point at the hard extent needs conservative arithmetic headroom.
    CHECK_FALSE(render_detail::transformed_bounds(point_bounds({0, 0, 0}), world));
    CHECK_FALSE(render_detail::transformed_bounds({{1, 0, 0}, {0, 1, 1}}, math::Mat4{1}));
    CHECK_FALSE(render_detail::transformed_bounds(point_bounds({65537, 0, 0}), math::Mat4{1}));
    CHECK_FALSE(render_detail::transformed_bounds(
        point_bounds({std::numeric_limits<double>::infinity(), 0, 0}), math::Mat4{1}));
}
