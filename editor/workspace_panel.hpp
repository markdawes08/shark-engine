#pragma once

#include "project_session.hpp"
#include "viewport_navigation.hpp"
#include <memory>
#include <shark/client/graphics.hpp>

namespace shark::editor {

struct ViewportPointerActions {
    NavigationMotion motion;
    bool pick{false};
    double x{0};
    double y{0};
};

// Editor-private routing over the current raw snapshot and the actual displayed
// image rectangle. Leaving/cancelling never resumes a held drag without a new
// press. It performs no input injection, native capture or scene mutation.
class ViewportPointerRouter final {
  public:
    [[nodiscard]] ViewportPointerActions route(const client::InputSnapshot& input,
                                               const ViewportRectangle& rectangle, bool hovered,
                                               bool blocked) noexcept;
    void cancel() noexcept;

  private:
    enum class Drag { none, orbit, pan };
    Drag drag_{Drag::none};
    std::optional<ViewportRectangle> rectangle_;
    double previous_x_{0};
    double previous_y_{0};
};

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
