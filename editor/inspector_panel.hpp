#pragma once

#include "scene_panel.hpp"

namespace shark::editor {

// Temporary typed draft over the same ScenePanel/project writer. Selection or
// revision changes never silently retarget or rebase the retained draft.
class InspectorPanel final {
  public:
    explicit InspectorPanel(ScenePanel& panel) noexcept;
    [[nodiscard]] Result<void> refresh();
    void discard() noexcept;
    [[nodiscard]] std::optional<InspectorValues>& draft() noexcept;
    [[nodiscard]] const std::optional<InspectorValues>& draft() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool stale() const noexcept;
    [[nodiscard]] Result<SceneEditReceipt> apply();

  private:
    ScenePanel& panel_;
    std::optional<InspectorValues> draft_;
    std::optional<InspectorValues> baseline_;
};

} // namespace shark::editor
