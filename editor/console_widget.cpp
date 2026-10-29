#include "console_widget.hpp"

#include <algorithm>
#include <imgui.h>

namespace shark::editor {
void ConsoleWidget::draw(ProjectSession& session) {
    ImGui::TextUnformatted("CONSOLE");
    ImGui::TextWrapped("Authoring outcomes and diagnostics for this project session. Clearing "
                       "this view does not acknowledge jobs, undo edits or remove source files.");
    if (ImGui::Button("Clear console")) {
        const auto cleared = session.clear_console();
        if (!cleared)
            ImGui::TextWrapped("%s", cleared.error().message.c_str());
        else {
            page_ = 0;
            selected_.reset();
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu / %zu entries; %llu older or unavailable entries",
                        session.console().entries().size(), console_maximum_entries,
                        static_cast<unsigned long long>(session.console().dropped()));
    const auto entries = session.console().entries();
    constexpr std::size_t page_size = 16;
    const auto pages = std::max<std::size_t>(1, (entries.size() + page_size - 1) / page_size);
    page_ = std::min(page_, pages - 1);
    ImGui::BeginDisabled(page_ == 0);
    if (ImGui::Button("Previous"))
        --page_;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(page_ + 1 >= pages);
    if (ImGui::Button("Next"))
        ++page_;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Page %zu / %zu", page_ + 1, pages);
    ImGui::Separator();
    const auto begin = page_ * page_size;
    for (std::size_t index = begin; index < std::min(entries.size(), begin + page_size); ++index) {
        const auto& entry = entries[entries.size() - index - 1];
        const auto color = entry.level == LogLevel::error     ? ImVec4{1, .55F, .43F, 1}
                           : entry.level == LogLevel::warning ? ImVec4{1, .73F, .34F, 1}
                                                              : ImVec4{.35F, .82F, .77F, 1};
        ImGui::PushID(static_cast<int>(index));
        if (ImGui::SmallButton("Details"))
            selected_ = entry.sequence;
        ImGui::SameLine();
        ImGui::TextColored(color, "%s: %s", entry.method.c_str(), entry.message.c_str());
        ImGui::PopID();
    }
    const auto selected = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
        return selected_ && entry.sequence == *selected_;
    });
    if (selected == entries.end()) {
        selected_.reset();
        return;
    }
    ImGui::Separator();
    const auto& entry = *selected;
    ImGui::TextWrapped("%s", entry.message.c_str());
    ImGui::TextDisabled("Request %s / repeated %llu", entry.request_id.c_str(),
                        static_cast<unsigned long long>(entry.repeats));
    if (!entry.job_id.empty())
        ImGui::TextWrapped("Job %s", entry.job_id.c_str());
    if (entry.published)
        ImGui::TextWrapped("Published: %s", entry.durability.c_str());
    if (entry.error) {
        const auto& error = *entry.error;
        const auto code = error_code_name(error.code);
        ImGui::TextDisabled("%.*s / %s", static_cast<int>(code.size()), code.data(),
                            error.context.operation.c_str());
        if (!error.context.path.empty())
            ImGui::TextWrapped("Path: %s", error.context.path.c_str());
        if (!error.context.entity_id.empty())
            ImGui::TextWrapped("Entity: %s", error.context.entity_id.c_str());
        if (!error.context.asset_id.empty())
            ImGui::TextWrapped("Asset: %s", error.context.asset_id.c_str());
    }
    if (entry.truncated)
        ImGui::TextDisabled("Display text shortened or sanitized; original outcome is unchanged.");
}
} // namespace shark::editor
