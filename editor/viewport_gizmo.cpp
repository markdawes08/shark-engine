#include "viewport_gizmo.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace shark::editor {
namespace {
struct Failure {
    Error error;
};
void require(bool value, const char* message, ErrorCode code = ErrorCode::invalid_argument) {
    if (!value)
        throw Failure{{code, message, {.operation = "editor.viewport.gizmo"}}};
}
template <typename T> T take(Result<T> value) {
    if (!value)
        throw Failure{std::move(value).error()};
    return std::move(value).value();
}
template <typename T, typename F> Result<T> boundary(F&& operation) {
    try {
        return operation();
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot prepare viewport gizmo",
                     {.operation = "editor.viewport.gizmo"}};
    }
}
bool finite(const math::Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
void matrix_finite(const math::Mat4& value) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            require(std::isfinite(value[c][r]), "Gizmo matrix is not finite");
}
bool inside(const ViewportRectangle& rectangle, double x, double y) {
    const auto result = take(viewport_coordinates(rectangle, x, y));
    require(std::abs(x) <= 1e7 && std::abs(y) <= 1e7,
            "Gizmo pointer exceeds logical coordinate bounds");
    return result.has_value();
}
math::Vec3 unproject(const math::Mat4& inverse, double x, double y, double z) {
    const auto h = inverse * glm::dvec4{x, y, z, 1.0};
    require(std::isfinite(h.w) && h.w != 0, "Gizmo ray cannot be unprojected");
    const math::Vec3 result = math::Vec3{h} / h.w;
    require(finite(result), "Gizmo ray is not finite");
    return result;
}
struct Ray {
    math::Vec3 origin;
    math::Vec3 direction;
};
Ray ray(const math::Mat4& inverse, const ViewportRectangle& rectangle, double x, double y) {
    require(inside(rectangle, x, y), "Gizmo pointer left the image", ErrorCode::conflict);
    const double nx = 2.0 * (x - rectangle.x) / rectangle.width - 1.0;
    const double ny = 2.0 * (y - rectangle.y) / rectangle.height - 1.0;
    const auto near_point = unproject(inverse, nx, ny, 0.0);
    const auto far_point = unproject(inverse, nx, ny, 1.0);
    auto direction = far_point - near_point;
    const auto length = std::hypot(direction.x, direction.y, direction.z);
    require(std::isfinite(length) && length > 0, "Gizmo ray is degenerate");
    direction /= length;
    return {near_point, direction};
}
std::optional<GizmoPoint> project(const math::Mat4& matrix, const ViewportRectangle& rectangle,
                                  const math::Vec3& point) {
    if (!finite(point))
        return {};
    const auto clip = matrix * glm::dvec4{point, 1.0};
    if (!std::isfinite(clip.w) || clip.w <= 0 || !std::isfinite(clip.z) || clip.z < 0 ||
        clip.z > clip.w)
        return {};
    const GizmoPoint result{rectangle.x + (clip.x / clip.w + 1.0) * rectangle.width * .5,
                            rectangle.y + (clip.y / clip.w + 1.0) * rectangle.height * .5};
    if (!std::isfinite(result.x) || !std::isfinite(result.y) || std::abs(result.x) > 1e7 ||
        std::abs(result.y) > 1e7)
        return {};
    return result;
}
double distance(GizmoPoint a, GizmoPoint b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}
double segment_distance(GizmoPoint p, GizmoPoint a, GizmoPoint b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double squared = dx * dx + dy * dy;
    if (squared == 0)
        return distance(p, a);
    const double t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / squared, 0.0, 1.0);
    return distance(p, {a.x + t * dx, a.y + t * dy});
}
double axis_parameter(const Ray& value, const math::Mat4& frame, std::size_t axis) {
    const math::Vec3 direction{frame[static_cast<int>(axis)]};
    const math::Vec3 offset = value.origin - math::Vec3{frame[3]};
    const double a = glm::dot(direction, direction);
    const double b = glm::dot(direction, value.direction);
    const double denominator = a - b * b;
    require(std::isfinite(denominator) && denominator > a * 1e-8,
            "Gizmo axis is too close to the view direction");
    const double parameter =
        (glm::dot(direction, offset) - b * glm::dot(value.direction, offset)) / denominator;
    require(std::isfinite(parameter), "Gizmo axis displacement is not finite");
    return parameter;
}
double ring_angle(const Ray& value, const math::Mat4& inverse, std::size_t axis) {
    const math::Vec3 origin{inverse * glm::dvec4{value.origin, 1.0}};
    const math::Vec3 direction{inverse * glm::dvec4{value.direction, 0.0}};
    const auto length = std::hypot(direction.x, direction.y, direction.z);
    const auto index = static_cast<int>(axis);
    require(finite(origin) && finite(direction) && std::isfinite(length) && length > 0 &&
                std::abs(direction[index]) > length * 1e-5,
            "Gizmo rotation plane is edge-on");
    const auto t = -origin[index] / direction[index];
    require(std::isfinite(t) && t >= 0, "Gizmo rotation plane is behind the near plane");
    const auto point = origin + t * direction;
    const auto u = static_cast<int>((axis + 1) % 3), v = static_cast<int>((axis + 2) % 3);
    require(finite(point) && std::hypot(point[u], point[v]) > 1e-12,
            "Gizmo rotation pointer is at the pivot");
    return std::atan2(point[v], point[u]);
}
bool valid_snap(const tools::TransformSnap& snap) {
    const auto good = [](const auto& value) {
        return !value || (std::isfinite(*value) && *value > 0);
    };
    return good(snap.translation) && good(snap.rotation_radians) && good(snap.scale);
}
} // namespace

