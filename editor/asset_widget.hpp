#pragma once

#include "asset_panel.hpp"

#include <memory>

namespace shark::editor {

// Native-editor view over the retained CPU adapter. Draw only observes cached
// values unless a user explicitly invokes a control; it never drains jobs.
class AssetWidget final {
  public:
    AssetWidget(AssetPanel&, ProjectSession&);
    ~AssetWidget();
    AssetWidget(const AssetWidget&) = delete;
    AssetWidget& operator=(const AssetWidget&) = delete;
    // True means a catalog/cache/metadata action may invalidate a cached view.
    bool draw(bool blocked);
    [[nodiscard]] bool has_changes() const noexcept;
    [[nodiscard]] Result<void> discard_draft();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::editor
