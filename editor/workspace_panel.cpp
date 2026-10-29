#include "workspace_panel.hpp"
#include "asset_widget.hpp"
#include "console_widget.hpp"
#include "inspector_widget.hpp"
#include "scene_panel.hpp"

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
    if (!left.held && !left.pressed && right.pressed && right.held && !middle.held)
        drag_ = Drag::orbit;
    else if (!left.held && !left.pressed && middle.pressed && middle.held && !right.held)
        drag_ = Drag::pan;
    const bool beginning =
        (drag_ == Drag::orbit && right.pressed) || (drag_ == Drag::pan && middle.pressed);
    if ((drag_ == Drag::orbit && !right.held) || (drag_ == Drag::pan && !middle.held))
        drag_ = Drag::none;
    if (drag_ != Drag::none && !beginning) {
        const auto dx = cursor.x - previous_x_;
        const auto dy = cursor.y - previous_y_;
        if (drag_ == Drag::orbit) {
            result.motion.orbit_x = dx;
            result.motion.orbit_y = dy;
        } else {
            result.motion.pan_x = dx;
            result.motion.pan_y = dy;
        }
    }
    previous_x_ = cursor.x;
    previous_y_ = cursor.y;
    if (drag_ == Drag::none && !right.held && !middle.held && !right.pressed && !middle.pressed) {
        result.pick = left.pressed;
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
    ScenePanel hierarchy;
    InspectorPanel inspector;
    InspectorWidget inspector_widget;
    AssetPanel assets;
    AssetWidget asset_widget;
    ConsoleWidget console;
    ShaderArtifact vertex;
    ShaderArtifact pixel;
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
    std::optional<RenderScene> snapshot;
    ViewportNavigation navigation;
    ViewportPointerRouter pointer;
    std::optional<Error> error;
    std::optional<Error> view_error;
    std::string notice;

    Impl(ProjectSession& owner, const ShaderArtifact& vs, const ShaderArtifact& ps)
        : session(owner), hierarchy(owner), inspector(hierarchy),
          inspector_widget(inspector, hierarchy), assets(owner), asset_widget(assets, owner),
          vertex(vs), pixel(ps) {
        text_buffer(create_name, "Entity");
        text_buffer(target, "windows-client");
        refresh();
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
        view.reset();
        snapshot.reset();
        pointer.cancel();
        if (reset_navigation)
            navigation.clear();
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
            return;
        }
        ImGui::TextWrapped("Right drag: orbit. Middle drag: pan. Wheel: zoom. Left click: select. "
                           "Esc: stop navigation. Scene cameras are unchanged.");
        const auto available = ImGui::GetContentRegionAvail();
        if (available.x < 1 || available.y < 1)
            return;
        const auto scale = ImGui::GetIO().DisplayFramebufferScale;
        if (!std::isfinite(scale.x) || !std::isfinite(scale.y) || scale.x <= 0 || scale.y <= 0) {
            pointer.cancel();
            return;
        }
        const WindowExtent extent{static_cast<std::uint32_t>(
                                      std::clamp(std::floor(available.x * scale.x), 1.0F, 2048.0F)),
                                  static_cast<std::uint32_t>(std::clamp(
                                      std::floor(available.y * scale.y), 1.0F, 2048.0F))};
        if (view && view->extent != extent)
            pointer.cancel();
        if (refresh_view || (view && view->extent != extent)) {
            refresh_view = false;
            tools::AuthoringViewportRequest request{camera->id, camera->revision};
            request.viewport = {extent.width, extent.height};
            request.target = {target.data(), tools::AssetAudience::client};
            request.view_camera = navigation.view();
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
            const bool input_blocked = blocked || deleting || !session.state_current() ||
                                       !hierarchy.current() || hierarchy.needs_reconciliation() ||
                                       ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive() ||
                                       ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
            const auto actions =
                pointer.route(frame.input, rectangle, ImGui::IsItemHovered(), input_blocked);
            if (actions.pick && snapshot) {
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
            if (!input_blocked && snapshot) {
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
};

WorkspacePanel::WorkspacePanel(ProjectSession& session, const ShaderArtifact& vertex,
                               const ShaderArtifact& pixel)
    : impl_(std::make_unique<Impl>(session, vertex, pixel)) {}
WorkspacePanel::~WorkspacePanel() = default;

void WorkspacePanel::draw(const GraphicsWindowFrame& frame, bool blocked) {
    auto& value = *impl_;
    value.image_drawn = false;
    value.synchronize();
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
            if (value.inspector_widget.draw(blocked))
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
            if (value.asset_widget.draw(blocked)) {
                value.invalidate_view();
                value.refresh_view = true;
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Console")) {
            value.console.draw(value.session);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!value.image_drawn)
        value.pointer.cancel();
}
void WorkspacePanel::cancel_confirmation() noexcept {
    impl_->deleting.reset();
    impl_->pointer.cancel();
}
bool WorkspacePanel::has_unapplied_drafts() const noexcept {
    return impl_->inspector_widget.has_changes() || impl_->asset_widget.has_changes();
}
Result<void> WorkspacePanel::discard_drafts() {
    auto result = impl_->asset_widget.discard_draft();
    if (!result)
        return result;
    impl_->inspector_widget.discard_draft();
    return {};
}
const client::SceneViewportRequest* WorkspacePanel::viewport() const noexcept {
    return impl_->image_drawn && impl_->view ? &*impl_->view : nullptr;
}

} // namespace shark::editor