Result<ViewportGizmoGeometry> make_viewport_gizmo(const RenderScene& scene,
                                                  const TransformObservation& observed,
                                                  tools::TransformOperation operation,
                                                  tools::TransformSpace space,
                                                  const ViewportRectangle& rectangle) {
    return boundary<ViewportGizmoGeometry>([&] {
        require(scene.revision() == observed.selection.revision, "Gizmo observation is stale",
                ErrorCode::conflict);
        require(
            (operation == tools::TransformOperation::translate ||
             operation == tools::TransformOperation::rotate ||
             operation == tools::TransformOperation::scale) &&
                (space == tools::TransformSpace::local || space == tools::TransformSpace::world),
            "Unknown gizmo operation or space");
        static_cast<void>(take(viewport_coordinates(rectangle, rectangle.x, rectangle.y)));
        require(std::abs(rectangle.x) <= 1e7 && std::abs(rectangle.y) <= 1e7 &&
                    rectangle.width <= 1e6 && rectangle.height <= 1e6,
                "Gizmo image rectangle exceeds logical bounds");
        const auto local = take(canonicalize_transform(observed.local));
        auto parent_valid = validate_transform_matrix(observed.parent_world);
        if (!parent_valid)
            throw Failure{parent_valid.error()};
        const auto world = observed.parent_world * take(transform_matrix(local));
        auto world_valid = validate_transform_matrix(world);
        if (!world_valid)
            throw Failure{world_valid.error()};
        const math::Vec3 pivot = math::Vec3{world[3]} - scene.origin().scene_local_offset;
        require(finite(pivot) && std::abs(pivot.x) <= render_coordinate_limit &&
                    std::abs(pivot.y) <= render_coordinate_limit &&
                    std::abs(pivot.z) <= render_coordinate_limit,
                "Gizmo pivot exceeds the render origin bounds");
        ViewportGizmoGeometry result;
        result.rectangle_ = rectangle;
        result.revision_ = scene.revision();
        result.operation_ = operation;
        result.space_ = space;
        result.view_projection_ = scene.camera().view_projection;
        result.inverse_view_projection_ = glm::inverse(result.view_projection_);
        matrix_finite(result.inverse_view_projection_);
        const auto projected = project(result.view_projection_, rectangle, pivot);
        require(projected.has_value(), "Gizmo pivot is outside the camera clip range",
                ErrorCode::capability_unavailable);
        result.pivot_ = *projected;
        auto frame = space == tools::TransformSpace::local
                         ? observed.parent_world * glm::mat4_cast(local.rotation)
                         : math::Mat4{1.0};
        double largest = 0;
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                largest = std::max(largest, std::abs(frame[c][r]));
        require(std::isfinite(largest) && largest > 0, "Gizmo axis frame is not finite");
        for (int c = 0; c < 3; ++c)
            frame[c] = glm::dvec4{math::Vec3{frame[c]} / largest, 0.0};
        frame[3] = glm::dvec4{pivot, 1.0};
        result.frame_ = frame;
        result.inverse_frame_ = take(inverse_transform_matrix(frame));
        result.translation_scale_ = 1.0 / largest;
        require(std::isfinite(result.translation_scale_) && result.translation_scale_ > 0,
                "Gizmo axis units cannot be represented");
        const auto camera_point = scene.camera().view * glm::dvec4{pivot, 1.0};
        const double probe = std::clamp(std::abs(camera_point.z) * .01, 1e-6, 1.0);
        double pixels_per_unit = 0;
        for (int c = 0; c < 3; ++c) {
            const auto endpoint =
                project(result.view_projection_, rectangle, pivot + math::Vec3{frame[c]} * probe);
            if (endpoint)
                pixels_per_unit =
                    std::max(pixels_per_unit, distance(*endpoint, *projected) / probe);
        }
        require(std::isfinite(pixels_per_unit) && pixels_per_unit > 1e-12,
                "Gizmo axes cannot be projected");
        result.radius_ = 80.0 / pixels_per_unit;
        require(std::isfinite(result.radius_) && result.radius_ > 0, "Gizmo radius is not finite");
        for (std::size_t axis = 0; axis < 3; ++axis) {
            auto& output = result.axes_[axis];
            if (operation != tools::TransformOperation::rotate) {
                const auto endpoint =
                    project(result.view_projection_, rectangle,
                            pivot + math::Vec3{frame[static_cast<int>(axis)]} * result.radius_);
                if (endpoint && distance(*endpoint, *projected) >= 12.0) {
                    output.visible = true;
                    output.points[0] = *projected;
                    output.points[1] = *endpoint;
                    output.point_count = 2;
                }
                continue;
            }
            const auto u = static_cast<int>((axis + 1) % 3), v = static_cast<int>((axis + 2) % 3);
            bool visible = true;
            for (std::size_t index = 0; index < output.points.size(); ++index) {
                const double angle = 2.0 * std::numbers::pi * static_cast<double>(index) / 64.0;
                const auto point =
                    pivot + result.radius_ * (math::Vec3{frame[u]} * std::cos(angle) +
                                              math::Vec3{frame[v]} * std::sin(angle));
                const auto screen = project(result.view_projection_, rectangle, point);
                if (!screen) {
                    visible = false;
                    break;
                }
                output.points[index] = *screen;
            }
            // Reject visually edge-on rings by their projected two-axis area.
            const auto a = output.points[0], b = output.points[16];
            const double area = std::abs((a.x - projected->x) * (b.y - projected->y) -
                                         (a.y - projected->y) * (b.x - projected->x));
            output.visible = visible && area >= 48.0;
            output.point_count = output.visible ? output.points.size() : 0;
        }
        return result;
    });
}

