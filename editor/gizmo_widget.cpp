#include "gizmo_widget.hpp"
#include "transform_drag.hpp"
#include "viewport_gizmo.hpp"

#include <array>
#include <cmath>
#include <imgui.h>
#include <numbers>

namespace shark::editor {
namespace {
using tools::TransformOperation;
using tools::TransformSpace;

bool same_rectangle(const ViewportRectangle& left, const ViewportRectangle& right) noexcept {
    return left.x == right.x && left.y == right.y && left.width == right.width &&
           left.height == right.height;
}
bool drawable(const ViewportGizmoGeometry& geometry) noexcept {
    const auto finite_point = [](GizmoPoint point) {
        return std::isfinite(point.x) && std::isfinite(point.y) && std::abs(point.x) <= 1e7 &&
               std::abs(point.y) <= 1e7;
    };
    const auto& rectangle = geometry.rectangle();
    if (!finite_point({rectangle.x, rectangle.y}) ||
        !finite_point({rectangle.x + rectangle.width, rectangle.y + rectangle.height}))
        return false;
    for (const auto& axis : geometry.axes()) {
        if (axis.point_count > axis.points.size())
            return false;
        for (std::size_t index = 0; index < axis.point_count; ++index)
            if (!finite_point(axis.points[index]))
                return false;
    }
    return finite_point(geometry.pivot());
}
ImVec2 screen(GizmoPoint point) noexcept {
    return {static_cast<float>(point.x), static_cast<float>(point.y)};
}
void draw_geometry(const ViewportGizmoGeometry& geometry, std::optional<std::size_t> selected) {
    if (!drawable(geometry))
        return;
    const auto& rectangle = geometry.rectangle();
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(screen({rectangle.x, rectangle.y}),
                       screen({rectangle.x + rectangle.width, rectangle.y + rectangle.height}),
                       true);
    constexpr std::array colors{IM_COL32(240, 90, 90, 255), IM_COL32(105, 220, 115, 255),
                                IM_COL32(100, 160, 255, 255)};
    constexpr std::array labels{"X", "Y", "Z"};
    for (std::size_t index = 0; index < geometry.axes().size(); ++index) {
        const auto& axis = geometry.axes()[index];
        if (!axis.visible || axis.point_count < 2)
            continue;
        const auto color = selected == index ? IM_COL32(255, 235, 125, 255) : colors[index];
        const float width = selected == index ? 4.0F : 2.5F;
        for (std::size_t point = 1; point < axis.point_count; ++point)
            draw->AddLine(screen(axis.points[point - 1]), screen(axis.points[point]), color, width);
        if (geometry.operation() != TransformOperation::rotate) {
            const auto tip = screen(axis.points[axis.point_count - 1]);
            if (geometry.operation() == TransformOperation::scale)
                draw->AddRectFilled({tip.x - 5, tip.y - 5}, {tip.x + 5, tip.y + 5}, color);
            else
                draw->AddCircleFilled(tip, 5, color, 12);
            draw->AddText({tip.x + 7, tip.y - 7}, color, labels[index]);
        }
    }
    draw->AddCircle(screen(geometry.pivot()), 4, IM_COL32(240, 240, 240, 255), 12, 1.5F);
    draw->PopClipRect();
}
} // namespace

struct GizmoWidget::Impl {
    ScenePanel& panel;
    TransformDrag drag;
    ViewportGizmoGesture gesture;
    std::optional<SceneSelection> selection;
    std::optional<TransformOperation> operation;
    TransformSpace space{TransformSpace::local};
    tools::TransformSnap snap;
    bool snapping{false};
    double translation_step{1};
    double rotation_degrees{15};
    double scale_step{.1};
    bool wait_for_release{false};
    RenderOrigin origin;
    RenderViewport viewport;
    EntityHandle camera;
    std::optional<Error> error;
    bool invalid_configuration{false};

    explicit Impl(ScenePanel& value) : panel(value), drag(value) {}

