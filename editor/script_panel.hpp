#pragma once

#include "project_session.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace shark::editor {

// One bounded source draft. ProjectSession remains the only file writer; the
// opaque revision is never silently rebased on external edits.
class ScriptPanel final {
  public:
    explicit ScriptPanel(ProjectSession& session) noexcept : session_(session) {}
    [[nodiscard]] bool loaded() const noexcept {
        return loaded_;
    }
    [[nodiscard]] bool has_changes() const noexcept {
        return loaded_ && (is_new_ || text_ != baseline_ || uncertain_);
    }
    [[nodiscard]] bool uncertain() const noexcept {
        return uncertain_;
    }
    [[nodiscard]] const std::string& path() const noexcept {
        return path_;
    }
    [[nodiscard]] const std::string& text() const noexcept {
        return text_;
    }
    [[nodiscard]] const std::string& notice() const noexcept {
        return notice_;
    }
    [[nodiscard]] Result<void> open(std::string_view path);
    [[nodiscard]] Result<void> create(std::string_view path);
    [[nodiscard]] Result<void> edit(std::string_view text);
    [[nodiscard]] Result<void> write();
    [[nodiscard]] Result<void> reconcile();
    [[nodiscard]] Result<void> discard();

  private:
    ProjectSession& session_;
    std::string path_;
    std::string text_;
    std::string baseline_;
    std::string notice_;
    std::optional<std::string> token_;
    bool loaded_{false};
    bool is_new_{false};
    bool uncertain_{false};
};

} // namespace shark::editor
