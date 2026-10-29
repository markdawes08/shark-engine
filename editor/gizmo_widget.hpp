#pragma once

#include "scene_panel.hpp"
#include "viewport_navigation.hpp"

#include <shark/client/input.hpp>
#include <shark/tools/transform_manipulation.hpp>

#include <functional>
#include <memory>

namespace shark::editor {

struct GizmoFrameResult {
    bool consumed{false};
    std::optional<SceneEditReceipt> receipt;
    std::optional<Error> error;
};

// Runs on the same owner thread, without mutation: prepare the candidate's
// render snapshot/packing before it can be shown or committed on release.
using GizmoPreviewValidator = std::function<Result<void>(const tools::AuthoringTransformPreview&)>;

class GizmoWidget final {
  public:
    explicit GizmoWidget(ScenePanel&);
    ~GizmoWidget();
    GizmoWidget(const GizmoWidget&) = delete;
    GizmoWidget& operator=(const GizmoWidget&) = delete;

    // Empty operation is ordinary selection. Configuration cancels any gesture;
    // these are editor settings, not a second authoring mutation surface.
    [[nodiscard]] Result<void> configure(std::optional<tools::TransformOperation>,
                                         tools::TransformSpace, tools::TransformSnap = {});
    void controls(bool blocked);
    // Call after the actual ImGui Image, before navigation/picking. A consumed
    // frame includes a gesture's release/cancellation and must not also navigate.
    [[nodiscard]] GizmoFrameResult overlay(const client::InputSnapshot&, const ViewportRectangle&,
                                           const RenderScene&, bool hovered, bool blocked,
                                           const GizmoPreviewValidator& validate_preview = {});
    // Caller cancels when a tab/image disappears, resource/view reset or close.
    void cancel() noexcept;
    [[nodiscard]] bool has_changes() const noexcept;
    [[nodiscard]] std::optional<tools::AuthoringTransformPreview> preview() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::editor
