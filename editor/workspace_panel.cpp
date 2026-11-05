#include "workspace_panel.hpp"
#include "asset_widget.hpp"
#include "console_widget.hpp"
#include "gizmo_widget.hpp"
#include "input_trace.hpp"
#include "inspector_widget.hpp"
#include "physics_panel.hpp"
#include "prefab_panel.hpp"
#include "scene_panel.hpp"
#include "script_panel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <imgui.h>
#include <optional>
#include <string>
#include <vector>

namespace shark::editor {
namespace {
using namespace client;
template <std::size_t N> void text_buffer(std::array<char, N>& buffer, std::string_view text) {
    const auto count = std::min(text.size(), N - 1);
    std::copy_n(text.data(), count, buffer.data());
    buffer[count] = '\0';
}
void error_text(const Error& error) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1, .55F, .43F, 1});
    ImGui::TextWrapped("%s", error.message.c_str());
    ImGui::PopStyleColor();
    if (!error.context.path.empty())
        ImGui::TextWrapped("%s", error.context.path.c_str());
    if (!error.context.entity_id.empty())
        ImGui::TextWrapped("Entity: %s", error.context.entity_id.c_str());
}
bool selectable_literal(const std::string& text, bool selected) {
    const auto position = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::Selectable("##entity", selected, ImGuiSelectableFlags_None,
                                           {0, ImGui::GetTextLineHeight()});
    ImGui::GetWindowDrawList()->AddText(position, ImGui::GetColorU32(ImGuiCol_Text), text.data(),
                                        text.data() + text.size());
    return clicked;
}
} // namespace

ViewportPointerActions ViewportPointerRouter::route(const client::InputSnapshot& input,
                                                    const ViewportRectangle& rectangle,
                                                    bool hovered, bool blocked) noexcept {
    ViewportPointerActions result{};
    const auto& cursor = input.cursor;
    if (blocked || !hovered || !input.focused || input.focus_lost ||
        input.key(client::Key::escape).pressed || !cursor.valid || !std::isfinite(cursor.x) ||
        !std::isfinite(cursor.y) || !std::isfinite(rectangle.x) || !std::isfinite(rectangle.y) ||
        !std::isfinite(rectangle.width) || !std::isfinite(rectangle.height) ||
        rectangle.width <= 0 || rectangle.height <= 0 || cursor.x < rectangle.x ||
        cursor.y < rectangle.y || cursor.x - rectangle.x >= rectangle.width ||
        cursor.y - rectangle.y >= rectangle.height) {
        cancel();
        return result;
    }
    const bool moved =
        rectangle_ &&
        (rectangle_->x != rectangle.x || rectangle_->y != rectangle.y ||
         rectangle_->width != rectangle.width || rectangle_->height != rectangle.height);
    rectangle_ = rectangle;
    if (moved) {
        drag_ = Drag::none;
        return result;
    }
    result.x = cursor.x;
    result.y = cursor.y;
    const auto left = input.mouse_button(client::MouseButton::left);
    const auto right = input.mouse_button(client::MouseButton::right);
    const auto middle = input.mouse_button(client::MouseButton::middle);
    if (left.held || left.pressed || (right.held && middle.held))
        drag_ = Drag::none;
    const auto inside = [&](CursorPosition position) {
        return position.valid && std::isfinite(position.x) && std::isfinite(position.y) &&
               position.x >= rectangle.x && position.y >= rectangle.y &&
               position.x - rectangle.x < rectangle.width &&
               position.y - rectangle.y < rectangle.height;
    };
    const auto right_edges = input.mouse_edges(MouseButton::right);
    const auto middle_edges = input.mouse_edges(MouseButton::middle);
    const bool mixed_navigation = (right.held || right.pressed || right.released) &&
                                  (middle.held || middle.pressed || middle.released);
    if (right_edges.ambiguous || middle_edges.ambiguous || mixed_navigation) {
        drag_ = Drag::none;
        return result;
    }
    const auto begin = [&](MouseButtonEdges edges, Drag drag) {
        if (!edges.press_seen || !inside(edges.press)) {
            drag_ = Drag::none;
            return false;
        }
        drag_ = drag;
        previous_x_ = edges.press.x;
        previous_y_ = edges.press.y;
        return true;
    };
    if (!left.held && !left.pressed && right.pressed && !middle.held)
        static_cast<void>(begin(right_edges, Drag::orbit));
    else if (!left.held && !left.pressed && middle.pressed && !right.held)
        static_cast<void>(begin(middle_edges, Drag::pan));
    const auto active_button = drag_ == Drag::orbit ? right : middle;
    const auto active_edges = drag_ == Drag::orbit ? right_edges : middle_edges;
    const bool finishing = drag_ != Drag::none && !active_button.held && active_button.released;
    const auto motion_cursor =
        finishing ? (active_edges.release_seen ? active_edges.release : CursorPosition{}) : cursor;
    if (drag_ != Drag::none &&
        (!inside(motion_cursor) || (!active_button.held && !active_button.released)))
        drag_ = Drag::none;
    if (drag_ != Drag::none) {
        const auto dx = motion_cursor.x - previous_x_;
        const auto dy = motion_cursor.y - previous_y_;
        if (drag_ == Drag::orbit) {
            result.motion.orbit_x = dx;
            result.motion.orbit_y = dy;
        } else {
            result.motion.pan_x = dx;
            result.motion.pan_y = dy;
        }
    }
    previous_x_ = motion_cursor.x;
    previous_y_ = motion_cursor.y;
    if (finishing)
        drag_ = Drag::none;
    if (drag_ == Drag::none && !right.held && !middle.held && !right.pressed && !middle.pressed &&
        !right.released && !middle.released) {
        const auto edges = input.mouse_edges(MouseButton::left);
        result.pick = left.pressed && edges.press_seen && !edges.ambiguous && inside(edges.press);
        if (result.pick) {
            result.x = edges.press.x;
            result.y = edges.press.y;
        }
        if (!left.held && !left.pressed && std::isfinite(input.scroll.y))
            result.motion.dolly = input.scroll.y;
    }
    return result;
}
void ViewportPointerRouter::cancel() noexcept {
    drag_ = Drag::none;
    rectangle_.reset();
}

