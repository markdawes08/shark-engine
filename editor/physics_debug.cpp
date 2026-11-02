#include "physics_debug.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace shark::editor {
namespace {
struct Failure {
    Error error;
};
void require(bool condition, const char* message) {
    if (!condition)
        throw Failure{
            {ErrorCode::invalid_argument, message, {.operation = "editor.physics.debug"}}};
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
                     "Cannot prepare physics debug geometry",
                     {.operation = "editor.physics.debug"}};
    }
}
bool finite(const math::Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool bounded(const math::Vec3& value, double limit) {
    return finite(value) && std::abs(value.x) <= limit && std::abs(value.y) <= limit &&
           std::abs(value.z) <= limit;
}
bool dimension(double value) {
    return std::isfinite(value) && value >= physics_minimum_dimension &&
           value <= physics_maximum_dimension;
}
bool valid_kind(PhysicsDebugKind kind) {
    return kind == PhysicsDebugKind::static_body || kind == PhysicsDebugKind::dynamic_body ||
           kind == PhysicsDebugKind::kinematic_body || kind == PhysicsDebugKind::trigger ||
           kind == PhysicsDebugKind::query || kind == PhysicsDebugKind::hit;
}
constexpr std::size_t circle_segments = 24;

struct Lines {
    std::vector<PhysicsDebugLine> values;
    void add(math::Vec3 first, math::Vec3 second, PhysicsDebugKind kind) {
        require(values.size() < physics_debug_maximum_lines, "Physics debug line limit exceeded");
        require(bounded(first, 1e6) && bounded(second, 1e6),
                "Physics debug line is not finite or exceeds coordinate bounds");
        values.push_back({first, second, kind});
    }
    template <typename F> void curve(F&& point, std::size_t segments, PhysicsDebugKind kind) {
        auto previous = point(0.0);
        for (std::size_t i = 1; i <= segments; ++i) {
            auto next = point(static_cast<double>(i) / static_cast<double>(segments));
            add(previous, next, kind);
            previous = next;
        }
    }
};

template <typename F>
void box(Lines& lines, const math::Vec3& half, PhysicsDebugKind kind, F&& point) {
    std::array<math::Vec3, 8> vertices;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const math::Vec3 p{(i & 1U) ? half.x : -half.x, (i & 2U) ? half.y : -half.y,
                           (i & 4U) ? half.z : -half.z};
        vertices[i] = point(p);
    }
    for (std::size_t i = 0; i < vertices.size(); ++i)
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto bit = std::size_t{1} << axis;
            if ((i & bit) == 0)
                lines.add(vertices[i], vertices[i | bit], kind);
        }
}
template <typename F> void sphere(Lines& lines, double radius, PhysicsDebugKind kind, F&& point) {
    for (int axis = 0; axis < 3; ++axis)
        lines.curve(
            [&](double t) {
                const double angle = t * 2 * std::numbers::pi;
                math::Vec3 p{0};
                p[(axis + 1) % 3] = radius * std::cos(angle);
                p[(axis + 2) % 3] = radius * std::sin(angle);
                return point(p);
            },
            circle_segments, kind);
}
template <typename F>
void capsule(Lines& lines, double radius, double half_height, PhysicsDebugKind kind, F&& point) {
    for (int end : {-1, 1}) {
        const double y = static_cast<double>(end) * half_height;
        lines.curve(
            [&](double t) {
                const double angle = t * 2 * std::numbers::pi;
                return point({radius * std::cos(angle), y, radius * std::sin(angle)});
            },
            circle_segments, kind);
        for (int axis : {0, 2})
            lines.curve(
                [&](double t) {
                    const double angle = t * std::numbers::pi;
                    math::Vec3 p{0};
                    p[axis] = radius * std::cos(angle);
                    p.y = y + static_cast<double>(end) * radius * std::sin(angle);
                    return point(p);
                },
                circle_segments / 2, kind);
    }
    for (int axis : {0, 2})
        for (int sign : {-1, 1}) {
            math::Vec3 lower{0}, upper{0};
            lower[axis] = upper[axis] = static_cast<double>(sign) * radius;
            lower.y = -half_height;
            upper.y = half_height;
            lines.add(point(lower), point(upper), kind);
        }
}
PhysicsDebugKind body_kind(const RigidBody& body) {
    if (body.is_trigger)
        return PhysicsDebugKind::trigger;
    if (body.motion == BodyMotion::dynamic_body)
        return PhysicsDebugKind::dynamic_body;
    if (body.motion == BodyMotion::kinematic_body)
        return PhysicsDebugKind::kinematic_body;
    return PhysicsDebugKind::static_body;
}
void add_body(Lines& lines, const PhysicsDebugBody& body) {
    const auto pose = take(validate_rigid_body_pose(body.settings, body.pose));
    const double scale = pose.scale.x;
    const auto point = [&](const math::Vec3& value) {
        return pose.translation + pose.rotation * value;
    };
    const auto kind = body_kind(body.settings);
    switch (body.settings.shape) {
    case ColliderShape::box:
        box(lines, body.settings.half_extents * scale, kind, point);
        break;
    case ColliderShape::sphere:
        sphere(lines, body.settings.radius * scale, kind, point);
        break;
    case ColliderShape::capsule:
        capsule(lines, body.settings.radius * scale, body.settings.half_height * scale, kind,
                point);
        break;
    }
}
void add_query(Lines& lines, const PhysicsDebugQuery& query) {
    require(query.hit_positions.size() <= physics_debug_maximum_hits,
            "Physics debug query hit limit exceeded");
    require(bounded(query.first, physics_local_position_limit), "Invalid query origin");
    const auto point = [&](const math::Vec3& value) { return query.first + value; };
    switch (query.kind) {
    case PhysicsDebugQueryKind::raycast:
        require(bounded(query.second, physics_local_position_limit) && query.first != query.second,
                "Invalid debug ray");
        lines.add(query.first, query.second, PhysicsDebugKind::query);
        break;
    case PhysicsDebugQueryKind::overlap_sphere:
        require(dimension(query.radius) &&
                    bounded(query.first + math::Vec3{query.radius}, physics_local_position_limit) &&
                    bounded(query.first - math::Vec3{query.radius}, physics_local_position_limit),
                "Invalid debug overlap sphere");
        sphere(lines, query.radius, PhysicsDebugKind::query, point);
        break;
    case PhysicsDebugQueryKind::overlap_box:
        require(dimension(query.second.x) && dimension(query.second.y) &&
                    dimension(query.second.z) &&
                    bounded(query.first + query.second, physics_local_position_limit) &&
                    bounded(query.first - query.second, physics_local_position_limit),
                "Invalid debug overlap box");
        box(lines, query.second, PhysicsDebugKind::query, point);
        break;
    default:
        require(false, "Invalid debug query type");
    }
    for (const auto& hit : query.hit_positions) {
        require(bounded(hit, physics_local_position_limit), "Invalid debug hit position");
        constexpr double mark = 0.1;
        for (int axis = 0; axis < 3; ++axis) {
            math::Vec3 delta{0};
            delta[axis] = mark;
            lines.add(hit - delta, hit + delta, PhysicsDebugKind::hit);
        }
    }
}

