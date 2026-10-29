#include "workspace_panel.hpp"
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

struct WorkspacePanel::Impl {
    ProjectSession& session;
    ScenePanel hierarchy;
    InspectorPanel inspector;
    InspectorWidget inspector_widget;
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
    std::optional<Error> error;
    std::optional<Error> view_error;
    std::string notice;

    Impl(ProjectSession& owner, const ShaderArtifact& vs, const ShaderArtifact& ps)
        : session(owner), hierarchy(owner), inspector(hierarchy),
          inspector_widget(inspector, hierarchy), vertex(vs), pixel(ps) {
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
            view.reset();
            view_error.reset();
            notice = "Scene changed. Select a camera again to refresh the view.";
        }
        if (!hierarchy.current())
            view.reset();
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
        view.reset();
        if (!result)
            view_error = result.error();
        else {
            view_error.reset();
            refresh_view = true;
        }
    }
    void draw_viewport(const GraphicsWindowFrame& frame) {
        ImGui::TextUnformatted("SCENE VIEWPORT");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", camera ? camera_name.c_str() : "No camera selected");
        if (ImGui::CollapsingHeader("Viewport resources")) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("Import target", target.data(), target.size())) {
                view.reset();
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
            view.reset();
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
            frame.state.iconified)
            return;
        const auto available = ImGui::GetContentRegionAvail();
        if (available.x < 1 || available.y < 1)
            return;
        const auto scale = ImGui::GetIO().DisplayFramebufferScale;
        const WindowExtent extent{static_cast<std::uint32_t>(
                                      std::clamp(std::floor(available.x * scale.x), 1.0F, 2048.0F)),
                                  static_cast<std::uint32_t>(std::clamp(
                                      std::floor(available.y * scale.y), 1.0F, 2048.0F))};
        if (refresh_view || (view && view->extent != extent)) {
            refresh_view = false;
            tools::AuthoringViewportRequest request{camera->id, camera->revision};
            request.viewport = {extent.width, extent.height};
            request.target = {target.data(), tools::AssetAudience::client};
            auto snapshot = session.viewport_snapshot(request);
            if (!snapshot) {
                view.reset();
                view_error = snapshot.error();
            } else {
                auto prepared = make_scene_viewport_request(snapshot.value(), vertex, pixel);
                if (!prepared) {
                    view.reset();
                    view_error = prepared.error();
                } else {
                    view = std::move(prepared).value();
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
    if (!blocked)
        value.confirmation();
    ImGui::BeginDisabled(blocked);
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
    ImGui::BeginChild("Viewport", {0, 0}, ImGuiChildFlags_Borders);
    value.draw_viewport(frame);
    ImGui::EndChild();
    ImGui::EndDisabled();
}
void WorkspacePanel::cancel_confirmation() noexcept {
    impl_->deleting.reset();
}
bool WorkspacePanel::has_inspector_draft() const noexcept {
    return impl_->inspector_widget.has_changes();
}
void WorkspacePanel::discard_inspector_draft() noexcept {
    impl_->inspector_widget.discard_draft();
}
const client::SceneViewportRequest* WorkspacePanel::viewport() const noexcept {
    return impl_->image_drawn && impl_->view ? &*impl_->view : nullptr;
}

} // namespace shark::editor