struct WorkspacePanel::Impl {
    ProjectSession& session;
    EditorInputTrace* input_trace;
    nlohmann::json trace_observation;
    bool trace_recording{false};
    bool trace_started{false};
    std::uint64_t trace_frame{0};
    ScenePanel hierarchy;
    GizmoWidget gizmo;
    InspectorPanel inspector;
    InspectorWidget inspector_widget;
    AssetPanel assets;
    AssetWidget asset_widget;
    ConsoleWidget console;
    PhysicsPanel physics;
    bool show_physics_debug{false};
    int physics_query_kind{0};
    std::array<double, 3> physics_query_first{0, 0, 0};
    std::array<double, 3> physics_query_second{0, 0, -10};
    double physics_query_radius{1};
    int physics_query_layer{0};
    int physics_query_mask{65535};
    int physics_query_max_hits{64};
    bool physics_query_triggers{true};
    bool physics_query_ignore_selected{false};
    bool physics_debug_truncated{false};
    std::optional<Error> physics_debug_error;
    std::optional<Error> physics_overlay_error;
    ScriptPanel script;
    PrefabPanel prefab;
    SteadyMonotonicClock clock;
    ShaderArtifact vertex;
    ShaderArtifact pixel;
    std::array<char, 4097> script_path{};
    std::array<char, 32769> script_text{};
    std::optional<Error> script_error;
    std::array<char, 4097> prefab_path{};
    std::array<char, 37> prefab_asset{};
    std::vector<char> prefab_text =
        std::vector<char>(tools::authoring_protocol_maximum_request_bytes + 1, '\0');
    std::optional<Error> prefab_error;
    std::array<char, 257> create_name{};
    std::array<char, 65> target{};
    std::array<char, 16385> sidecars{};
    bool child_of_selection{false};
    bool keep_world{true};
    bool image_drawn{false};
    bool refresh_view{false};
    std::optional<AuthoredEntityId> parent_draft;
    std::optional<SceneSelection> edited_selection;
    std::optional<SceneSelection> deleting;
    std::string deleting_name;
    std::optional<SceneSelection> camera;
    std::string camera_name;
    std::optional<SceneViewportRequest> view;
    // Keep the committed request while a preview is displayed. Cancellation
    // restores it by move before submission, without recapturing or allocating.
    std::optional<SceneViewportRequest> committed_view;
    std::optional<tools::AuthoringTransformPreview> shown_preview;
    std::optional<RenderScene> snapshot;
    ViewportNavigation navigation;
    ViewportPointerRouter pointer;
    std::optional<Error> error;
    std::optional<Error> view_error;
    std::optional<Error> physics_error;
    std::string notice;

