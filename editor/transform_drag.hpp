#pragma once

#include "scene_panel.hpp"

#include <shark/tools/transform_manipulation.hpp>

#include <cstdint>
#include <optional>
#include <thread>

namespace shark::editor {

// One owner-thread gesture over the retained project. The candidate is an owned
// display value, never a live Scene write. Panel must outlive this controller.
class TransformDrag final {
  public:
    explicit TransformDrag(ScenePanel& panel) noexcept;
    [[nodiscard]] Result<void> begin();
    // Motion is cumulative from begin, including after a rejected update.
    // A rejection clears the candidate; release cannot commit an older pose.
    [[nodiscard]] Result<void> update(const tools::TransformManipulation& manipulation);
    [[nodiscard]] Result<SceneEditReceipt> commit();
    [[nodiscard]] Result<void> cancel();

    // Observations, like the panel, are read on the creating thread only.
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool stale() const noexcept;
    [[nodiscard]] const std::optional<Transform>& candidate() const noexcept;

  private:
    [[nodiscard]] Result<void> check_thread() const;
    void discard() noexcept;
    ScenePanel& panel_;
    const std::thread::id thread_{std::this_thread::get_id()};
    std::optional<TransformObservation> baseline_;
    std::optional<Transform> candidate_;
    std::uint64_t selection_epoch_{0};
};

} // namespace shark::editor
