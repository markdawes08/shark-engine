#include "inspector_widget.hpp"

#include <algorithm>
#include <array>
#include <imgui.h>
#include <string>

namespace shark::editor {
namespace {
template <std::size_t N> void copy_text(std::array<char, N>& buffer, std::string_view text) {
    const auto count = std::min(text.size(), N - 1);
    std::copy_n(text.data(), count, buffer.data());
    buffer[count] = '\0';
}
void show_error(const Error& error) {
    ImGui::TextColored({1, .55F, .43F, 1}, "%s", error.message.c_str());
    const auto code = error_code_name(error.code);
    ImGui::TextDisabled("%.*s", static_cast<int>(code.size()), code.data());
    if (!error.context.path.empty())
        ImGui::TextWrapped("%s", error.context.path.c_str());
    if (!error.context.entity_id.empty())
        ImGui::TextWrapped("Entity: %s", error.context.entity_id.c_str());
}
void field_label(const PropertySchema& property) {
    ImGui::TextUnformatted(property.display_name.c_str());
    if (property.unit == PropertyUnit::meters) {
        ImGui::SameLine();
        ImGui::TextDisabled("(meters)");
    } else if (property.unit == PropertyUnit::radians) {
        ImGui::SameLine();
        ImGui::TextDisabled("(radians)");
    }
    ImGui::SetNextItemWidth(-1);
}
bool vector_field(math::Vec3& value) {
    std::array<double, 3> input{value.x, value.y, value.z};
    if (!ImGui::InputScalarN("##value", ImGuiDataType_Double, input.data(), 3, nullptr, nullptr,
                             "%.9g"))
        return false;
    value = {input[0], input[1], input[2]};
    return true;
}
} // namespace

struct InspectorWidget::Impl {
    InspectorPanel& panel;
    ScenePanel& scene;
    const std::array<ComponentSchema, 4> schemas{component_schemas()};
    std::optional<tools::AuthoringHistoryStatus> history;
    std::optional<SceneRevision> history_observation;
    std::optional<Error> history_error;
    std::optional<Error> error;
    std::string notice;
    std::optional<SceneSelection> loaded;
    std::array<char, maximum_name_bytes + 1> name{};
    std::array<char, 37> asset{};
    std::array<char, 37> baseline_asset{};
    std::optional<MeshRenderer> original_mesh;
    bool mesh_present{false};
    std::uint32_t mesh_index{0};
    bool mesh_visible{true};

    Impl(InspectorPanel& inspector, ScenePanel& owner) : panel(inspector), scene(owner) {}

