#include "asset_widget.hpp"

#include <algorithm>
#include <array>
#include <imgui.h>
#include <nlohmann/json.hpp>

namespace shark::editor {
namespace {
using namespace tools;
template <std::size_t N> void copy_text(std::array<char, N>& buffer, std::string_view text) {
    const auto count = std::min(text.size(), N - 1);
    std::copy_n(text.data(), count, buffer.data());
    buffer[count] = '\0';
}
void show_error(const Error& error) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1, .55F, .43F, 1});
    ImGui::TextWrapped("%s", error.message.c_str());
    ImGui::PopStyleColor();
    const auto code = error_code_name(error.code);
    ImGui::TextDisabled("%.*s", static_cast<int>(code.size()), code.data());
    if (!error.context.path.empty())
        ImGui::TextWrapped("Path: %s", error.context.path.c_str());
    if (!error.context.asset_id.empty())
        ImGui::TextWrapped("Asset: %s", error.context.asset_id.c_str());
}
const char* audience_name(AssetAudience value) {
    switch (value) {
    case AssetAudience::client:
        return "client";
    case AssetAudience::server:
        return "server";
    case AssetAudience::shared:
        return "shared";
    }
    return "unknown";
}
const char* cache_name(AssetCacheState value) {
    switch (value) {
    case AssetCacheState::missing:
        return "missing";
    case AssetCacheState::fresh:
        return "fresh";
    case AssetCacheState::stale:
        return "stale";
    }
    return "unknown";
}
const char* state_name(AssetImportJobState value) {
    switch (value) {
    case AssetImportJobState::queued:
        return "Queued";
    case AssetImportJobState::running:
        return "Running";
    case AssetImportJobState::ready:
        return "Ready: awaiting publication";
    case AssetImportJobState::succeeded:
        return "Succeeded";
    case AssetImportJobState::failed:
        return "Failed";
    case AssetImportJobState::cancelled:
        return "Cancelled";
    }
    return "Unknown";
}
bool terminal(AssetImportJobState value) {
    return value == AssetImportJobState::succeeded || value == AssetImportJobState::failed ||
           value == AssetImportJobState::cancelled;
}
void publication(const AssetPublication& value) {
    ImGui::Text("Published: %s; %zu bytes", value.published ? "yes" : "no", value.bytes);
    switch (value.durability) {
    case FileDurability::file_flushed:
        ImGui::TextUnformatted("File flushed");
        break;
    case FileDurability::file_and_directory_flushed:
        ImGui::TextUnformatted("File and directory flushed");
        break;
    case FileDurability::directory_flush_failed:
        ImGui::TextWrapped("Published; directory durability could not be confirmed.");
        break;
    }
}
void job_details(const AssetJobStatus& job) {
    ImGui::Text("%s (%zu / %zu nodes)", state_name(job.state), job.completed_nodes,
                job.total_nodes);
    ImGui::TextWrapped("Job: %s / %llu", job.id.scope.to_string().c_str(),
                       static_cast<unsigned long long>(job.id.sequence));
    if (job.summary) {
        ImGui::TextWrapped("Root: %s", job.summary->root.value().to_string().c_str());
        ImGui::TextWrapped("Target: %s (%s)", job.summary->target.id.c_str(),
                           audience_name(job.summary->target.side));
        ImGui::Text("Imported %zu; reused %zu", job.summary->imported, job.summary->reused);
        ImGui::TextWrapped("Artifact: %s", job.summary->root_key.to_hex().c_str());
    }
    if (job.commit) {
        ImGui::Text("Cache changed: %s; revision %llu", job.commit->changed ? "yes" : "no",
                    static_cast<unsigned long long>(job.commit->revision.value));
        if (job.commit->publication)
            publication(*job.commit->publication);
        else
            ImGui::TextUnformatted("No file publication was needed");
    }
    if (job.error)
        show_error(*job.error);
}
bool selectable_literal(std::string_view label, bool selected) {
    const auto position = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::Selectable("##asset", selected, ImGuiSelectableFlags_None,
                                           {0, ImGui::GetTextLineHeight()});
    ImGui::GetWindowDrawList()->AddText(position, ImGui::GetColorU32(ImGuiCol_Text), label.data(),
                                        label.data() + label.size());
    return clicked;
}
} // namespace