Result<std::optional<std::size_t>> hit_viewport_gizmo(const ViewportGizmoGeometry& geometry,
                                                      double x, double y) {
    return boundary<std::optional<std::size_t>>([&] {
        std::optional<std::size_t> found;
        if (!inside(geometry.rectangle(), x, y))
            return found;
        double nearest = 7.0;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto& handle = geometry.axes()[axis];
            if (!handle.visible)
                continue;
            // Reserve the pivot for ordinary picking; shafts begin beyond it.
            if (geometry.operation() != tools::TransformOperation::rotate &&
                distance({x, y}, geometry.pivot()) < 12.0)
                continue;
            for (std::size_t index = 1; index < handle.point_count; ++index) {
                const double candidate =
                    segment_distance({x, y}, handle.points[index - 1], handle.points[index]);
                if (candidate <= nearest) {
                    nearest = candidate;
                    found = axis;
                }
            }
        }
        return found;
    });
}

Result<void> ViewportGizmoGesture::begin(const ViewportGizmoGeometry& geometry, std::size_t axis,
                                         double x, double y, tools::TransformSnap snap) {
    return boundary<void>([&] {
        require(!geometry_, "Finish or cancel the active gizmo gesture", ErrorCode::conflict);
        require(axis < 3 && geometry.axes_[axis].visible, "Gizmo axis is unavailable");
        require(valid_snap(snap), "Gizmo snap steps must be finite and positive");
        const auto pointer = ray(geometry.inverse_view_projection_, geometry.rectangle_, x, y);
        const double parameter = geometry.operation_ == tools::TransformOperation::rotate
                                     ? ring_angle(pointer, geometry.inverse_frame_, axis)
                                     : axis_parameter(pointer, geometry.frame_, axis);
        geometry_ = geometry;
        axis_ = axis;
        snap_ = snap;
        start_parameter_ = parameter;
        previous_angle_ = parameter;
        accumulated_angle_ = 0;
        return Result<void>{};
    });
}
Result<tools::TransformManipulation> ViewportGizmoGesture::update(double x, double y) {
    return boundary<tools::TransformManipulation>([&] {
        require(geometry_.has_value(), "No active gizmo gesture", ErrorCode::conflict);
        const auto& geometry = *geometry_;
        const auto pointer = ray(geometry.inverse_view_projection_, geometry.rectangle_, x, y);
        tools::TransformManipulation result;
        result.operation = geometry.operation_;
        result.space = geometry.space_;
        result.snap = snap_;
        const auto index = static_cast<int>(axis_);
        if (result.operation == tools::TransformOperation::rotate) {
            const auto angle = ring_angle(pointer, geometry.inverse_frame_, axis_);
            const auto delta = std::remainder(angle - previous_angle_, 2.0 * std::numbers::pi);
            const auto accumulated = accumulated_angle_ + delta;
            require(std::isfinite(accumulated) &&
                        std::abs(accumulated) <= 1024.0 * std::numbers::pi,
                    "Gizmo rotation exceeds the gesture bound");
            result.rotation_axis = {0, 0, 0};
            result.rotation_axis[index] = 1;
            result.rotation_radians = accumulated;
            previous_angle_ = angle;
            accumulated_angle_ = accumulated;
        } else {
            const auto parameter =
                axis_parameter(pointer, geometry.frame_, axis_) - start_parameter_;
            const double value = result.operation == tools::TransformOperation::translate
                                     ? parameter * geometry.translation_scale_
                                     : 1.0 + parameter / geometry.radius_;
            require(std::isfinite(value), "Gizmo manipulation is not finite");
            if (result.operation == tools::TransformOperation::translate)
                result.translation[index] = value;
            else
                result.scale_factors[index] = value;
        }
        return result;
    });
}
void ViewportGizmoGesture::cancel() noexcept {
    geometry_.reset();
}

} // namespace shark::editor
