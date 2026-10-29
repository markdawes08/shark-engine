#pragma once

#include "project_session.hpp"

#include <shark/scene.hpp>
#include <shark/schema.hpp>
#include <shark/tools/authoring_history.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace shark::editor {

inline constexpr std::size_t scene_panel_maximum_rows = 4096;
inline constexpr std::size_t scene_panel_page_size = 64;

struct HierarchyRow {
    AuthoredEntityId id;
    std::string name;
    std::optional<AuthoredEntityId> parent;
    std::size_t depth;
    std::size_t child_count;
    bool has_camera;
    bool has_mesh_renderer;
};

struct SceneSelection {
    AuthoredEntityId id;
    SceneRevision revision;
};

struct SceneEditReceipt {
    std::optional<AuthoredEntityId> entity;
    bool changed;
    SceneRevision revision;
    // A known successful edit remains a success if display refresh fails.
    std::optional<Error> refresh_error;
};

struct InspectorValues {
    SceneSelection selection;
    Name name;
    Transform transform;
    std::optional<Camera> camera;
    std::optional<MeshRenderer> mesh_renderer;
};

struct TransformObservation {
    SceneSelection selection;
    Transform local;
    math::Mat4 parent_world{1.0};
};

// Read-only bounded display snapshot over one ProjectSession, never another
// mutable scene. The borrowed session must outlive the panel on its owner thread.
// Query cursors are consumed completely; display paging retains no wire cursor.
class ScenePanel final {
  public:
    explicit ScenePanel(ProjectSession& session);
    ~ScenePanel();
    ScenePanel(const ScenePanel&) = delete;
    ScenePanel& operator=(const ScenePanel&) = delete;
    ScenePanel(ScenePanel&&) = delete;
    ScenePanel& operator=(ScenePanel&&) = delete;

    [[nodiscard]] Result<void> refresh();
    [[nodiscard]] bool current() const noexcept;
    [[nodiscard]] SceneRevision revision() const noexcept;
    [[nodiscard]] std::span<const HierarchyRow> rows() const noexcept;
    [[nodiscard]] std::span<const HierarchyRow> page() const noexcept;
    [[nodiscard]] std::size_t page_index() const noexcept;
    [[nodiscard]] std::size_t page_count() const noexcept;
    bool next_page() noexcept;
    bool previous_page() noexcept;
    [[nodiscard]] const std::optional<SceneSelection>& selection() const noexcept;
    // Changes on selection transitions, including away-and-back without a scene
    // mutation. Exhaustion invalidates future drag captures instead of wrapping.
    [[nodiscard]] std::uint64_t selection_epoch() const noexcept;
    [[nodiscard]] Result<void> select(AuthoredEntityId id);
    void clear_selection() noexcept;

    // UI confirmations retain the exact observed entity and revision. Any
    // external revision change invalidates selection, including delete/undo
    // recreation whose native generation intentionally stays protocol-private.
    [[nodiscard]] Result<SceneEditReceipt>
    create(std::string_view name, std::optional<AuthoredEntityId> parent, SceneRevision expected);
    [[nodiscard]] Result<SceneEditReceipt> erase(AuthoredEntityId id, SceneRevision expected);
    [[nodiscard]] Result<SceneEditReceipt> reparent(AuthoredEntityId id,
                                                    std::optional<AuthoredEntityId> parent,
                                                    ReparentMode mode, SceneRevision expected);

    [[nodiscard]] Result<InspectorValues> inspect(AuthoredEntityId id, SceneRevision expected);
    // One atomic retained batch and history group for a complete component draft.
    [[nodiscard]] Result<SceneEditReceipt> apply_components(const InspectorValues& values);
    [[nodiscard]] Result<TransformObservation> observe_transform(SceneSelection expected);
    [[nodiscard]] Result<void> preview_transform(SceneSelection expected,
                                                 const Transform& transform);
    [[nodiscard]] Result<SceneEditReceipt> apply_transform(SceneSelection expected,
                                                           const Transform& transform);
    [[nodiscard]] Result<tools::AuthoringHistoryStatus> history(SceneRevision expected);
    [[nodiscard]] Result<SceneEditReceipt> undo(tools::AuthoringHistoryTicket ticket,
                                                SceneRevision expected);
    [[nodiscard]] Result<SceneEditReceipt> redo(tools::AuthoringHistoryTicket ticket,
                                                SceneRevision expected);

    // Ambiguous delivery blocks new intents. Lookup the original retained
    // mutation; never allocate a replacement identity or silently replay it.
    [[nodiscard]] bool needs_reconciliation() const noexcept;
    [[nodiscard]] Result<SceneEditReceipt> reconcile();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::editor