    bool mesh_changed() const noexcept {
        if (!loaded)
            return false;
        if (mesh_present != original_mesh.has_value())
            return true;
        if (!mesh_present)
            return false;
        return !original_mesh || std::string_view(asset.data()) != baseline_asset.data() ||
               mesh_index != original_mesh->mesh_index || mesh_visible != original_mesh->visible;
    }
    bool changed() const noexcept {
        return panel.dirty() || mesh_changed();
    }
    void load_buffers() {
        if (!panel.draft()) {
            loaded.reset();
            original_mesh.reset();
            mesh_present = false;
            name.fill('\0');
            asset.fill('\0');
            baseline_asset.fill('\0');
            return;
        }
        const auto& draft = *panel.draft();
        loaded = draft.selection;
        copy_text(name, draft.name.value);
        original_mesh = draft.mesh_renderer;
        mesh_present = original_mesh.has_value();
        copy_text(asset, original_mesh ? original_mesh->asset.value().to_string() : "");
        baseline_asset = asset;
        mesh_index = original_mesh ? original_mesh->mesh_index : 0;
        mesh_visible = original_mesh ? original_mesh->visible : true;
    }
    void reload() {
        const auto result = panel.refresh();
        if (!result) {
            error = result.error();
            return;
        }
        load_buffers();
        error.reset();
        notice.clear();
    }
    void read_history() {
        history_observation = scene.revision();
        const auto result = scene.history(scene.revision());
        if (!result) {
            history.reset();
            history_error = result.error();
        } else {
            history = result.value();
            history_error.reset();
        }
    }
    bool result(Result<SceneEditReceipt> receipt, bool inspector) {
        if (!receipt) {
            error = receipt.error();
            notice.clear();
            return false;
        }
        error = receipt.value().refresh_error;
        notice = receipt.value().changed ? "Scene updated" : "No change";
        if (inspector && !receipt.value().refresh_error)
            load_buffers();
        read_history();
        return receipt.value().changed;
    }
    bool apply() {
        if (!panel.draft())
            return false;
        if (mesh_present) {
            auto parsed = StableId::parse(asset.data());
            if (!parsed) {
                error = parsed.error();
                return false;
            }
            panel.draft()->mesh_renderer =
                MeshRenderer{AssetId{parsed.value()}, mesh_index, mesh_visible};
        } else {
            panel.draft()->mesh_renderer.reset();
        }
        return result(panel.apply(), true);
    }
    void property(const PropertySchema& field, InspectorValues& draft) {
        ImGui::PushID(static_cast<int>(field.id));
        field_label(field);
        switch (field.id) {
        case PropertyId::name_value:
            if (ImGui::InputText("##value", name.data(), name.size()))
                draft.name.value = name.data();
            break;
        case PropertyId::transform_translation:
            vector_field(draft.transform.translation);
            break;
        case PropertyId::transform_rotation: {
            auto& rotation = draft.transform.rotation;
            std::array<double, 4> values{rotation.x, rotation.y, rotation.z, rotation.w};
            if (ImGui::InputScalarN("##value", ImGuiDataType_Double, values.data(), 4, nullptr,
                                    nullptr, "%.9g"))
                rotation = {values[3], values[0], values[1], values[2]};
            ImGui::TextDisabled("Quaternion x, y, z, w");
            break;
        }
        case PropertyId::transform_scale:
            vector_field(draft.transform.scale);
            ImGui::TextDisabled("Signed scale; each magnitude must be at least 1e-6");
            break;
        case PropertyId::camera_projection: {
            auto& camera = *draft.camera;
            const auto& options = std::get<EnumerationConstraints>(field.constraints).values;
            const auto value = static_cast<std::uint32_t>(camera.projection);
            const auto selected =
                std::find_if(options.begin(), options.end(),
                             [&](const auto& item) { return item.value == value; });
            if (ImGui::BeginCombo("##value",
                                  selected == options.end() ? "Unknown" : selected->key.c_str())) {
                for (const auto& option : options)
                    if (ImGui::Selectable(option.key.c_str(), option.value == value))
                        camera.projection = static_cast<CameraProjection>(option.value);
                ImGui::EndCombo();
            }
            break;
        }
        case PropertyId::camera_vertical_fov_radians:
            ImGui::InputDouble("##value", &draft.camera->vertical_fov_radians, 0, 0, "%.9g");
            break;
        case PropertyId::camera_orthographic_height:
            ImGui::InputDouble("##value", &draft.camera->orthographic_height, 0, 0, "%.9g");
            break;
        case PropertyId::camera_near_clip:
            ImGui::InputDouble("##value", &draft.camera->near_clip, 0, 0, "%.9g");
            break;
        case PropertyId::camera_far_clip:
            ImGui::InputDouble("##value", &draft.camera->far_clip, 0, 0, "%.9g");
            break;
        case PropertyId::mesh_renderer_asset:
            ImGui::InputText("##value", asset.data(), asset.size());
            break;
        case PropertyId::mesh_renderer_mesh_index:
            ImGui::InputScalar("##value", ImGuiDataType_U32, &mesh_index);
            break;
        case PropertyId::mesh_renderer_visible:
            ImGui::Checkbox("##value", &mesh_visible);
            break;
        }
        ImGui::PopID();
    }
    void components() {
        auto& draft = *panel.draft();
        for (const auto& schema : schemas) {
            ImGui::PushID(static_cast<int>(schema.id));
            if (ImGui::CollapsingHeader(schema.display_name.c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                bool present = schema.required ||
                               (schema.id == ComponentSchemaId::camera ? draft.camera.has_value()
                                                                       : mesh_present);
                if (!schema.required) {
                    if (present && ImGui::SmallButton("Remove component from draft")) {
                        if (schema.id == ComponentSchemaId::camera)
                            draft.camera.reset();
                        else
                            mesh_present = false;
                        present = false;
                    } else if (!present && ImGui::SmallButton("Add component to draft")) {
                        if (schema.id == ComponentSchemaId::camera)
                            draft.camera.emplace();
                        else
                            mesh_present = true;
                        present = true;
                    }
                }
                if (present)
                    for (const auto& field : schema.properties)
                        property(field, draft);
                if (present && schema.id == ComponentSchemaId::camera)
                    ImGui::TextWrapped("Both projection modes' fields must be valid. Far clip must "
                                       "exceed near clip.");
                if (present && schema.id == ComponentSchemaId::mesh_renderer)
                    ImGui::TextWrapped("Supply an existing asset UUID. This reference does not "
                                       "import or resolve content.");
            }
            ImGui::PopID();
        }
    }
    bool draw(bool blocked) {
        bool mutated = false;
        ImGui::TextUnformatted("INSPECTOR AND HISTORY");
        if (!history_observation || *history_observation != scene.revision())
            read_history();
        ImGui::BeginDisabled(blocked || scene.needs_reconciliation());
        if (ImGui::Button("Refresh history"))
            read_history();
        const bool can_move = history && scene.current() && history->synchronized;
        ImGui::BeginDisabled(!can_move || !history->next_undo);
        if (ImGui::Button("Undo"))
            mutated = result(scene.undo(history->next_undo->ticket, history->revision), false);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!history || !scene.current() || !history->synchronized ||
                             !history->next_redo);
        if (ImGui::Button("Redo"))
            mutated =
                result(scene.redo(history->next_redo->ticket, history->revision), false) || mutated;
        ImGui::EndDisabled();
        if (history) {
            ImGui::SameLine();
            ImGui::TextDisabled("%zu undo / %zu redo", history->undo_entries,
                                history->redo_entries);
            if (!history->synchronized)
                ImGui::TextWrapped("History conflicts with the current scene. Reconcile through "
                                   "the shared owner before editing.");
        }
        if (history_error)
            show_error(*history_error);
        ImGui::Separator();
        if (!loaded && panel.draft())
            load_buffers();
        ImGui::BeginDisabled(!scene.selection() || !scene.current());
        if (ImGui::Button(panel.draft() ? "Reload selection / discard draft" : "Inspect selection"))
            reload();
        ImGui::EndDisabled();
        if (panel.draft()) {
            ImGui::TextWrapped("Entity: %s",
                               panel.draft()->selection.id.value().to_string().c_str());
            if (panel.stale())
                ImGui::TextWrapped("Selection or scene revision changed. Draft is retained; reload "
                                   "explicitly before applying.");
            if (changed())
                ImGui::TextColored({1, .73F, .34F, 1}, "Unapplied inspector edits");
            ImGui::TextWrapped("Apply commits all component changes together. Save scene saves "
                               "applied changes only.");
            ImGui::BeginDisabled(panel.stale());
            ImGui::BeginDisabled(!changed());
            if (ImGui::Button("Apply changes"))
                mutated = apply() || mutated;
            ImGui::EndDisabled();
            components();
            ImGui::EndDisabled();
        } else {
            ImGui::TextWrapped("Select an entity in the hierarchy, then inspect it.");
        }
        ImGui::EndDisabled();
        if (error)
            show_error(*error);
        if (!notice.empty())
            ImGui::TextWrapped("%s", notice.c_str());
        return mutated;
    }
};

InspectorWidget::InspectorWidget(InspectorPanel& panel, ScenePanel& scene)
    : impl_(std::make_unique<Impl>(panel, scene)) {}
InspectorWidget::~InspectorWidget() = default;
bool InspectorWidget::draw(bool blocked) {
    return impl_->draw(blocked);
}
bool InspectorWidget::has_changes() const noexcept {
    return impl_->changed();
}
void InspectorWidget::discard_draft() noexcept {
    impl_->panel.discard();
    impl_->loaded.reset();
    impl_->original_mesh.reset();
    impl_->mesh_present = false;
    impl_->error.reset();
    impl_->notice.clear();
}

} // namespace shark::editor
