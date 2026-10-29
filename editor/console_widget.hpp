#pragma once

#include "project_session.hpp"

namespace shark::editor {
// Read-only outcomes, with bounded summary pages and one selected detail view.
class ConsoleWidget final {
  public:
    void draw(ProjectSession&);

  private:
    std::size_t page_{0};
    std::optional<std::uint64_t> selected_;
};
} // namespace shark::editor