    void cancel() noexcept {
        static_cast<void>(drag.cancel());
        gesture.cancel();
        selection.reset();
        wait_for_release = true;
    }
    std::optional<tools::AuthoringTransformPreview> preview() const {
        if (!selection || !drag.active() || drag.stale() || !drag.candidate())
            return {};
        return tools::AuthoringTransformPreview{selection->id, *drag.candidate()};
    }
    Result<void> configure(std::optional<TransformOperation> desired_operation,
                           TransformSpace desired_space, tools::TransformSnap desired_snap) {
        cancel();
        tools::TransformManipulation probe;
        probe.operation = desired_operation.value_or(TransformOperation::translate);
        probe.space = desired_space;
        probe.snap = desired_snap;
        const auto valid = tools::calculate_manipulated_transform({}, math::Mat4{1}, probe);
        invalid_configuration = !valid;
        if (!valid) {
            error = valid.error();
            return valid.error();
        }
        operation = desired_operation;
        space = desired_space;
        snap = desired_snap;
        error.reset();
        return {};
    }
};

GizmoWidget::GizmoWidget(ScenePanel& panel) : impl_(std::make_unique<Impl>(panel)) {}
GizmoWidget::~GizmoWidget() = default;

Result<void> GizmoWidget::configure(std::optional<TransformOperation> operation,
                                    TransformSpace space, tools::TransformSnap snap) {
    auto& value = *impl_;
    value.snapping = snap.translation || snap.rotation_radians || snap.scale;
    if (snap.translation)
        value.translation_step = *snap.translation;
    if (snap.rotation_radians)
        value.rotation_degrees = *snap.rotation_radians * (180.0 / std::numbers::pi);
    if (snap.scale)
        value.scale_step = *snap.scale;
    return value.configure(operation, space, snap);
}

void GizmoWidget::controls(bool blocked) {
    auto& value = *impl_;
    ImGui::PushID(this);
    ImGui::BeginDisabled(blocked);
    int mode = !value.operation ? 0 : static_cast<int>(*value.operation) + 1;
    constexpr const char* modes[]{"Select", "Translate", "Rotate", "Scale"};
    ImGui::SetNextItemWidth(140);
    bool changed = ImGui::Combo("Tool", &mode, modes, 4);
    ImGui::SameLine();
    int space = value.space == TransformSpace::local ? 0 : 1;
    constexpr const char* spaces[]{"Local", "World"};
    ImGui::SetNextItemWidth(105);
    changed = ImGui::Combo("Space", &space, spaces, 2) || changed;
    changed = ImGui::Checkbox("Snap", &value.snapping) || changed;
    if (value.snapping && mode != 0) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        if (mode == 1)
            changed =
                ImGui::InputDouble("meters", &value.translation_step, 0, 0, "%.6g") || changed;
        else if (mode == 2)
            changed =
                ImGui::InputDouble("degrees", &value.rotation_degrees, 0, 0, "%.6g") || changed;
        else
            changed = ImGui::InputDouble("factor step", &value.scale_step, 0, 0, "%.6g") || changed;
    }
    ImGui::EndDisabled();
    if (changed) {
        tools::TransformSnap snap;
        if (value.snapping)
            snap = {value.translation_step, value.rotation_degrees * (std::numbers::pi / 180.0),
                    value.scale_step};
        const auto operation = mode == 0 ? std::optional<TransformOperation>{}
                                         : std::optional{static_cast<TransformOperation>(mode - 1)};
        static_cast<void>(value.configure(
            operation, space == 0 ? TransformSpace::local : TransformSpace::world, snap));
    }
    if (value.operation)
        ImGui::TextWrapped("Drag an axis handle; release to apply once. Esc cancels. "
                           "Axes viewed edge-on are unavailable; orbit to another angle.");
    if (value.operation) {
        // Error appearance/clearing must not move the image during the next
        // gesture, which would correctly trip its frozen-rectangle fence.
        ImGui::BeginChild("Gizmo status", {0, ImGui::GetTextLineHeightWithSpacing() * 2},
                          ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (value.error) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1, .55F, .43F, 1});
            ImGui::TextWrapped("%s", value.error->message.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextWrapped("%s", value.error->message.c_str());
                ImGui::EndTooltip();
            }
        }
        ImGui::EndChild();
    }
    ImGui::PopID();
}