    Impl(ProjectSession& owner, const ShaderArtifact& vs, const ShaderArtifact& ps,
         EditorInputTrace* trace)
        : session(owner), input_trace(trace), hierarchy(owner), gizmo(hierarchy),
          inspector(hierarchy), inspector_widget(inspector, hierarchy), assets(owner),
          asset_widget(assets, owner), physics(owner), script(owner), prefab(owner), vertex(vs),
          pixel(ps) {
        text_buffer(script_path, "scripts/main.lua");
        text_buffer(prefab_path, "assets/object.sharkprefab");
        text_buffer(create_name, "Entity");
        text_buffer(target, "windows-client");
        refresh();
    }
    void begin_input_trace(const GraphicsWindowFrame& frame, bool blocked) noexcept {
        trace_recording = false;
        if (!input_trace || !input_trace->enabled())
            return;
        trace_started = trace_started || camera.has_value();
        if (!trace_started)
            return;
        try {
            const auto button = [](ButtonState value) {
                return nlohmann::json{
                    {"held", value.held}, {"pressed", value.pressed}, {"released", value.released}};
            };
            const auto pointer_edges = [&](MouseButton value) {
                const auto edges = frame.input.mouse_edges(value);
                return nlohmann::json{
                    {"press", {edges.press.x, edges.press.y, edges.press.valid}},
                    {"release", {edges.release.x, edges.release.y, edges.release.valid}},
                    {"press_seen", edges.press_seen},
                    {"release_seen", edges.release_seen},
                    {"ambiguous", edges.ambiguous}};
            };
            const auto& io = ImGui::GetIO();
            trace_observation = {
                {"input_kind", "captured_interval_not_ordered_events"},
                {"cursor", {frame.input.cursor.x, frame.input.cursor.y, frame.input.cursor.valid}},
                {"left", button(frame.input.mouse_button(MouseButton::left))},
                {"right", button(frame.input.mouse_button(MouseButton::right))},
                {"middle", button(frame.input.mouse_button(MouseButton::middle))},
                {"left_edges", pointer_edges(MouseButton::left)},
                {"right_edges", pointer_edges(MouseButton::right)},
                {"middle_edges", pointer_edges(MouseButton::middle)},
                {"focused", frame.input.focused},
                {"focus_lost", frame.input.focus_lost},
                {"escape_pressed", frame.input.key(Key::escape).pressed},
                {"scroll", {frame.input.scroll.x, frame.input.scroll.y}},
                {"blocked", blocked},
                {"iconified", frame.state.iconified},
                {"scene_instance", std::to_string(hierarchy.revision().scene_instance)},
                {"scene_revision", std::to_string(hierarchy.revision().revision)},
                {"selection", hierarchy.selection()
                                  ? nlohmann::json(hierarchy.selection()->id.value().to_string())
                                  : nlohmann::json(nullptr)},
                {"camera",
                 camera ? nlohmann::json(camera->id.value().to_string()) : nlohmann::json(nullptr)},
                {"content", {frame.state.content.width, frame.state.content.height}},
                {"framebuffer", {frame.state.framebuffer.width, frame.state.framebuffer.height}},
                {"imgui",
                 {{"cursor", {io.MousePos.x, io.MousePos.y}},
                  {"left_down", io.MouseDown[0]},
                  {"left_clicked", io.MouseClicked[0]},
                  {"left_released", io.MouseReleased[0]},
                  {"want_capture_mouse", io.WantCaptureMouse}}},
                {"viewport", nullptr}};
            trace_recording = true;
        } catch (...) {
            trace_recording = false;
        }
    }
    void trace_viewport(const ViewportRectangle& rectangle, bool hovered, bool blocked,
                        bool active_before, const GizmoFrameResult& result,
                        const ViewportPointerActions& actions,
                        std::optional<SceneSelection> selection_before) noexcept {
        if (!trace_recording)
            return;
        try {
            using Json = nlohmann::json;
            const auto& io = ImGui::GetIO();
            Json tips = Json::array();
            for (const auto& point : result.trace.tips)
                tips.push_back(point ? Json(*point) : Json(nullptr));
            Json receipt = nullptr;
            if (result.receipt)
                receipt = {
                    {"changed", result.receipt->changed},
                    {"scene_instance", std::to_string(result.receipt->revision.scene_instance)},
                    {"revision", std::to_string(result.receipt->revision.revision)},
                    {"refresh_failed", result.receipt->refresh_error.has_value()}};
            const auto selection = [](std::optional<SceneSelection> value) {
                return value ? Json(value->id.value().to_string()) : Json(nullptr);
            };
            trace_observation["viewport"] = {
                {"rectangle", {rectangle.x, rectangle.y, rectangle.width, rectangle.height}},
                {"hovered", hovered},
                {"blocked", blocked},
                {"text_input", io.WantTextInput},
                {"any_item_active", ImGui::IsAnyItemActive()},
                {"popup", ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)},
                {"project_current", session.state_current()},
                {"scene_current", hierarchy.current()},
                {"reconciliation", hierarchy.needs_reconciliation()},
                {"selection_before", selection(selection_before)},
                {"selection_after", selection(hierarchy.selection())},
                {"gizmo",
                 {{"reason", result.trace.reason},
                  {"consumed", result.consumed},
                  {"active_before", active_before},
                  {"active_after", gizmo.has_changes()},
                  {"hit", result.trace.hit ? Json(*result.trace.hit) : Json(nullptr)},
                  {"pivot", result.trace.pivot ? Json(*result.trace.pivot) : Json(nullptr)},
                  {"axis_tips", std::move(tips)},
                  {"operation", result.trace.operation
                                    ? Json(static_cast<int>(*result.trace.operation))
                                    : Json(nullptr)},
                  {"space", result.trace.space == tools::TransformSpace::local ? "local" : "world"},
                  {"receipt", std::move(receipt)},
                  {"error", result.error ? Json{{"code", error_code_name(result.error->code)},
                                                {"operation", result.error->context.operation}}
                                         : Json(nullptr)}}},
                {"pointer_pick", actions.pick},
                {"navigation",
                 {actions.motion.orbit_x, actions.motion.orbit_y, actions.motion.pan_x,
                  actions.motion.pan_y, actions.motion.dolly}}};
        } catch (...) {
            trace_recording = false;
        }
    }
    void finish_input_trace() noexcept {
        if (!trace_recording)
            return;
        try {
            trace_observation["image_drawn"] = image_drawn;
            trace_observation["gesture_active_after_frame"] = gizmo.has_changes();
            input_trace->record(++trace_frame, std::move(trace_observation));
        } catch (...) {
            trace_recording = false;
        }
    }
    void refresh() {
        const auto result = hierarchy.refresh();
        if (!result)
            error = result.error();
        else
            error.reset();
        synchronize();
    }
    void synchronize() {
        if (!gizmo.preview())
            restore_committed_view();
        if (camera && (!hierarchy.current() || camera->revision != hierarchy.revision())) {
            camera.reset();
            invalidate_view(true);
            view_error.reset();
            notice = "Scene changed. Select a camera again to refresh the view.";
        }
        if (!hierarchy.current())
            invalidate_view(true);
    }
    void invalidate_view(bool reset_navigation = false) {
        gizmo.cancel();
        committed_view.reset();
        shown_preview.reset();
        view.reset();
        snapshot.reset();
        pointer.cancel();
        if (reset_navigation)
            navigation.clear();
    }
    void restore_committed_view() noexcept {
        if (shown_preview && committed_view)
            view = std::move(committed_view);
        committed_view.reset();
        shown_preview.reset();
    }
    void cancel_gizmo() noexcept {
        gizmo.cancel();
        restore_committed_view();
    }
    Result<void> prepare_preview(const tools::AuthoringTransformPreview& preview) {
        if (!camera || !snapshot || !view || !hierarchy.current() ||
            snapshot->revision() != hierarchy.revision())
            return Error{ErrorCode::conflict,
                         "Refresh the scene view before manipulating",
                         {.operation = "editor.gizmo.preview"}};
        tools::AuthoringViewportRequest request{camera->id, camera->revision};
        request.viewport = {view->extent.width, view->extent.height};
        request.target = {target.data(), tools::AssetAudience::client};
        request.view_camera = navigation.view();
        request.transform_preview = preview;
        auto captured = session.viewport_snapshot(request);
        if (!captured)
            return std::move(captured).error();
        auto prepared = make_scene_viewport_request(captured.value(), vertex, pixel);
        if (!prepared)
            return std::move(prepared).error();
        if (!shown_preview)
            committed_view = std::move(view);
        view = std::move(prepared).value();
        shown_preview = preview;
        return {};
    }
    std::optional<HierarchyRow> selected_row() const {
        if (!hierarchy.selection())
            return std::nullopt;
        const auto id = hierarchy.selection()->id;
        const auto rows = hierarchy.rows();
        const auto found =
            std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.id == id; });
        return found == rows.end() ? std::nullopt : std::optional{*found};
    }
    void edited(Result<SceneEditReceipt> result) {
        if (!result) {
            error = result.error();
            notice.clear();
        } else {
            error = result.value().refresh_error;
            notice = result.value().changed ? "Scene updated" : "No change";
        }
        synchronize();
    }
    void parents(const HierarchyRow& row) {
        const auto selection = *hierarchy.selection();
        if (!edited_selection || edited_selection->id != selection.id ||
            edited_selection->revision != selection.revision) {
            parent_draft = row.parent;
            edited_selection = selection;
        }
        std::string label = "Scene root";
        for (const auto& parent : hierarchy.rows())
            if (parent_draft && parent.id == *parent_draft)
                label = parent.name;
        ImGui::TextWrapped("%s", label.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##parent", "Choose parent")) {
            if (ImGui::Selectable("Scene root", !parent_draft))
                parent_draft.reset();
            ImGuiListClipper clipper;
            const auto rows = hierarchy.rows();
            clipper.Begin(static_cast<int>(rows.size()));
            while (clipper.Step()) {
                for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                    const auto& parent = rows[static_cast<std::size_t>(index)];
                    if (parent.id == row.id) {
                        ImGui::Dummy({0, ImGui::GetTextLineHeight()});
                        continue;
                    }
                    const auto id = parent.id.value().to_string();
                    ImGui::PushID(id.c_str());
                    if (selectable_literal(parent.name, parent_draft == parent.id))
                        parent_draft = parent.id;
                    ImGui::PopID();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Checkbox("Keep world transform", &keep_world);
        if (ImGui::Button("Apply parent"))
            edited(
                hierarchy.reparent(selection.id, parent_draft,
                                   keep_world ? ReparentMode::keep_world : ReparentMode::keep_local,
                                   selection.revision));
    }
    void draw_hierarchy() {
        ImGui::TextUnformatted("HIERARCHY");
        if (ImGui::Button("Refresh"))
            refresh();
        if (hierarchy.needs_reconciliation()) {
            ImGui::TextWrapped("An edit has an uncertain result. Resolve it before continuing.");
            if (ImGui::Button("Resolve edit result"))
                edited(hierarchy.reconcile());
        }
        ImGui::BeginDisabled(!hierarchy.current() || hierarchy.needs_reconciliation());
        ImGui::BeginDisabled(physics.state().active || physics.state().uncertain);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##entity-name", create_name.data(), create_name.size());
        ImGui::Checkbox("Parent to selection", &child_of_selection);
        const bool missing_parent = child_of_selection && !hierarchy.selection();
        ImGui::BeginDisabled(missing_parent);
        if (ImGui::Button("Create entity")) {
            const auto parent = child_of_selection && hierarchy.selection()
                                    ? std::optional{hierarchy.selection()->id}
                                    : std::nullopt;
            edited(hierarchy.create(create_name.data(), parent, hierarchy.revision()));
        }
        ImGui::EndDisabled();
        if (missing_parent)
            ImGui::TextWrapped("Select a parent or clear Parent to selection.");
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::BeginDisabled(hierarchy.page_index() == 0);
        if (ImGui::SmallButton("Previous"))
            hierarchy.previous_page();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(hierarchy.page_index() + 1 >= hierarchy.page_count());
        if (ImGui::SmallButton("Next"))
            hierarchy.next_page();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu / %zu", hierarchy.page_count() ? hierarchy.page_index() + 1 : 0,
                            hierarchy.page_count());
        const float list_height = std::max(90.0F, ImGui::GetContentRegionAvail().y * .46F);
        ImGui::BeginChild("Hierarchy rows", {0, list_height}, ImGuiChildFlags_Borders);
        for (const auto& row : hierarchy.page()) {
            const auto identity = row.id.value().to_string();
            ImGui::PushID(identity.c_str());
            const float indent = 8.0F * static_cast<float>(std::min<std::size_t>(row.depth, 12));
            ImGui::Indent(indent);
            if (selectable_literal(row.name,
                                   hierarchy.selection() && hierarchy.selection()->id == row.id)) {
                const auto selected = hierarchy.select(row.id);
                if (!selected)
                    error = selected.error();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(identity.c_str());
                ImGui::Text("Depth %zu; children %zu", row.depth, row.child_count);
                if (row.parent)
                    ImGui::Text("Parent %s", row.parent->value().to_string().c_str());
                if (row.has_camera)
                    ImGui::TextUnformatted("Camera");
                if (row.has_mesh_renderer)
                    ImGui::TextUnformatted("Mesh renderer");
                ImGui::EndTooltip();
            }
            ImGui::Unindent(indent);
            ImGui::PopID();
        }
        if (hierarchy.rows().empty())
            ImGui::TextDisabled("The scene is empty");
        ImGui::EndChild();
        const auto selected = selected_row();
        if (selected && hierarchy.selection()) {
            const auto selection = *hierarchy.selection();
            ImGui::TextWrapped("Selected: %s", selected->name.c_str());
            ImGui::BeginDisabled(physics.state().active || physics.state().uncertain);
            if (selected->has_camera && ImGui::Button("View through camera")) {
                camera = selection;
                camera_name = selected->name;
                invalidate_view(true);
                refresh_view = true;
                notice.clear();
            }
            if (ImGui::Button("Delete subtree")) {
                deleting = selection;
                deleting_name = selected->name;
            }
            ImGui::TextUnformatted("Parent");
            parents(*selected);
            ImGui::EndDisabled();
        } else
            ImGui::TextDisabled("Select an entity");
        ImGui::EndDisabled();
        if (error)
            error_text(*error);
        if (!notice.empty())
            ImGui::TextWrapped("%s", notice.c_str());
    }
    void load_inventory() {
        std::vector<std::string_view> paths;
        std::string_view lines{sidecars.data()};
        while (!lines.empty()) {
            const auto end = lines.find('\n');
            auto line = lines.substr(0, end);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            if (!line.empty())
                paths.push_back(line);
            if (end == std::string_view::npos)
                break;
            lines.remove_prefix(end + 1);
        }
        const auto result = session.refresh_asset_inventory(paths);
        invalidate_view();
        if (!result)
            view_error = result.error();
        else {
            view_error.reset();
            refresh_view = true;
        }
    }
    void draw_viewport(const GraphicsWindowFrame& frame, bool blocked) {
        ImGui::TextUnformatted("SCENE VIEWPORT");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", camera ? camera_name.c_str() : "No camera selected");
        ImGui::BeginDisabled(physics.state().active);
        if (ImGui::CollapsingHeader("Viewport resources")) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("Import target", target.data(), target.size())) {
                invalidate_view();
                refresh_view = true;
            }
            ImGui::TextWrapped("For imported meshes, list their project-relative asset sidecars, "
                               "including dependencies. Existing imports must match the target.");
            ImGui::InputTextMultiline("##sidecars", sidecars.data(), sidecars.size(),
                                      {-1, ImGui::GetTextLineHeight() * 4});
            if (ImGui::Button("Load asset inventory"))
                load_inventory();
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!camera);
        if (ImGui::Button("Refresh view")) {
            refresh_view = true;
            invalidate_view();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset view")) {
            refresh_view = true;
            invalidate_view(true);
        }
        ImGui::EndDisabled();
        gizmo.controls(blocked || physics.state().active || !hierarchy.current() ||
                       hierarchy.needs_reconciliation());
        if (!gizmo.preview())
            restore_committed_view();
        if (view_error)
            error_text(*view_error);
        if (!camera) {
            ImGui::TextWrapped(
                "Select a Camera in the hierarchy, then choose View through camera.");
            return;
        }
        if (!frame.state.framebuffer.width || !frame.state.framebuffer.height ||
            frame.state.iconified) {
            pointer.cancel();
            cancel_gizmo();
            return;
        }
        ImGui::TextWrapped(
            "Right drag: orbit. Middle drag: pan. Wheel: zoom. Left click: select. "
            "Drag a colored handle to edit. Esc: cancel. Navigation never edits the camera.");
        const auto available = ImGui::GetContentRegionAvail();
        if (available.x < 1 || available.y < 1)
            return;
        const auto scale = ImGui::GetIO().DisplayFramebufferScale;
        if (!std::isfinite(scale.x) || !std::isfinite(scale.y) || scale.x <= 0 || scale.y <= 0) {
            pointer.cancel();
            cancel_gizmo();
            return;
        }
        const WindowExtent extent{static_cast<std::uint32_t>(
                                      std::clamp(std::floor(available.x * scale.x), 1.0F, 2048.0F)),
                                  static_cast<std::uint32_t>(std::clamp(
                                      std::floor(available.y * scale.y), 1.0F, 2048.0F))};
        if (view && view->extent != extent) {
            pointer.cancel();
            cancel_gizmo();
        }
        if (refresh_view || physics.state().active || (view && view->extent != extent)) {
            refresh_view = false;
            tools::AuthoringViewportRequest request{camera->id, camera->revision};
            request.viewport = {extent.width, extent.height};
            request.target = {target.data(), tools::AssetAudience::client};
            request.view_camera = navigation.view();
            if (physics.state().active) {
                request.physics_session = physics.state().session_id;
                request.physics_alpha = physics.state().alpha;
            }
            auto captured = session.viewport_snapshot(request);
            if (!captured) {
                invalidate_view();
                view_error = captured.error();
            } else {
                if (!navigation.view()) {
                    const auto observed = hierarchy.inspect(camera->id, camera->revision);
                    if (!observed || !observed.value().camera) {
                        invalidate_view(true);
                        view_error = observed ? Error{ErrorCode::conflict,
                                                      "Selected view camera is no longer available",
                                                      {.operation = "editor.viewport.seed"}}
                                              : observed.error();
                        return;
                    }
                    const auto seeded =
                        navigation.reset(captured.value(), *observed.value().camera);
                    if (!seeded) {
                        invalidate_view(true);
                        view_error = seeded.error();
                        return;
                    }
                }
                auto prepared = make_scene_viewport_request(captured.value(), vertex, pixel);
                if (!prepared) {
                    invalidate_view();
                    view_error = prepared.error();
                } else {
                    committed_view.reset();
                    shown_preview.reset();
                    view = std::move(prepared).value();
                    snapshot = std::move(captured).value();
                    view_error.reset();
                }
            }
        }
        if (view) {
            // Fixed engine-owned texture choice, never a native resource pointer.
            ImGui::Image(static_cast<ImTextureID>(2),
                         {static_cast<float>(view->extent.width) / scale.x,
                          static_cast<float>(view->extent.height) / scale.y});
            image_drawn = true;
            const auto minimum = ImGui::GetItemRectMin();
            const auto maximum = ImGui::GetItemRectMax();
            const ViewportRectangle rectangle{minimum.x, minimum.y, maximum.x - minimum.x,
                                              maximum.y - minimum.y};
            if (show_physics_debug && snapshot && physics.state().active &&
                !physics.state().uncertain && !physics.state().faulted) {
                auto bodies = physics.debug_bodies();
                if (!bodies) {
                    physics_overlay_error = bodies.error();
                } else {
                    auto lines = make_physics_debug_lines(bodies.value(), physics.debug_query());
                    if (!lines) {
                        physics_overlay_error = lines.error();
                    } else {
                        auto projected = project_physics_debug_lines(
                            lines.value(), snapshot->camera().view_projection,
                            snapshot->origin().scene_local_offset, rectangle);
                        if (!projected) {
                            physics_overlay_error = projected.error();
                        } else {
                            auto* draw = ImGui::GetWindowDrawList();
                            draw->PushClipRect(minimum, maximum, true);
                            constexpr std::size_t maximum_visible_lines = 2048;
                            physics_debug_truncated =
                                projected.value().size() > maximum_visible_lines;
                            std::size_t drawn = 0;
                            for (int pass = 0; pass < 2 && drawn < maximum_visible_lines; ++pass)
                                for (const auto& line : projected.value()) {
                                    const bool is_query = line.kind == PhysicsDebugKind::query ||
                                                          line.kind == PhysicsDebugKind::hit;
                                    if ((pass == 0) != is_query)
                                        continue;
                                    if (drawn == maximum_visible_lines)
                                        break;
                                    ImU32 color = IM_COL32(200, 200, 200, 255);
                                    switch (line.kind) {
                                    case PhysicsDebugKind::static_body:
                                        break;
                                    case PhysicsDebugKind::dynamic_body:
                                        color = IM_COL32(80, 230, 230, 255);
                                        break;
                                    case PhysicsDebugKind::kinematic_body:
                                        color = IM_COL32(110, 160, 255, 255);
                                        break;
                                    case PhysicsDebugKind::trigger:
                                        color = IM_COL32(255, 160, 80, 255);
                                        break;
                                    case PhysicsDebugKind::query:
                                        color = IM_COL32(255, 240, 100, 255);
                                        break;
                                    case PhysicsDebugKind::hit:
                                        color = IM_COL32(255, 100, 220, 255);
                                        break;
                                    }
                                    draw->AddLine(
                                        {static_cast<float>(line.x0), static_cast<float>(line.y0)},
                                        {static_cast<float>(line.x1), static_cast<float>(line.y1)},
                                        color, 1.5F);
                                    ++drawn;
                                }
                            draw->PopClipRect();
                            physics_overlay_error.reset();
                        }
                    }
                }
            }
            const bool input_blocked = blocked || deleting || !session.state_current() ||
                                       !hierarchy.current() || hierarchy.needs_reconciliation() ||
                                       ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive() ||
                                       ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
            GizmoFrameResult manipulated;
            const bool gesture_before = gizmo.has_changes();
            const auto selection_before = hierarchy.selection();
            const bool image_hovered = ImGui::IsItemHovered();
            if (snapshot && !physics.state().active) {
                manipulated = gizmo.overlay(frame.input, rectangle, *snapshot,
                                            ImGui::IsItemHovered(), input_blocked,
                                            [&](const tools::AuthoringTransformPreview& preview) {
                                                return prepare_preview(preview);
                                            });
                // The widget retains its diagnostic in a fixed-height status
                // area; repeating it above the image would change drag geometry.
                if (manipulated.receipt) {
                    const auto& receipt = *manipulated.receipt;
                    error = receipt.refresh_error;
                    notice = receipt.changed ? "Transform committed; one undo restores the drag."
                                             : "No transform change";
                    // A known set_transform keeps the camera's identity alive.
                    // Preserve navigation and explicitly rebind only this known
                    // same-incarnation commit; unrelated edits still invalidate it.
                    if (camera && !receipt.refresh_error && hierarchy.current() &&
                        camera->revision.scene_instance == receipt.revision.scene_instance)
                        camera->revision = receipt.revision;
                    // The validated preview is this commit's geometry. Retain it
                    // for the already emitted image; capture its new revision
                    // before next frame's input. No stale snapshot is picked.
                    committed_view.reset();
                    shown_preview.reset();
                    refresh_view = true;
                } else if (!gizmo.preview())
                    restore_committed_view();
            }
            const auto actions = pointer.route(frame.input, rectangle, ImGui::IsItemHovered(),
                                               input_blocked || manipulated.consumed);
            if (actions.pick && snapshot && !physics.state().active) {
                const auto hit =
                    pick_viewport(*snapshot, hierarchy.revision(), rectangle, actions.x, actions.y);
                if (!hit)
                    view_error = hit.error();
                else if (hit.value()) {
                    const auto selected = hierarchy.select(hit.value()->entity);
                    if (!selected)
                        error = selected.error();
                    else {
                        error.reset();
                        notice = inspector_widget.has_changes()
                                     ? "Selection changed; unapplied inspector draft is retained."
                                     : "Viewport selection updated. Inspect selection to edit.";
                    }
                } else {
                    hierarchy.clear_selection();
                    notice = inspector_widget.has_changes()
                                 ? "Selection cleared; unapplied inspector draft is retained."
                                 : "No visible mesh at this point.";
                }
            }
            if (!input_blocked && !manipulated.consumed && snapshot) {
                auto moved = navigation.navigate(actions.motion, snapshot->viewport(), rectangle);
                if (!moved) {
                    pointer.cancel();
                    view_error = moved.error();
                } else if (moved.value()) {
                    // Retain the exact snapshot used above and in this frame's
                    // image. The new camera is extracted before the next image.
                    refresh_view = true;
                    view_error.reset();
                }
            }
            trace_viewport(rectangle, image_hovered, input_blocked, gesture_before, manipulated,
                           actions, selection_before);
        }
    }
    void confirmation() {
        if (!deleting)
            return;
        ImGui::OpenPopup("Delete subtree?");
        if (ImGui::BeginPopupModal("Delete subtree?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Delete %s and all its descendants?", deleting_name.c_str());
            if (ImGui::Button("Delete")) {
                const auto result = hierarchy.erase(deleting->id, deleting->revision);
                edited(result);
                if (result)
                    deleting.reset();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                deleting.reset();
            if (error)
                error_text(*error);
            if (!deleting)
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    void draw_script(bool blocked) {
        ImGui::TextUnformatted("SCRIPT SOURCE");
        ImGui::TextWrapped("Source is written separately from the scene and its undo history. "
                           "The scripts directory must already exist.");
        ImGui::BeginDisabled(blocked);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##script-path", script_path.data(), script_path.size());
        ImGui::BeginDisabled(script.has_changes());
        if (ImGui::Button("Open script")) {
            const auto opened = script.open(script_path.data());
            if (opened) {
                text_buffer(script_text, script.text());
                script_error.reset();
            } else
                script_error = opened.error();
        }
        ImGui::SameLine();
        if (ImGui::Button("New script")) {
            const auto created = script.create(script_path.data());
            if (created) {
                text_buffer(script_text, script.text());
                script_error.reset();
            } else
                script_error = created.error();
        }
        ImGui::EndDisabled();
        if (script.loaded()) {
            ImGui::TextWrapped("Editing: %s%s", script.path().c_str(),
                               script.has_changes() ? " (draft)" : "");
            ImGui::BeginDisabled(script.uncertain());
            if (ImGui::InputTextMultiline("##script-source", script_text.data(), script_text.size(),
                                          {-1, ImGui::GetTextLineHeightWithSpacing() * 14})) {
                const auto edited = script.edit(script_text.data());
                if (!edited)
                    script_error = edited.error();
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!script.has_changes() || script.uncertain() ||
                                 physics.state().active || physics.state().uncertain);
            if (ImGui::Button("Write script")) {
                const auto written = script.write();
                if (!written)
                    script_error = written.error();
                else
                    script_error.reset();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Discard script draft")) {
                const auto discarded = script.discard();
                if (!discarded)
                    script_error = discarded.error();
                else {
                    script_text.fill('\0');
                    script_error.reset();
                }
            }
            if (script.uncertain()) {
                ImGui::SameLine();
                if (ImGui::Button("Reconcile script write")) {
                    const auto observed = script.reconcile();
                    if (!observed)
                        script_error = observed.error();
                    else
                        script_error.reset();
                }
            }
            if (physics.state().active)
                ImGui::TextDisabled("Stop preview before writing source.");
            if (!script.notice().empty())
                ImGui::TextWrapped("%s", script.notice().c_str());
        }
        ImGui::EndDisabled();
        if (script_error)
            error_text(*script_error);
    }
    void sync_prefab_text() {
        std::fill(prefab_text.begin(), prefab_text.end(), '\0');
        if (prefab.text().size() < prefab_text.size())
            std::copy(prefab.text().begin(), prefab.text().end(), prefab_text.begin());
    }
    void prefab_edited(Result<SceneEditReceipt> outcome) {
        if (!outcome) {
            prefab_error = outcome.error();
            return;
        }
        prefab_error = outcome.value().refresh_error;
        const auto refreshed = hierarchy.refresh();
        if (!refreshed)
            prefab_error = refreshed.error();
        else if (outcome.value().entity && hierarchy.revision() == outcome.value().revision) {
            const auto selected = hierarchy.select(*outcome.value().entity);
            if (!selected)
                prefab_error = selected.error();
        }
        synchronize();
    }
    void draw_prefab(bool blocked) {
        ImGui::TextUnformatted("PREFABS");
        ImGui::TextWrapped(
            "Write the matching asset sidecar in Assets first. Prefab source and "
            "sidecar are separate publications; scene Save and undo do not write them.");
        ImGui::PushID("prefab-panel");
        ImGui::BeginDisabled(blocked);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("Source path", prefab_path.data(), prefab_path.size());
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("Asset ID for capture", prefab_asset.data(), prefab_asset.size());
        const bool playing = physics.state().active || physics.state().uncertain;
        ImGui::BeginDisabled(playing || !session.state_current() || prefab.has_changes());
        if (ImGui::Button("Open prefab")) {
            const auto opened = prefab.open(prefab_path.data());
            if (!opened)
                prefab_error = opened.error();
            else {
                sync_prefab_text();
                prefab_error.reset();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("New asset ID")) {
            const auto generated = generate_stable_id();
            if (!generated)
                prefab_error = generated.error();
            else {
                text_buffer(prefab_asset, generated.value().to_string());
                prefab_error.reset();
            }
        }
        ImGui::BeginDisabled(!hierarchy.current() || !hierarchy.selection());
        if (ImGui::Button("Capture selected subtree")) {
            const auto parsed = StableId::parse(prefab_asset.data());
            if (!parsed)
                prefab_error = parsed.error();
            else {
                const auto selection = *hierarchy.selection();
                const auto captured = prefab.capture(prefab_path.data(), selection.id,
                                                     AssetId{parsed.value()}, selection.revision);
                if (!captured)
                    prefab_error = captured.error();
                else {
                    sync_prefab_text();
                    prefab_error.reset();
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (prefab.loaded()) {
            ImGui::TextWrapped("Source: %s%s", prefab.path().c_str(),
                               prefab.has_changes() ? " (draft)" : "");
            const bool editable = prefab.text().size() < prefab_text.size();
            ImGui::BeginDisabled(prefab.uncertain() || prefab.instantiation_uncertain() ||
                                 !editable);
            if (editable &&
                ImGui::InputTextMultiline("##prefab-source", prefab_text.data(), prefab_text.size(),
                                          {-1, ImGui::GetTextLineHeightWithSpacing() * 14})) {
                const auto edited = prefab.edit(prefab_text.data());
                if (!edited)
                    prefab_error = edited.error();
                else
                    prefab_error.reset();
            }
            ImGui::EndDisabled();
            if (!editable)
                ImGui::TextWrapped("This source is too large for the Editor text field. Its full "
                                   "contents are retained; use the typed project-file workflow.");
            ImGui::BeginDisabled(playing || !session.state_current() || !prefab.has_changes() ||
                                 prefab.uncertain() || prefab.instantiation_uncertain() ||
                                 prefab.prepared());
            if (ImGui::Button("Write prefab source")) {
                const auto written = prefab.write();
                if (!written)
                    prefab_error = written.error();
                else {
                    sync_prefab_text();
                    prefab_error.reset();
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(playing || !session.state_current() || prefab.has_changes());
            if (ImGui::Button("Prepare instance")) {
                const auto prepared = prefab.prepare();
                if (!prepared)
                    prefab_error = prepared.error();
                else
                    prefab_error.reset();
            }
            ImGui::EndDisabled();
            if (prefab.prepared_root())
                ImGui::TextWrapped("Prepared root: %s",
                                   prefab.prepared_root()->value().to_string().c_str());
            ImGui::BeginDisabled(playing || !session.state_current() || !prefab.prepared() ||
                                 prefab.instantiation_uncertain());
            if (ImGui::Button("Instantiate prefab"))
                prefab_edited(prefab.instantiate());
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!prefab.prepared() || prefab.instantiation_uncertain());
            if (ImGui::Button("Release instance draft")) {
                const auto released = prefab.release_preparation();
                if (!released)
                    prefab_error = released.error();
                else
                    prefab_error.reset();
            }
            ImGui::EndDisabled();
            if (prefab.uncertain()) {
                if (ImGui::Button("Reconcile prefab write")) {
                    const auto observed = prefab.reconcile();
                    if (!observed)
                        prefab_error = observed.error();
                    else {
                        sync_prefab_text();
                        prefab_error.reset();
                    }
                }
            }
            if (prefab.instantiation_uncertain()) {
                if (ImGui::Button("Reconcile original instance"))
                    prefab_edited(prefab.reconcile_instantiation());
                ImGui::TextWrapped("The original instance outcome is unresolved. Reconcile it "
                                   "before preparing another intent or discarding this draft.");
            }
            ImGui::BeginDisabled(prefab.instantiation_uncertain());
            if (ImGui::Button("Discard prefab draft")) {
                const auto discarded = prefab.discard();
                if (!discarded)
                    prefab_error = discarded.error();
                else {
                    sync_prefab_text();
                    prefab_error.reset();
                }
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled("The complete authoring request must fit 64 KiB.");
            if (playing)
                ImGui::TextDisabled("Stop preview before publishing or instantiating prefabs.");
            if (!prefab.notice().empty())
                ImGui::TextWrapped("%s", prefab.notice().c_str());
        }
        ImGui::EndDisabled();
        ImGui::PopID();
        if (prefab_error)
            error_text(*prefab_error);
    }
    void draw_physics(const GraphicsWindowFrame& frame, bool blocked) {
        const auto now = clock.now();
        const bool paused_by_window = blocked || deleting || !frame.state.focused ||
                                      !frame.input.focused || frame.input.focus_lost ||
                                      frame.state.iconified || !session.state_current() ||
                                      ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        const bool was_active = physics.state().active;
        const auto polled = physics.poll(now, paused_by_window);
        if (!polled)
            physics_error = polled.error();
        if (was_active != physics.state().active) {
            invalidate_view();
            refresh_view = true;
        }
        const auto operation = [&](Result<void> result, bool changed_view) {
            if (!result)
                physics_error = result.error();
            else
                physics_error.reset();
            if (changed_view) {
                invalidate_view();
                refresh_view = true;
            }
        };
        ImGui::TextUnformatted("PHYSICS PREVIEW");
        ImGui::SameLine();
        ImGui::TextDisabled("Frozen runtime copy; source and history stay unchanged");
        ImGui::BeginDisabled(blocked);
        ImGui::BeginDisabled(physics.state().active || physics.state().uncertain || !camera ||
                             !session.state_current() || inspector_widget.has_changes() ||
                             asset_widget.has_changes() || script.has_changes() ||
                             prefab.has_changes() || gizmo.has_changes() ||
                             hierarchy.needs_reconciliation());
        if (ImGui::Button("Play physics")) {
            cancel_gizmo();
            operation(physics.start(camera->id, camera->revision, target.data(), now), true);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!physics.state().active || physics.state().uncertain ||
                             physics.state().faulted);
        if (ImGui::Button(physics.state().running ? "Pause physics" : "Resume physics"))
            physics.pause(physics.state().running);
        ImGui::SameLine();
        ImGui::BeginDisabled(physics.state().running);
        if (ImGui::Button("Step physics"))
            operation(physics.step(now), false);
        ImGui::SameLine();
        ImGui::BeginDisabled(!hierarchy.selection());
        if (ImGui::Button("Remove selected runtime body"))
            operation(physics.remove(hierarchy.selection()->id), false);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!physics.state().active);
        if (ImGui::Button("Stop physics"))
            operation(physics.stop(), true);
        ImGui::EndDisabled();
        if (physics.state().uncertain) {
            ImGui::SameLine();
            if (ImGui::Button("Reconcile physics result"))
                operation(physics.reconcile(now), true);
        }
        ImGui::EndDisabled();
        if (physics.state().active)
            ImGui::Text("Tick %llu; last operation events %zu%s",
                        static_cast<unsigned long long>(physics.state().tick),
                        physics.state().events, paused_by_window ? "; window/modal paused" : "");
        if (ImGui::CollapsingHeader("Physics inspection")) {
            ImGui::Checkbox("Show physics debug overlay", &show_physics_debug);
            ImGui::TextDisabled("Committed collider poses; viewport rendering may be interpolated");
            ImGui::TextDisabled(
                "Screen-space approximate wireframes; not depth-tested or selectable");
            ImGui::TextDisabled("Static: gray, dynamic: cyan, kinematic: blue, trigger: orange; "
                                "query: yellow, hit: pink");
            if (show_physics_debug && physics_debug_truncated)
                ImGui::TextWrapped("Overlay limited to 2048 visible lines; some colliders or query "
                                   "lines are omitted.");
            if (ImGui::TreeNode("Runtime bodies")) {
                if (!physics.state().active) {
                    ImGui::TextDisabled("Start a physics preview to inspect runtime bodies.");
                } else {
                    auto bodies = physics.debug_bodies();
                    if (!bodies) {
                        error_text(bodies.error());
                    } else {
                        ImGui::Text("Bodies: %zu", bodies.value().size());
                        ImGui::BeginChild("Runtime body details",
                                          {0, ImGui::GetTextLineHeightWithSpacing() * 5},
                                          ImGuiChildFlags_Borders);
                        for (const auto& body : physics.body_details())
                            ImGui::TextWrapped("%s", body.c_str());
                        ImGui::EndChild();
                    }
                }
                ImGui::TreePop();
            }
            if (ImGui::Combo("Query shape", &physics_query_kind,
                             "Ray\0Sphere overlap\0Box overlap\0"))
                physics_query_second = physics_query_kind == 2 ? std::array<double, 3>{1, 1, 1}
                                                               : std::array<double, 3>{0, 0, -10};
            ImGui::InputScalarN(physics_query_kind == 0 ? "Ray start" : "Center",
                                ImGuiDataType_Double, physics_query_first.data(), 3);
            if (physics_query_kind == 1)
                ImGui::InputDouble("Radius", &physics_query_radius);
            else
                ImGui::InputScalarN(physics_query_kind == 0 ? "Ray end" : "Half extents",
                                    ImGuiDataType_Double, physics_query_second.data(), 3);
            ImGui::InputInt("Query layer (0-15)", &physics_query_layer);
            ImGui::InputInt("Query mask (0-65535)", &physics_query_mask);
            ImGui::InputInt("Maximum query hits (1-128)", &physics_query_max_hits);
            ImGui::Checkbox("Include triggers", &physics_query_triggers);
            ImGui::Checkbox("Ignore selected runtime body", &physics_query_ignore_selected);
            ImGui::BeginDisabled(!physics.state().active || physics.state().running ||
                                 physics.state().uncertain || physics.state().faulted || blocked);
            if (ImGui::Button("Inspect physics query")) {
                if (physics_query_layer < 0 || physics_query_layer > 15 || physics_query_mask < 0 ||
                    physics_query_mask > 65535 || physics_query_max_hits < 1 ||
                    physics_query_max_hits > 128 ||
                    (physics_query_ignore_selected && !hierarchy.selection())) {
                    physics.clear_query();
                    physics_debug_error =
                        Error{ErrorCode::invalid_argument,
                              "Check the query filter, hit limit and selected body",
                              {.operation = "editor.physics.query"}};
                } else {
                    PhysicsDebugQuery query{};
                    query.kind = static_cast<PhysicsDebugQueryKind>(physics_query_kind);
                    query.first = {physics_query_first[0], physics_query_first[1],
                                   physics_query_first[2]};
                    query.second = {physics_query_second[0], physics_query_second[1],
                                    physics_query_second[2]};
                    query.radius = physics_query_radius;
                    const auto ignore = physics_query_ignore_selected
                                            ? std::optional{hierarchy.selection()->id}
                                            : std::optional<AuthoredEntityId>{};
                    auto result = physics.inspect_query(
                        std::move(query), static_cast<std::uint32_t>(physics_query_layer),
                        static_cast<std::uint32_t>(physics_query_mask), physics_query_triggers,
                        ignore, static_cast<std::size_t>(physics_query_max_hits));
                    if (!result)
                        physics_debug_error = result.error();
                    else
                        physics_debug_error.reset();
                }
            }
            ImGui::EndDisabled();
            if (physics.debug_query()) {
                ImGui::Text("Last successful query hits: %zu", physics.query_details().size());
                for (const auto& hit : physics.query_details())
                    ImGui::TextWrapped("%s", hit.c_str());
            }
            if (physics_debug_error)
                error_text(*physics_debug_error);
            if (physics_overlay_error && show_physics_debug)
                error_text(*physics_overlay_error);
        }
        if (!physics.contact_details().empty() &&
            ImGui::CollapsingHeader("Last contact transitions")) {
            ImGui::TextDisabled("Last nonempty operation; retained after stop");
            ImGui::BeginChild("Physics contacts", {0, ImGui::GetTextLineHeightWithSpacing() * 4},
                              ImGuiChildFlags_Borders);
            for (const auto& contact : physics.contact_details())
                ImGui::TextWrapped("%s", contact.c_str());
            ImGui::EndChild();
        }
        if (!physics.script_details().empty() && ImGui::CollapsingHeader("Script output")) {
            ImGui::TextDisabled("Bounded preview output; retained after stop");
            ImGui::BeginChild("Script output details",
                              {0, ImGui::GetTextLineHeightWithSpacing() * 5},
                              ImGuiChildFlags_Borders);
            for (const auto& message : physics.script_details())
                ImGui::TextWrapped("%s", message.c_str());
            ImGui::EndChild();
        }
        if (physics_error)
            error_text(*physics_error);
        ImGui::Separator();
    }
};

WorkspacePanel::WorkspacePanel(ProjectSession& session, const ShaderArtifact& vertex,
                               const ShaderArtifact& pixel, EditorInputTrace* input_trace)
    : impl_(std::make_unique<Impl>(session, vertex, pixel, input_trace)) {}
WorkspacePanel::~WorkspacePanel() = default;

void WorkspacePanel::draw(const GraphicsWindowFrame& frame, bool blocked) {
    auto& value = *impl_;
    value.image_drawn = false;
    value.synchronize();
    value.begin_input_trace(frame, blocked);
    value.draw_physics(frame, blocked);
    // Resolve last frame's confirmation before emitting this frame's scene image.
    if (!blocked && value.session.state_current())
        value.confirmation();
    if (ImGui::BeginTabBar("Workspace panels")) {
        if (ImGui::BeginTabItem("Scene")) {
            ImGui::BeginDisabled(blocked || !value.session.state_current());
            const float width = ImGui::GetContentRegionAvail().x;
            const float left = std::clamp(width * .25F, 230.0F, 350.0F);
            ImGui::BeginChild("Hierarchy", {left, 0}, ImGuiChildFlags_Borders);
            value.draw_hierarchy();
            ImGui::EndChild();
            ImGui::SameLine();
            const float inspector_width = std::clamp(width * .29F, 280.0F, 410.0F);
            ImGui::BeginChild("Inspector", {inspector_width, 0}, ImGuiChildFlags_Borders);
            if (value.inspector_widget.draw(blocked || value.physics.state().active ||
                                            value.physics.state().uncertain))
                value.synchronize();
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("Viewport", {0, 0}, ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            value.draw_viewport(frame, blocked);
            ImGui::EndChild();
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Assets")) {
            ImGui::BeginChild("Asset workspace", {0, 0}, ImGuiChildFlags_Borders);
            if (value.asset_widget.draw(blocked || value.physics.state().active ||
                                        value.physics.state().uncertain)) {
                value.invalidate_view();
                value.refresh_view = true;
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Scripts")) {
            value.draw_script(blocked);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Prefabs")) {
            value.draw_prefab(blocked);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Console")) {
            value.console.draw(value.session);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!value.image_drawn) {
        value.pointer.cancel();
        value.cancel_gizmo();
    }
    value.finish_input_trace();
}
void WorkspacePanel::cancel_confirmation() noexcept {
    impl_->deleting.reset();
    impl_->pointer.cancel();
    impl_->cancel_gizmo();
}
bool WorkspacePanel::has_unapplied_drafts() const noexcept {
    return impl_->gizmo.has_changes() || impl_->inspector_widget.has_changes() ||
           impl_->asset_widget.has_changes() || impl_->script.has_changes() ||
           impl_->prefab.has_changes();
}
Result<void> WorkspacePanel::discard_drafts() {
    impl_->cancel_gizmo();
    const auto prefab = impl_->prefab.discard();
    if (!prefab)
        return prefab;
    auto result = impl_->asset_widget.discard_draft();
    if (!result)
        return result;
    impl_->inspector_widget.discard_draft();
    return impl_->script.discard();
}
const client::SceneViewportRequest* WorkspacePanel::viewport() const noexcept {
    return impl_->image_drawn && impl_->view ? &*impl_->view : nullptr;
}

} // namespace shark::editor
