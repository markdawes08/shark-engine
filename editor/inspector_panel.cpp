#include "inspector_panel.hpp"

#include <utility>

namespace shark::editor {
namespace {
Error inspector_error(ErrorCode code, const char* message) {
    return {code, message, {.operation = "editor.inspector"}};
}
bool same_selection(const SceneSelection& left, const SceneSelection& right) noexcept {
    return left.id == right.id && left.revision == right.revision;
}
} // namespace

InspectorPanel::InspectorPanel(ScenePanel& panel) noexcept : panel_(panel) {}

void InspectorPanel::discard() noexcept {
    draft_.reset();
    baseline_.reset();
}

Result<void> InspectorPanel::refresh() {
    try {
        if (!panel_.current())
            return inspector_error(ErrorCode::conflict,
                                   "Refresh the hierarchy before reloading the draft");
        if (!panel_.selection()) {
            discard();
            return {};
        }
        const auto selected = *panel_.selection();
        auto observation = panel_.inspect(selected.id, selected.revision);
        if (!observation)
            return std::move(observation).error();
        auto baseline = std::move(observation).value();
        auto draft = baseline;
        baseline_ = std::move(baseline);
        draft_ = std::move(draft);
        return {};
    } catch (...) {
        return inspector_error(ErrorCode::execution_failed, "Cannot load the inspector draft");
    }
}

std::optional<InspectorValues>& InspectorPanel::draft() noexcept {
    return draft_;
}
const std::optional<InspectorValues>& InspectorPanel::draft() const noexcept {
    return draft_;
}
bool InspectorPanel::dirty() const noexcept {
    if (!draft_ || !baseline_)
        return draft_.has_value() != baseline_.has_value();
    return !same_selection(draft_->selection, baseline_->selection) ||
           draft_->name != baseline_->name || draft_->transform != baseline_->transform ||
           draft_->camera != baseline_->camera || draft_->mesh_renderer != baseline_->mesh_renderer;
}
bool InspectorPanel::stale() const noexcept {
    return draft_ && (!baseline_ || !same_selection(draft_->selection, baseline_->selection) ||
                      !panel_.current() || !panel_.selection() ||
                      !same_selection(draft_->selection, *panel_.selection()));
}

Result<SceneEditReceipt> InspectorPanel::apply() {
    if (!draft_ || !baseline_)
        return inspector_error(ErrorCode::conflict,
                               "Select an entity and load its inspector draft");
    if (stale())
        return inspector_error(ErrorCode::conflict,
                               "Inspector selection or revision changed; reload explicitly");
    auto result = panel_.apply_components(*draft_);
    if (!result)
        return result;
    auto receipt = std::move(result).value();
    // Publication has succeeded. Observation failure cannot turn that into a
    // rollback claim or discard the original mutation receipt.
    if (!receipt.refresh_error) {
        auto refreshed = refresh();
        if (!refreshed)
            receipt.refresh_error = std::move(refreshed).error();
    }
    return receipt;
}

} // namespace shark::editor