GizmoFrameResult GizmoWidget::overlay(const client::InputSnapshot& input,
                                      const ViewportRectangle& rectangle, const RenderScene& scene,
                                      bool hovered, bool blocked,
                                      const GizmoPreviewValidator& validate_preview) {
    auto& value = *impl_;
    GizmoFrameResult result;
    result.consumed = value.drag.active() || value.gesture.active();
    const auto fail = [&](Error error) {
        value.error = error;
        result.error = std::move(error);
        value.cancel();
    };
    const auto left = input.mouse_button(client::MouseButton::left);
    if (!left.held && !left.pressed)
        value.wait_for_release = false;
    const auto point = viewport_coordinates(rectangle, input.cursor.x, input.cursor.y);
    if (blocked || value.invalid_configuration || !value.operation || !input.focused ||
        input.focus_lost || input.key(client::Key::escape).pressed || !input.cursor.valid ||
        !point || !point.value() || !hovered || !value.panel.current() ||
        value.panel.needs_reconciliation() || !value.panel.selection() ||
        scene.revision() != value.panel.revision() ||
        input.mouse_button(client::MouseButton::right).held ||
        input.mouse_button(client::MouseButton::middle).held) {
        value.cancel();
        // Selection handles remain visible while the pointer is elsewhere.
        // This read-only drawing does not grant capture through a blocked pane.
        if (value.operation && !value.invalid_configuration && value.panel.current() &&
            !value.panel.needs_reconciliation() && value.panel.selection() &&
            scene.revision() == value.panel.revision()) {
            const auto observed = value.panel.observe_transform(*value.panel.selection());
            if (observed) {
                const auto geometry = make_viewport_gizmo(scene, observed.value(), *value.operation,
                                                          value.space, rectangle);
                if (geometry)
                    draw_geometry(geometry.value(), {});
            }
        }
        return result;
    }
    if (value.drag.active() &&
        (value.drag.stale() || !value.gesture.geometry() ||
         !same_rectangle(value.gesture.geometry()->rectangle(), rectangle) ||
         value.gesture.geometry()->view_projection() != scene.camera().view_projection ||
         value.origin != scene.origin() || value.viewport != scene.viewport() ||
         value.camera != scene.camera_entity().handle)) {
        value.cancel();
        return result;
    }
    if (value.drag.active() && left.pressed) {
        // Release/repress between captures loses the original release position.
        // Do not join two physical gestures or commit at the newer press.
        value.cancel();
        return result;
    }
    // A real new press inside the current image starts a new intent, even if
    // the preceding up frame was outside. A held button has no press edge and
    // cannot revive capture. Cancellation branches above never restart here.
    if (left.pressed && left.held)
        value.wait_for_release = false;
    if (!value.drag.active()) {
        const auto observation = value.panel.observe_transform(*value.panel.selection());
        if (!observation) {
            fail(observation.error());
            return result;
        }
        const auto geometry = make_viewport_gizmo(scene, observation.value(), *value.operation,
                                                  value.space, rectangle);
        if (!geometry) {
            fail(geometry.error());
            return result;
        }
        if (!drawable(geometry.value())) {
            fail({ErrorCode::invalid_argument,
                  "Gizmo image coordinates exceed drawable bounds",
                  {.operation = "editor.gizmo"}});
            return result;
        }
        auto hit = hit_viewport_gizmo(geometry.value(), input.cursor.x, input.cursor.y);
        if (!hit) {
            fail(hit.error());
            return result;
        }
        draw_geometry(geometry.value(), hit.value());
        // Even a complete press/release captured in one snapshot belongs to a
        // handle, not to geometry picking underneath it. It creates no drag.
        result.consumed = left.pressed && hit.value().has_value();
        if (!left.pressed || !left.held || value.wait_for_release || !hit.value())
            return result;
        result.consumed = true;
        const auto began = value.drag.begin();
        if (!began) {
            fail(began.error());
            return result;
        }
        const auto gesture = value.gesture.begin(geometry.value(), *hit.value(), input.cursor.x,
                                                 input.cursor.y, value.snap);
        if (!gesture) {
            fail(gesture.error());
            return result;
        }
        value.selection = observation.value().selection;
        value.origin = scene.origin();
        value.viewport = scene.viewport();
        value.camera = scene.camera_entity().handle;
    } else {
        draw_geometry(*value.gesture.geometry(), value.gesture.axis());
    }
    result.consumed = true;
    if (!left.held && !left.released) {
        value.cancel();
        return result;
    }
    const auto motion = value.gesture.update(input.cursor.x, input.cursor.y);
    if (!motion) {
        fail(motion.error());
        return result;
    }
    const auto updated = value.drag.update(motion.value());
    if (!updated) {
        if (updated.error().code == ErrorCode::invalid_argument && left.held) {
            // The shared drag has already discarded its newest candidate. Keep
            // the captured gesture so motion through an invalid zero-scale
            // interval can reach a valid signed scale without a new baseline.
            // With no candidate the workspace restores committed rendering;
            // releasing here still cancels rather than applying an older pose.
            value.error = updated.error();
            result.error = updated.error();
            return result;
        }
        fail(updated.error());
        return result;
    }
    const auto candidate = value.preview();
    if (!candidate) {
        fail({ErrorCode::conflict,
              "Gizmo candidate is no longer current",
              {.operation = "editor.gizmo"}});
        return result;
    }
    if (validate_preview) {
        const auto rendered = validate_preview(*candidate);
        if (!rendered) {
            fail(rendered.error());
            return result;
        }
    }
    value.error.reset();
    if (left.released && !left.held) {
        auto committed = value.drag.commit();
        value.gesture.cancel();
        value.selection.reset();
        value.wait_for_release = true;
        if (!committed) {
            value.error = committed.error();
            result.error = committed.error();
        } else {
            result.receipt = std::move(committed).value();
            value.error = result.receipt->refresh_error;
        }
    }
    return result;
}

void GizmoWidget::cancel() noexcept {
    impl_->cancel();
}
bool GizmoWidget::has_changes() const noexcept {
    return impl_->drag.active();
}
std::optional<tools::AuthoringTransformPreview> GizmoWidget::preview() const {
    return impl_->preview();
}

} // namespace shark::editor
