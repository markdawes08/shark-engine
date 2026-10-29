#include "viewport_navigation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace shark::editor {
namespace {
struct Failure {
    Error error;
};
void require(bool condition, const char* message, ErrorCode code = ErrorCode::invalid_argument) {
    if (!condition)
        throw Failure{{code, message, {.operation = "editor.viewport"}}};
}
template <typename T> T take(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
bool finite(const math::Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool rectangle_valid(const ViewportRectangle& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.width) &&
           std::isfinite(value.height) && value.width > 0 && value.height > 0 &&
           std::isfinite(value.x + value.width) && std::isfinite(value.y + value.height);
}
math::Vec3 point(const math::Mat4& matrix, const math::Vec3& value) {
    return math::Vec3{matrix * glm::dvec4{value, 1.0}};
}
math::Vec3 unproject(const math::Mat4& inverse, double x, double y, double z) {
    const auto homogeneous = inverse * glm::dvec4{x, y, z, 1.0};
    require(std::isfinite(homogeneous.w) && homogeneous.w != 0,
            "Viewport ray has an unusable homogeneous coordinate");
    const math::Vec3 value = math::Vec3{homogeneous} / homogeneous.w;
    require(finite(value), "Viewport ray is not finite");
    return value;
}
Error unexpected() {
    return {ErrorCode::execution_failed,
            "Cannot prepare viewport interaction",
            {.operation = "editor.viewport"}};
}
// Moller-Trumbore in mesh-local coordinates. Keep the transformed direction's
// magnitude: its parameter remains distance along the original normalized ray.
std::optional<double> intersect(const math::Vec3& origin, const math::Vec3& direction,
                                const PreparedPrimitive& primitive, std::size_t index,
                                double maximum_distance) {
    const auto vertex = [&](std::size_t offset) {
        const auto& value = primitive.vertices[primitive.indices[index + offset]].position;
        return math::Vec3{value[0], value[1], value[2]};
    };
    const auto a = vertex(0);
    const auto edge1 = vertex(1) - a;
    const auto edge2 = vertex(2) - a;
    const auto cross = glm::cross(direction, edge2);
    const double determinant = glm::dot(edge1, cross);
    const double magnitude = glm::length(edge1) * glm::length(edge2) * glm::length(direction);
    require(std::isfinite(determinant) && std::isfinite(magnitude),
            "Viewport triangle intersection is not finite");
    const double tolerance = 64 * std::numeric_limits<double>::epsilon() * magnitude;
    if (primitive.material.double_sided ? std::abs(determinant) <= tolerance
                                        : determinant <= tolerance)
        return std::nullopt;
    const auto offset = origin - a;
    const double u = glm::dot(offset, cross) / determinant;
    require(std::isfinite(u), "Viewport triangle coordinate is not finite");
    if (u < 0 || u > 1)
        return std::nullopt;
    const auto q = glm::cross(offset, edge1);
    const double v = glm::dot(direction, q) / determinant;
    require(std::isfinite(v), "Viewport triangle coordinate is not finite");
    if (v < 0 || u + v > 1)
        return std::nullopt;
    const double distance = glm::dot(edge2, q) / determinant;
    require(std::isfinite(distance), "Viewport triangle hit is not finite");
    if (distance < 0 || distance > maximum_distance)
        return std::nullopt;
    return distance;
}
} // namespace

Result<std::optional<ViewportCoordinates>> viewport_coordinates(const ViewportRectangle& rect,
                                                                double x, double y) {
    if (!rectangle_valid(rect) || !std::isfinite(x) || !std::isfinite(y))
        return Error{ErrorCode::invalid_argument,
                     "Viewport image rectangle or pointer is invalid",
                     {.operation = "editor.viewport.coordinates"}};
    if (x < rect.x || y < rect.y || x >= rect.x + rect.width || y >= rect.y + rect.height)
        return std::optional<ViewportCoordinates>{};
    return std::optional<ViewportCoordinates>{
        {(x - rect.x) / rect.width, (y - rect.y) / rect.height}};
}

Result<void> ViewportNavigation::reset(const RenderScene& scene, const Camera& camera) {
    try {
        auto world = take(inverse_transform_matrix(scene.camera().view));
        world[3] += glm::dvec4{scene.origin().scene_local_offset, 0.0};
        RenderViewCamera candidate{camera, world};
        const auto matrices =
            take(make_render_camera(camera, world, scene.viewport(), scene.origin()));
        require(
            transform_matrices_equivalent(matrices.view_projection, scene.camera().view_projection),
            "Navigation seed does not match the displayed Camera");
        const double distance = std::clamp((camera.far_clip - camera.near_clip) * .25, .01, 10.0);
        const auto forward = -glm::normalize(math::Vec3{world[2]});
        const auto pivot = math::Vec3{world[3]} + forward * distance;
        require(finite(pivot), "Navigation pivot is not finite");
        view_ = std::move(candidate);
        origin_ = scene.origin();
        pivot_ = pivot;
        distance_ = distance;
        return {};
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return unexpected();
    }
}
void ViewportNavigation::clear() noexcept {
    view_.reset();
}
const std::optional<RenderViewCamera>& ViewportNavigation::view() const noexcept {
    return view_;
}
Result<bool> ViewportNavigation::navigate(const NavigationMotion& motion, RenderViewport viewport,
                                          const ViewportRectangle& rect) {
    try {
        require(view_.has_value(), "Navigation requires a current view", ErrorCode::conflict);
        require(rectangle_valid(rect), "Navigation image rectangle is invalid");
        for (const double delta : {motion.orbit_x, motion.orbit_y, motion.pan_x, motion.pan_y})
            require(std::isfinite(delta) && std::abs(delta) <= 4096,
                    "Navigation drag exceeds its finite per-frame bound");
        require(std::isfinite(motion.dolly) && std::abs(motion.dolly) <= 32,
                "Navigation dolly exceeds its finite per-frame bound");
        require(viewport.width > 0 && viewport.height > 0 &&
                    viewport.width <= render_viewport_maximum_extent &&
                    viewport.height <= render_viewport_maximum_extent,
                "Navigation viewport dimensions are invalid");
        if (motion.orbit_x == 0 && motion.orbit_y == 0 && motion.pan_x == 0 && motion.pan_y == 0 &&
            motion.dolly == 0)
            return false;
        auto candidate = *view_;
        auto pivot = pivot_;
        double distance = distance_;
        auto& world = candidate.scene_local_world;
        if (motion.orbit_x != 0 || motion.orbit_y != 0) {
            // A proper world rotation preserves the seed's full affine basis.
            const auto yaw =
                glm::rotate(math::Mat4{1.0}, -motion.orbit_x * .005, math::Vec3{0, 1, 0});
            const auto right = glm::normalize(math::Vec3{yaw * world[0]});
            const auto pitch = glm::rotate(math::Mat4{1.0}, -motion.orbit_y * .005, right);
            const auto rotation = pitch * yaw;
            world = glm::translate(math::Mat4{1.0}, pivot) * rotation *
                    glm::translate(math::Mat4{1.0}, -pivot) * world;
        }
        if (motion.pan_x != 0 || motion.pan_y != 0) {
            const double height = candidate.camera.projection == CameraProjection::orthographic
                                      ? candidate.camera.orthographic_height
                                      : 2 * distance / glm::length(math::Vec3{world[2]}) *
                                            std::tan(candidate.camera.vertical_fov_radians * .5);
            const double aspect = static_cast<double>(viewport.width) / viewport.height;
            const auto translation =
                math::Vec3{world[0]} * (-motion.pan_x * height * aspect / rect.width) +
                math::Vec3{world[1]} * (motion.pan_y * height / rect.height);
            world[3] += glm::dvec4{translation, 0.0};
            pivot += translation;
        }
        if (motion.dolly != 0) {
            const double factor = std::exp(-motion.dolly * .1);
            if (candidate.camera.projection == CameraProjection::orthographic)
                candidate.camera.orthographic_height *= factor;
            else {
                const double next_distance = distance * factor;
                require(next_distance >= .01 && next_distance <= render_coordinate_limit,
                        "Navigation focus distance exceeds its supported range");
                const auto forward = -glm::normalize(math::Vec3{world[2]});
                world[3] += glm::dvec4{forward * (distance - next_distance), 0.0};
                distance = next_distance;
            }
        }
        require(finite(pivot), "Navigation pivot is not finite");
        static_cast<void>(take(make_render_camera(candidate.camera, world, viewport, origin_)));
        view_ = std::move(candidate);
        pivot_ = pivot;
        distance_ = distance;
        return true;
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return unexpected();
    }
}

Result<std::optional<ViewportHit>> pick_viewport(const RenderScene& scene,
                                                 SceneRevision expected_revision,
                                                 const ViewportRectangle& rect, double x,
                                                 double y) {
    try {
        require(scene.revision() == expected_revision,
                "Picking requires the displayed scene revision", ErrorCode::conflict);
        const auto coordinates = take(viewport_coordinates(rect, x, y));
        if (!coordinates)
            return std::optional<ViewportHit>{};
        std::size_t triangles = 0;
        for (const auto& record : scene.records()) {
            require(std::holds_alternative<AuthoredEntityId>(record.entity.identity),
                    "Editor picking requires authored entity identities");
            for (const auto& primitive : record.asset->meshes[record.mesh_index].primitives) {
                const auto count = primitive.indices.size() / 3;
                require(count <= viewport_pick_maximum_triangles - triangles,
                        "Viewport picking triangle work exceeds its limit");
                triangles += count;
            }
        }
        // Vulkan depth is [0,1], and projection already flips Y. Image top-left
        // therefore maps to (-1,-1), with no second screen-Y inversion.
        const auto inverse = glm::inverse(scene.camera().view_projection);
        const double nx = coordinates->u * 2 - 1;
        const double ny = coordinates->v * 2 - 1;
        const auto near_point = unproject(inverse, nx, ny, 0);
        const auto far_point = unproject(inverse, nx, ny, 1);
        const double maximum_distance = glm::length(far_point - near_point);
        require(std::isfinite(maximum_distance) && maximum_distance > 0,
                "Viewport ray segment is not usable");
        const auto direction = (far_point - near_point) / maximum_distance;
        std::optional<ViewportHit> nearest;
        for (const auto& record : scene.records()) {
            const auto inverse_world = take(inverse_transform_matrix(record.world_matrix));
            const auto local_origin = point(inverse_world, near_point);
            const auto local_direction = math::Vec3{inverse_world * glm::dvec4{direction, 0.0}};
            for (const auto& primitive : record.asset->meshes[record.mesh_index].primitives) {
                for (std::size_t index = 0; index < primitive.indices.size(); index += 3) {
                    const auto hit = intersect(local_origin, local_direction, primitive, index,
                                               nearest ? nearest->distance : maximum_distance);
                    if (hit)
                        nearest = ViewportHit{std::get<AuthoredEntityId>(record.entity.identity),
                                              scene.revision(), *hit};
                }
            }
        }
        return nearest;
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return unexpected();
    }
}

} // namespace shark::editor
