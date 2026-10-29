#include "transform_drag.hpp"

#include <limits>
#include <utility>

namespace shark::editor {
namespace {
Error drag_error(const char* message) {
    return {ErrorCode::conflict, message, {.operation = "editor.transform-drag"}};
}
} // namespace

TransformDrag::TransformDrag(ScenePanel& panel) noexcept : panel_(panel) {}

Result<void> TransformDrag::check_thread() const {
    if (std::this_thread::get_id() != thread_)
        return drag_error("Transform drags require the creating thread");
    return {};
}

Result<void> TransformDrag::begin() {
    auto thread = check_thread();
    if (!thread)
        return thread;
    if (baseline_)
        return drag_error("Finish or cancel the current transform drag first");
    if (!panel_.current() || !panel_.selection() || panel_.needs_reconciliation())
        return drag_error("Select a current entity and reconcile pending edits before dragging");
    if (panel_.selection_epoch() == std::numeric_limits<std::uint64_t>::max())
        return drag_error("Selection identities are exhausted; recreate the scene panel");
    auto observation = panel_.observe_transform(*panel_.selection());
    if (!observation)
        return std::move(observation).error();
    baseline_ = std::move(observation).value();
    selection_epoch_ = panel_.selection_epoch();
    candidate_.reset();
    return {};
}

bool TransformDrag::active() const noexcept {
    return baseline_.has_value();
}

bool TransformDrag::stale() const noexcept {
    return baseline_ && (!panel_.current() || panel_.needs_reconciliation() ||
                         !panel_.selection() || panel_.selection_epoch() != selection_epoch_ ||
                         panel_.selection()->id != baseline_->selection.id ||
                         panel_.selection()->revision != baseline_->selection.revision);
}

const std::optional<Transform>& TransformDrag::candidate() const noexcept {
    return candidate_;
}

Result<void> TransformDrag::update(const tools::TransformManipulation& manipulation) {
    auto thread = check_thread();
    if (!thread)
        return thread;
    candidate_.reset();
    if (!baseline_ || stale())
        return drag_error("Transform selection or revision changed; begin a new drag");
    auto candidate = tools::calculate_manipulated_transform(baseline_->local,
                                                            baseline_->parent_world, manipulation);
    if (!candidate)
        return std::move(candidate).error();
    auto preview = panel_.preview_transform(baseline_->selection, candidate.value());
    if (!preview)
        return preview;
    candidate_ = std::move(candidate).value();
    return {};
}

void TransformDrag::discard() noexcept {
    candidate_.reset();
    baseline_.reset();
}

Result<void> TransformDrag::cancel() {
    auto thread = check_thread();
    if (!thread)
        return thread;
    discard();
    return {};
}

Result<SceneEditReceipt> TransformDrag::commit() {
    auto thread = check_thread();
    if (!thread)
        return std::move(thread).error();
    if (!baseline_ || !candidate_ || stale()) {
        discard();
        return drag_error("Transform drag has no current validated candidate");
    }
    const auto selection = baseline_->selection;
    const auto candidate = *candidate_;
    // Every commit attempt ends this gesture. Ambiguous delivery remains in the
    // ScenePanel ledger; cancel/begin cannot forget or replace its retry identity.
    discard();
    return panel_.apply_transform(selection, candidate);
}

} // namespace shark::editor
