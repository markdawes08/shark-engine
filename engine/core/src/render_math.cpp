#include "render_math.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace shark {
namespace {

constexpr double roundoff_allowance = 64.0 * std::numeric_limits<double>::epsilon();

Error render_error(const char* operation, const char* message) {
    return {ErrorCode::invalid_argument, message, {.operation = operation}};
}

bool finite(const math::Vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(const math::Mat4& value) noexcept {
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            if (!std::isfinite(value[column][row]))
                return false;
    return true;
}

bool bounded(const math::Vec3& value) noexcept {
    return finite(value) && std::abs(value.x) <= render_coordinate_limit &&
           std::abs(value.y) <= render_coordinate_limit &&
           std::abs(value.z) <= render_coordinate_limit;
}

Result<void> validate_origin(const RenderOrigin& origin) {
    if (!finite(origin.scene_local_offset) || origin.generation == 0)
        return render_error("render.transform",
                            "Render origin must be finite with a nonzero generation");
    if (origin.world_anchor)
        return validate_world_position(*origin.world_anchor);
    return {};
}

bool mirrored(const math::Mat4& matrix) noexcept {
    // Existing transform validation establishes nonzero, independent columns.
    // Normalize by each column's largest coefficient to avoid determinant overflow.
    glm::dmat3 basis{matrix};
    for (int column = 0; column < 3; ++column) {
        const auto& value = basis[column];
        basis[column] /= std::max({std::abs(value.x), std::abs(value.y), std::abs(value.z)});
    }
    return glm::determinant(basis) < 0.0;
}

Result<RenderPlane> normalized_plane(const glm::dvec4& plane) {
    const double maximum = std::max({std::abs(plane.x), std::abs(plane.y), std::abs(plane.z)});
    if (!std::isfinite(maximum) || maximum == 0.0 || !std::isfinite(plane.w))
        return render_error("render.camera", "Camera frustum has a degenerate or nonfinite plane");
    const math::Vec3 scaled{plane.x / maximum, plane.y / maximum, plane.z / maximum};
    const double length = std::hypot(scaled.x, scaled.y, scaled.z);
    RenderPlane result{scaled / length, (plane.w / maximum) / length};
    if (!finite(result.normal) || !std::isfinite(result.distance))
        return render_error("render.camera", "Camera frustum plane cannot be represented finitely");
    return result;
}

bool reliable_distance(double actual, double expected) noexcept {
    return std::isfinite(actual) &&
           std::abs(actual - expected) <=
               transform_absolute_tolerance + transform_relative_tolerance * std::abs(expected);
}

} // namespace

namespace render_detail {

Result<RenderTransform> relative_transform(const math::Mat4& scene_local_world,
                                           const RenderOrigin& origin) {
    if (auto valid = validate_origin(origin); !valid)
        return valid.error();
    if (auto valid = validate_transform_matrix(scene_local_world); !valid)
        return valid.error();
    auto world = scene_local_world;
    world[3] = glm::dvec4{math::Vec3{scene_local_world[3]} - origin.scene_local_offset, 1.0};
    if (!bounded(math::Vec3{world[3]}))
        return render_error("render.transform",
                            "Origin-relative translation exceeds the render coordinate bound");
    auto inverse = inverse_transform_matrix(world);
    if (!inverse)
        return inverse.error();
    math::Mat4 normal{1.0};
    for (int column = 0; column < 3; ++column)
        for (int row = 0; row < 3; ++row)
            normal[column][row] = inverse.value()[row][column];
    return RenderTransform{world, normal, mirrored(world)};
}

Result<RenderBounds> transformed_bounds(const RenderBounds& local, const math::Mat4& render_world) {
    constexpr auto operation = "render.bounds";
    if (!bounded(local.minimum) || !bounded(local.maximum) || local.minimum.x > local.maximum.x ||
        local.minimum.y > local.maximum.y || local.minimum.z > local.maximum.z)
        return render_error(
            operation,
            "Local bounds must be ordered, finite and inside the render coordinate bound");
    if (auto valid = validate_transform_matrix(render_world); !valid)
        return valid.error();
    if (!bounded(math::Vec3{render_world[3]}))
        return render_error(operation, "Render translation exceeds the render coordinate bound");
    RenderBounds result{math::Vec3{std::numeric_limits<double>::infinity()},
                        math::Vec3{-std::numeric_limits<double>::infinity()}};
    for (unsigned corner = 0; corner < 8; ++corner) {
        const math::Vec3 point{(corner & 1U) != 0 ? local.maximum.x : local.minimum.x,
                               (corner & 2U) != 0 ? local.maximum.y : local.minimum.y,
                               (corner & 4U) != 0 ? local.maximum.z : local.minimum.z};
        for (int row = 0; row < 3; ++row) {
            double value = render_world[3][row];
            double magnitude = std::abs(value);
            for (int column = 0; column < 3; ++column) {
                const double term = render_world[column][row] * point[column];
                value += term;
                magnitude += std::abs(term);
            }
            // Bounds include a conservative allowance for products and sums,
            // including cancellation. Never shrink an AABB through rounding.
            const double allowance = roundoff_allowance * std::max(1.0, magnitude);
            const double low =
                std::nextafter(value - allowance, -std::numeric_limits<double>::infinity());
            const double high =
                std::nextafter(value + allowance, std::numeric_limits<double>::infinity());
            if (!std::isfinite(value) || !std::isfinite(magnitude) || !std::isfinite(low) ||
                !std::isfinite(high) || low < -render_coordinate_limit ||
                high > render_coordinate_limit)
                return render_error(
                    operation,
                    "Conservative transformed bounds exceed the render coordinate bound");
            result.minimum[row] = std::min(result.minimum[row], low);
            result.maximum[row] = std::max(result.maximum[row], high);
        }
    }
    return result;
}

bool visible(const RenderBounds& bounds, const RenderCameraMatrices& camera) noexcept {
    for (const auto& plane : camera.frustum) {
        double distance = plane.distance;
        double magnitude = std::abs(plane.distance);
        for (int axis = 0; axis < 3; ++axis) {
            const double term =
                plane.normal[axis] *
                (plane.normal[axis] >= 0.0 ? bounds.maximum[axis] : bounds.minimum[axis]);
            distance += term;
            magnitude += std::abs(term);
        }
        // Touching boxes and uncertain signs remain visible. This is a
        // conservative six-plane test, not an exact frustum/AABB intersection.
        if (distance < -plane.uncertainty - roundoff_allowance * std::max(1.0, magnitude))
            return false;
    }
    return true;
}

} // namespace render_detail

Result<RenderCameraMatrices> make_render_camera(const Camera& camera,
                                                const math::Mat4& scene_local_world,
                                                RenderViewport viewport,
                                                const RenderOrigin& origin) {
    constexpr auto operation = "render.camera";
    if (auto valid = validate_camera(camera); !valid)
        return valid.error();
    if (viewport.width == 0 || viewport.height == 0 ||
        viewport.width > render_viewport_maximum_extent ||
        viewport.height > render_viewport_maximum_extent)
        return render_error(operation, "Viewport extent must be between 1 and 16384");
    auto transform = render_detail::relative_transform(scene_local_world, origin);
    if (!transform)
        return transform.error();
    auto view = inverse_transform_matrix(transform.value().world);
    if (!view)
        return view.error();
    RenderCameraMatrices result;
    result.view = view.value();
    result.mirrored = transform.value().mirrored;
    auto& projection = result.projection;
    projection = math::Mat4{0.0};
    const double aspect =
        static_cast<double>(viewport.width) / static_cast<double>(viewport.height);
    const double depth_range = camera.far_clip - camera.near_clip;
    if (camera.projection == CameraProjection::perspective) {
        const double focal = 1.0 / std::tan(camera.vertical_fov_radians / 2.0);
        projection[0][0] = focal / aspect;
        projection[1][1] = -focal;
        projection[2][2] = -camera.far_clip / depth_range;
        projection[2][3] = -1.0;
        projection[3][2] = camera.near_clip * projection[2][2];
    } else {
        const double half_height = camera.orthographic_height / 2.0;
        projection[0][0] = 1.0 / (half_height * aspect);
        projection[1][1] = -1.0 / half_height;
        projection[2][2] = -1.0 / depth_range;
        projection[3][2] = -camera.near_clip / depth_range;
        projection[3][3] = 1.0;
    }
    if (!finite(projection) || projection[0][0] <= 0.0 || projection[1][1] >= 0.0 ||
        projection[2][2] >= 0.0)
        return render_error(operation, "Projection coefficients are nonfinite or degenerate");

    // Validate the clip distances represented by the actual rounded projection.
    // A finite far plane must not silently become infinite (p22 rounds to -1),
    // or drift through cancellation beyond the existing transform tolerance.
    const double represented_near = projection[3][2] / projection[2][2];
    const double represented_far = camera.projection == CameraProjection::perspective
                                       ? projection[3][2] / (1.0 + projection[2][2])
                                       : (projection[3][2] - 1.0) / projection[2][2];
    if (!reliable_distance(represented_near, camera.near_clip) ||
        !reliable_distance(represented_far, camera.far_clip) ||
        represented_far <= represented_near ||
        (camera.projection == CameraProjection::perspective && represented_near <= 0.0))
        return render_error(
            operation,
            "Projection cannot preserve finite clip distances within transform tolerance");

    result.view_projection = projection * result.view;
    if (!finite(result.view_projection))
        return render_error(operation, "Camera view-projection is nonfinite");
    std::array<glm::dvec4, 4> rows{};
    for (int row = 0; row < 4; ++row)
        rows[static_cast<std::size_t>(row)] = glm::dvec4{projection[0][row], projection[1][row],
                                                         projection[2][row], projection[3][row]};
    const std::array<glm::dvec4, 6> planes{rows[3] + rows[0], rows[3] - rows[0], rows[3] + rows[1],
                                           rows[3] - rows[1], rows[2],           rows[3] - rows[2]};
    for (std::size_t index = 0; index < planes.size(); ++index) {
        // Normalize before applying the view to avoid multiplying tiny finite
        // far-plane coefficients into an otherwise usable inverse transform.
        auto local = normalized_plane(planes[index]);
        if (!local)
            return local.error();
        const glm::dvec4 local_plane{local.value().normal, local.value().distance};
        glm::dvec4 transformed{0.0};
        glm::dvec4 magnitudes{0.0};
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                const double term = result.view[row][column] * local_plane[column];
                transformed[row] += term;
                magnitudes[row] += std::abs(term);
            }
            if (!std::isfinite(magnitudes[row]))
                return render_error(operation, "Frustum construction uncertainty is nonfinite");
        }
        auto world = normalized_plane(transformed);
        if (!world)
            return world.error();
        const double maximum =
            std::max({std::abs(transformed.x), std::abs(transformed.y), std::abs(transformed.z)});
        const double length =
            std::hypot(transformed.x / maximum, transformed.y / maximum, transformed.z / maximum);
        double construction_scale = (magnitudes.w / maximum) / length;
        for (int axis = 0; axis < 3; ++axis)
            construction_scale += ((magnitudes[axis] / maximum) / length) * render_coordinate_limit;
        // The normalized plane's final coefficients can hide cancellation in
        // its construction. Carry that error separately, including coefficient
        // normalization over the full bounded coordinate cube. Near/far planes
        // also retain the admitted distance drift of the rounded projection.
        const double clip_error = index == 4   ? std::abs(represented_near - camera.near_clip)
                                  : index == 5 ? std::abs(represented_far - camera.far_clip)
                                               : 0.0;
        world.value().uncertainty = roundoff_allowance * std::max(1.0, construction_scale) +
                                    (clip_error / maximum) / length;
        if (!std::isfinite(world.value().uncertainty))
            return render_error(operation, "Normalized frustum uncertainty is nonfinite");
        result.frustum[index] = world.value();
    }
    return result;
}

} // namespace shark