// Clip one homogeneous segment against x,y in [-w,w], z in [0,w], and a
// strictly positive w so perspective division remains bounded.
std::optional<std::pair<glm::dvec4, glm::dvec4>> clip(glm::dvec4 a, glm::dvec4 b) {
    const auto planes = [](const glm::dvec4& p) {
        return std::array<double, 7>{p.w + p.x, p.w - p.x, p.w + p.y, p.w - p.y,
                                     p.z,       p.w - p.z, p.w - 1e-9};
    };
    const auto first = planes(a), second = planes(b);
    double lower = 0, upper = 1;
    for (std::size_t i = 0; i < first.size(); ++i) {
        require(std::isfinite(first[i]) && std::isfinite(second[i]),
                "Physics debug clip coordinates are not finite");
        if (first[i] < 0 && second[i] < 0)
            return {};
        if ((first[i] < 0) != (second[i] < 0)) {
            const double t = first[i] / (first[i] - second[i]);
            require(std::isfinite(t), "Physics debug clipping failed");
            if (first[i] < 0)
                lower = std::max(lower, t);
            else
                upper = std::min(upper, t);
            if (lower > upper)
                return {};
        }
    }
    return std::pair{a + (b - a) * lower, a + (b - a) * upper};
}
} // namespace

Result<std::vector<PhysicsDebugLine>>
make_physics_debug_lines(std::span<const PhysicsDebugBody> bodies,
                         const std::optional<PhysicsDebugQuery>& query) {
    return boundary<std::vector<PhysicsDebugLine>>([&] {
        require(bodies.size() <= physics_debug_maximum_bodies, "Physics debug body limit exceeded");
        Lines lines;
        for (const auto& body : bodies)
            add_body(lines, body);
        if (query)
            add_query(lines, *query);
        return std::move(lines.values);
    });
}

Result<std::vector<PhysicsDebugScreenLine>>
project_physics_debug_lines(std::span<const PhysicsDebugLine> lines,
                            const math::Mat4& view_projection, const math::Vec3& scene_local_offset,
                            const ViewportRectangle& rectangle) {
    return boundary<std::vector<PhysicsDebugScreenLine>>([&] {
        require(lines.size() <= physics_debug_maximum_lines, "Physics debug line limit exceeded");
        require(bounded(scene_local_offset, 1e7), "Invalid render origin");
        require(std::isfinite(rectangle.x) && std::isfinite(rectangle.y) &&
                    std::isfinite(rectangle.width) && std::isfinite(rectangle.height) &&
                    std::abs(rectangle.x) <= 1e7 && std::abs(rectangle.y) <= 1e7 &&
                    rectangle.width > 0 && rectangle.height > 0 && rectangle.width <= 1e6 &&
                    rectangle.height <= 1e6,
                "Invalid physics debug image rectangle");
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                require(std::isfinite(view_projection[c][r]), "Invalid physics debug camera");
        std::vector<PhysicsDebugScreenLine> output;
        output.reserve(lines.size());
        for (const auto& line : lines) {
            require(bounded(line.first, 1e6) && bounded(line.second, 1e6) && valid_kind(line.kind),
                    "Invalid physics debug line");
            const auto a = view_projection * glm::dvec4{line.first - scene_local_offset, 1};
            const auto b = view_projection * glm::dvec4{line.second - scene_local_offset, 1};
            auto clipped = clip(a, b);
            if (!clipped)
                continue;
            const auto& [p, q] = *clipped;
            PhysicsDebugScreenLine projected{rectangle.x + (p.x / p.w + 1) * rectangle.width * .5,
                                             rectangle.y + (p.y / p.w + 1) * rectangle.height * .5,
                                             rectangle.x + (q.x / q.w + 1) * rectangle.width * .5,
                                             rectangle.y + (q.y / q.w + 1) * rectangle.height * .5,
                                             line.kind};
            require(std::isfinite(projected.x0) && std::isfinite(projected.y0) &&
                        std::isfinite(projected.x1) && std::isfinite(projected.y1),
                    "Physics debug projection is not finite");
            output.push_back(projected);
        }
        return output;
    });
}
} // namespace shark::editor