struct AssetWidget::Impl {
    AssetPanel& panel;
    ProjectSession& session;
    std::array<char, asset_panel_inventory_bytes + 1> sidecars{};
    std::array<char, 65> target{};
    int target_side{0};
    std::optional<AssetCacheStatus> selected_cache;
    std::optional<AssetArtifactInfo> artifact;
    std::optional<AssetJobStatus> taken;
    std::optional<AssetJobStatus> cancelled;
    std::optional<AssetDrainReceipt> drained;
    std::optional<AssetMetadataReceipt> written;
    std::optional<Error> error;
    std::string notice;
    std::array<char, 4097> metadata_path{};
    std::array<char, asset_metadata_maximum_bytes + 1> metadata_text{};
    std::array<char, asset_metadata_maximum_bytes + 1> metadata_baseline{};
    bool draft_active{false};
    bool create_only{false};
    int template_importer{-1};

    Impl(AssetPanel& assets, ProjectSession& owner) : panel(assets), session(owner) {
        copy_text(target, "windows-client");
    }
    bool changed() const noexcept {
        return draft_active &&
               (create_only || std::string_view(metadata_text.data()) != metadata_baseline.data());
    }
    AssetTargetProfile profile() const {
        return {target.data(), target_side == 0 ? AssetAudience::client : AssetAudience::server};
    }
    bool result(Result<void> value, std::string_view success) {
        if (!value) {
            error = value.error();
            notice.clear();
            return false;
        }
        error.reset();
        notice = success;
        return true;
    }
    void clear_cache_observation() {
        selected_cache.reset();
        artifact.reset();
    }
    bool load_inventory() {
        if (!result(panel.refresh_catalog(), "Catalog revision observed"))
            return false;
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
        if (!result(panel.refresh_inventory(paths),
                    "Asset inventory updated; refresh the catalog to browse it."))
            return false;
        clear_cache_observation();
        return true;
    }
    const AssetBrowserRecord* selected_record() const {
        if (!panel.page() || !panel.selection())
            return nullptr;
        const auto& records = panel.page()->records;
        const auto found = std::find_if(records.begin(), records.end(), [&](const auto& row) {
            return row.metadata.asset == *panel.selection();
        });
        return found == records.end() ? nullptr : &*found;
    }
    void browse() {
        if (ImGui::Button("Refresh catalog")) {
            result(panel.refresh_catalog(), "Catalog observed");
            clear_cache_observation();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh cache")) {
            result(panel.refresh_cache(), "Cache usage observed");
            clear_cache_observation();
        }
        if (panel.cache()) {
            const auto& cache = *panel.cache();
            ImGui::Text("Cache: %zu artifacts, %zu bindings, %zu payload bytes", cache.artifacts,
                        cache.bindings, cache.payload_bytes);
            if (cache.unpublished_stage)
                ImGui::TextWrapped("An unpublished cache stage needs explicit recovery through the "
                                   "terminal service.");
        }
        if (!panel.page()) {
            ImGui::TextWrapped("Refresh the catalog after loading the complete sidecar inventory.");
            return;
        }
        const auto offset = panel.page()->offset;
        const auto next = panel.page()->next_offset;
        ImGui::BeginDisabled(offset == 0);
        if (ImGui::SmallButton("Previous page")) {
            result(panel.query_page(offset >= asset_panel_page_size ? offset - asset_panel_page_size
                                                                    : 0),
                   "Catalog page observed");
            clear_cache_observation();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!next);
        if (ImGui::SmallButton("Next page")) {
            result(panel.query_page(*next), "Catalog page observed");
            clear_cache_observation();
        }
        ImGui::EndDisabled();
        if (!panel.page())
            return;
        ImGui::SameLine();
        ImGui::TextDisabled("%zu / %zu assets", panel.page()->records.size(), panel.page()->total);
        ImGui::BeginChild("Asset rows", {0, ImGui::GetTextLineHeightWithSpacing() * 9},
                          ImGuiChildFlags_Borders);
        for (const auto& row : panel.page()->records) {
            const auto id = row.metadata.asset.value().to_string();
            ImGui::PushID(id.c_str());
            if (selectable_literal(row.source, panel.selection() == row.metadata.asset)) {
                result(panel.select(row.metadata.asset), "Asset selected");
                clear_cache_observation();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (const auto* row = selected_record()) {
            ImGui::TextWrapped("Asset: %s", row->metadata.asset.value().to_string().c_str());
            ImGui::TextWrapped("Source: %s (%s)", row->source.c_str(),
                               row->source_present ? "present" : "missing");
            ImGui::TextWrapped("Sidecar: %s", row->sidecar.c_str());
            ImGui::TextWrapped("Kind: %s; importer: %s; audience: %s", row->metadata.kind.c_str(),
                               row->metadata.importer.c_str(),
                               audience_name(row->metadata.audience));
            ImGui::TextWrapped("Origin: %s", row->metadata.origin.c_str());
            ImGui::TextWrapped("License: %s", row->metadata.license.c_str());
            ImGui::Text("Dependencies: %zu", row->metadata.dependencies.size());
            ImGui::BeginDisabled(changed());
            if (ImGui::Button("Read selected metadata"))
                read_metadata(row->sidecar);
            ImGui::EndDisabled();
        }
        ImGui::BeginDisabled(!panel.selection() || !panel.cache());
        if (ImGui::Button("Check selected cache")) {
            auto value = panel.query_selected_cache(profile());
            if (!value) {
                error = value.error();
                selected_cache.reset();
            } else {
                error.reset();
                selected_cache = std::move(value).value();
            }
            artifact.reset();
        }
        ImGui::SameLine();
        if (ImGui::Button("Import selected")) {
            auto value = panel.submit_selected(profile());
            if (!value) {
                error = value.error();
                notice.clear();
            } else {
                error.reset();
                notice = value.value().deduplicated
                             ? "Matching pending import already exists. Refresh jobs to observe it."
                             : "Import admitted. Refresh jobs to observe progress.";
            }
        }
        ImGui::EndDisabled();
        if (selected_cache) {
            ImGui::Text("Observed cache state: %s", cache_name(selected_cache->state));
            if (selected_cache->required_key)
                ImGui::TextWrapped("Required key: %s",
                                   selected_cache->required_key->to_hex().c_str());
            if (selected_cache->last_valid_key) {
                ImGui::TextWrapped("Last valid key: %s",
                                   selected_cache->last_valid_key->to_hex().c_str());
                ImGui::TextWrapped(
                    "A retained artifact alone does not establish current freshness.");
                if (ImGui::Button("Inspect last valid artifact")) {
                    auto value = panel.inspect_artifact(*selected_cache->last_valid_key);
                    if (!value)
                        error = value.error();
                    else {
                        error.reset();
                        artifact = std::move(value).value();
                    }
                }
            }
            if (selected_cache->blocked_by)
                show_error(*selected_cache->blocked_by);
        }
        if (artifact) {
            ImGui::TextWrapped("Artifact %s", artifact->key.to_hex().c_str());
            ImGui::Text("%zu payload bytes; %zu dependencies", artifact->payload_bytes,
                        artifact->dependencies.size());
            ImGui::TextWrapped("Importer: %s v%u; payload v%u", artifact->importer.c_str(),
                               artifact->importer_version, artifact->payload_version);
        }
    }
    bool jobs() {
        bool invalidate = false;
        ImGui::TextUnformatted("IMPORT JOBS");
        ImGui::TextWrapped("Refresh observes only. Publish handles at most one ready result. "
                           "Acknowledge releases a terminal slot.");
        if (ImGui::Button("Refresh jobs"))
            result(panel.refresh_jobs(), "Job states observed");
        ImGui::SameLine();
        ImGui::BeginDisabled(!session.state_current());
        if (ImGui::Button("Publish ready result")) {
            auto value = panel.drain();
            if (!value) {
                error = value.error();
                notice.clear();
            } else {
                error.reset();
                notice = value.value().drained
                             ? "Ready completion handled; inspect the publication result below."
                             : "No ready completion was available.";
                invalidate = value.value().drained != 0;
                drained = std::move(value).value();
                if (invalidate)
                    clear_cache_observation();
            }
        }
        ImGui::EndDisabled();
        // Copy at most four small retained statuses: actions may replace the
        // model's vector, so a rendered row must not borrow across them.
        const std::vector<AssetJobStatus> observed(panel.jobs().begin(), panel.jobs().end());
        for (const auto& job : observed) {
            const auto key = job.id.scope.to_string() + "/" + std::to_string(job.id.sequence);
            ImGui::PushID(key.c_str());
            ImGui::Separator();
            job_details(job);
            ImGui::BeginDisabled(terminal(job.state));
            if (ImGui::SmallButton("Cancel import")) {
                auto value = panel.cancel(job.id);
                if (!value)
                    error = value.error();
                else {
                    error.reset();
                    cancelled = std::move(value).value();
                    notice = "Cancellation decision received; inspect its result below and refresh "
                             "jobs.";
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!terminal(job.state));
            if (ImGui::SmallButton("Acknowledge result")) {
                auto value = panel.take_job(job.id);
                if (!value)
                    error = value.error();
                else {
                    error.reset();
                    taken = std::move(value).value();
                    notice =
                        "Terminal slot released; its last acknowledged result is retained below.";
                }
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (observed.empty())
            ImGui::TextDisabled("No observed retained jobs");
        if (cancelled &&
            ImGui::CollapsingHeader("Last cancellation result", ImGuiTreeNodeFlags_DefaultOpen))
            job_details(*cancelled);
        if (drained &&
            ImGui::CollapsingHeader("Last publication check", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Handled completions: %zu", drained->drained);
            for (const auto& job : drained->jobs) {
                ImGui::Separator();
                job_details(job);
            }
        }
        if (taken &&
            ImGui::CollapsingHeader("Last acknowledged result", ImGuiTreeNodeFlags_DefaultOpen))
            job_details(*taken);
        ImGui::TextWrapped("Closing the project cancels and joins uncommitted work. Publish and "
                           "acknowledge intended results before closing.");
        return invalidate;
    }
    bool set_metadata(std::string_view path, std::string_view text, bool creating) {
        if (path.size() >= metadata_path.size() || text.size() >= metadata_text.size()) {
            error = Error{ErrorCode::invalid_argument,
                          "Metadata draft exceeds its fixed editor bounds",
                          {.operation = "editor.assets.metadata"}};
            return false;
        }
        copy_text(metadata_path, path);
        copy_text(metadata_text, text);
        copy_text(metadata_baseline, text);
        draft_active = true;
        create_only = creating;
        return true;
    }
    void read_metadata(std::string_view path) {
        const std::string owned_path{path};
        if (!result(panel.read_metadata(owned_path),
                    "Metadata observed at its exact file revision"))
            return;
        if (panel.metadata())
            set_metadata(panel.metadata()->path, panel.metadata()->json, false);
    }
    Result<void> discard() {
        auto value = panel.release_metadata();
        if (!value)
            return value;
        draft_active = false;
        create_only = false;
        metadata_text[0] = '\0';
        metadata_baseline[0] = '\0';
        return {};
    }
    void new_metadata() {
        const auto importers = panel.importers();
        if (template_importer < 0 ||
            static_cast<std::size_t>(template_importer) >= importers.size())
            return;
        const auto identity = generate_stable_id();
        if (!identity) {
            error = identity.error();
            return;
        }
        if (!result(discard(), "New create-only metadata draft"))
            return;
        const auto& importer = importers[static_cast<std::size_t>(template_importer)];
        // Provenance, license and audience require an explicit author decision.
        // Blank fields intentionally fail shared validation until completed.
        const nlohmann::json value{{"format", "shark.asset"},
                                   {"schema_version", 1},
                                   {"project_id", session.state().project_id},
                                   {"asset_id", identity.value().to_string()},
                                   {"kind", importer.kind},
                                   {"origin", ""},
                                   {"license", ""},
                                   {"importer", importer.id},
                                   {"audience", ""},
                                   {"settings", nlohmann::json::object()},
                                   {"dependencies", nlohmann::json::array()},
                                   {"extras", nlohmann::json::object()}};
        const std::string path{metadata_path.data()};
        set_metadata(path, value.dump(2), true);
    }
    bool metadata_editor() {
        bool invalidate = false;
        ImGui::TextUnformatted("ASSET METADATA");
        ImGui::TextWrapped(
            "Metadata writes are immediate file changes outside scene Save and Undo. Use the "
            "complete schema-1 JSON envelope; no source type or license is inferred.");
        ImGui::BeginDisabled(draft_active);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("Sidecar path", metadata_path.data(), metadata_path.size());
        ImGui::EndDisabled();
        ImGui::BeginDisabled(changed());
        if (ImGui::Button("Read metadata"))
            read_metadata(metadata_path.data());
        ImGui::SameLine();
        if (ImGui::Button("Discover importers")) {
            result(panel.refresh_importers(), "Registered importers observed");
            template_importer = -1;
        }
        const auto importers = panel.importers();
        const bool valid_importer = template_importer >= 0 &&
                                    static_cast<std::size_t>(template_importer) < importers.size();
        if (ImGui::BeginCombo(
                "New draft importer",
                valid_importer ? importers[static_cast<std::size_t>(template_importer)].id.c_str()
                               : "Choose importer")) {
            for (std::size_t index = 0; index < importers.size(); ++index)
                if (ImGui::Selectable(importers[index].id.c_str(),
                                      template_importer == static_cast<int>(index)))
                    template_importer = static_cast<int>(index);
            ImGui::EndCombo();
        }
        ImGui::BeginDisabled(!valid_importer || metadata_path[0] == '\0');
        if (ImGui::Button("New create-only draft"))
            new_metadata();
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (valid_importer && ImGui::TreeNode("Importer settings schema")) {
            ImGui::TextWrapped("%s", importers[static_cast<std::size_t>(template_importer)]
                                         .settings_schema_json.c_str());
            ImGui::TreePop();
        }
        if (draft_active) {
            ImGui::TextDisabled("%s", create_only
                                          ? "Create only: destination must be absent"
                                          : "Replace only the exact file revision read above");
            ImGui::InputTextMultiline("##metadata-json", metadata_text.data(), metadata_text.size(),
                                      {-1, ImGui::GetTextLineHeight() * 14});
            if (changed())
                ImGui::TextColored({1, .73F, .34F, 1}, "Unwritten metadata edits");
            if (ImGui::Button("Write metadata file")) {
                auto value =
                    panel.write_metadata(metadata_path.data(), metadata_text.data(), create_only);
                if (!value) {
                    error = value.error();
                    notice.clear();
                } else {
                    written = std::move(value).value();
                    error.reset();
                    notice =
                        "Metadata published. Reload the complete asset inventory before importing.";
                    if (panel.metadata())
                        set_metadata(panel.metadata()->path, panel.metadata()->json, false);
                    clear_cache_observation();
                    invalidate = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard metadata draft"))
                result(discard(), "Metadata draft discarded");
        }
        if (written) {
            ImGui::TextWrapped("Last metadata write: %s", written->path.c_str());
            publication(written->publication);
        }
        return invalidate;
    }
    bool draw(bool blocked) {
        bool invalidate = false;
        ImGui::BeginDisabled(blocked || !session.state_current());
        if (ImGui::CollapsingHeader("Inventory and import target",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextWrapped(
                "List the complete project-relative sidecar inventory, including dependencies. "
                "Omitting a sidecar removes its catalog entry, not its files.");
            ImGui::InputTextMultiline("##inventory", sidecars.data(), sidecars.size(),
                                      {-1, ImGui::GetTextLineHeight() * 4});
            if (ImGui::Button("Load complete inventory"))
                invalidate = load_inventory();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("Import target", target.data(), target.size()))
                clear_cache_observation();
            constexpr const char* sides[]{"client", "server"};
            if (ImGui::Combo("Target side", &target_side, sides, 2))
                clear_cache_observation();
            ImGui::TextWrapped("The scene viewport uses its own explicit client target. Match that "
                               "target when importing content for it.");
        }
        if (ImGui::CollapsingHeader("Catalog and cache", ImGuiTreeNodeFlags_DefaultOpen))
            browse();
        ImGui::EndDisabled();
        // Job observation/cancellation/acknowledgement remain available for
        // cleanup even if an unrelated project-status observation failed.
        ImGui::BeginDisabled(blocked);
        if (ImGui::CollapsingHeader("Import jobs", ImGuiTreeNodeFlags_DefaultOpen))
            invalidate = jobs() || invalidate;
        ImGui::EndDisabled();
        ImGui::BeginDisabled(blocked || !session.state_current());
        if (ImGui::CollapsingHeader("Metadata editor", ImGuiTreeNodeFlags_DefaultOpen))
            invalidate = metadata_editor() || invalidate;
        ImGui::EndDisabled();
        if (error)
            show_error(*error);
        if (!notice.empty())
            ImGui::TextWrapped("%s", notice.c_str());
        return invalidate;
    }
};

AssetWidget::AssetWidget(AssetPanel& panel, ProjectSession& session)
    : impl_(std::make_unique<Impl>(panel, session)) {}
AssetWidget::~AssetWidget() = default;
bool AssetWidget::draw(bool blocked) {
    return impl_->draw(blocked);
}
bool AssetWidget::has_changes() const noexcept {
    return impl_->changed();
}
Result<void> AssetWidget::discard_draft() {
    return impl_->discard();
}

} // namespace shark::editor
