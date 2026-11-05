#pragma once

#include "scene_panel.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace shark::editor {

// Bounded display/draft state over ProjectSession's retained prefab services.
// Source publication and Scene instantiation are separate explicit operations.
class PrefabPanel final {
  public:
    explicit PrefabPanel(ProjectSession& session);
    ~PrefabPanel();
    PrefabPanel(const PrefabPanel&) = delete;
    PrefabPanel& operator=(const PrefabPanel&) = delete;

    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] bool has_changes() const noexcept;
    [[nodiscard]] bool uncertain() const noexcept;
    [[nodiscard]] bool instantiation_uncertain() const noexcept;
    [[nodiscard]] bool prepared() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] const std::string& text() const noexcept;
    [[nodiscard]] const std::string& notice() const noexcept;
    [[nodiscard]] const std::optional<AuthoredEntityId>& prepared_root() const noexcept;

    [[nodiscard]] Result<void> open(std::string_view path);
    [[nodiscard]] Result<void> capture(std::string_view path, AuthoredEntityId selected,
                                       AssetId asset, SceneRevision expected_revision);
    [[nodiscard]] Result<void> edit(std::string_view text);
    [[nodiscard]] Result<void> write();
    [[nodiscard]] Result<void> reconcile();
    [[nodiscard]] Result<void> prepare();
    [[nodiscard]] Result<void> release_preparation();
    [[nodiscard]] Result<SceneEditReceipt> instantiate();
    [[nodiscard]] Result<SceneEditReceipt> reconcile_instantiation();
    // Refuses to abandon an uncertain Scene mutation. Reconcile its original
    // identity first; discarding a file draft never undoes a known Scene edit.
    [[nodiscard]] Result<void> discard();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::editor
