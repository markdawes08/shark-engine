#pragma once

#include "project_session.hpp"
#include <memory>
#include <shark/client/graphics.hpp>

namespace shark::editor {

// Application-private panels. ProjectSession owns all authoritative state;
// these panels retain only bounded query/display values and a render snapshot.
class WorkspacePanel final {
  public:
    WorkspacePanel(ProjectSession&, const client::ShaderArtifact& vertex,
                   const client::ShaderArtifact& pixel);
    ~WorkspacePanel();
    WorkspacePanel(const WorkspacePanel&) = delete;
    WorkspacePanel& operator=(const WorkspacePanel&) = delete;
    void draw(const client::GraphicsWindowFrame&, bool blocked);
    void cancel_confirmation() noexcept;
    [[nodiscard]] bool has_unapplied_drafts() const noexcept;
    [[nodiscard]] Result<void> discard_drafts();
    [[nodiscard]] const client::SceneViewportRequest* viewport() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::editor
