#pragma once

#include "inspector_panel.hpp"

#include <memory>

namespace shark::editor {

// ImGui-only view of temporary inspector values and the common retained history.
// Neither widget nor draft owns a mutable Scene. Draw performs no native I/O.
class InspectorWidget final {
  public:
    InspectorWidget(InspectorPanel&, ScenePanel&);
    ~InspectorWidget();
    InspectorWidget(const InspectorWidget&) = delete;
    InspectorWidget& operator=(const InspectorWidget&) = delete;
    // Returns true after a known changed scene mutation. Draw before any viewport image.
    bool draw(bool blocked);
    [[nodiscard]] bool has_changes() const noexcept;
    void discard_draft() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::editor
