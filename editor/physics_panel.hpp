#pragma once

#include "project_session.hpp"
#include <shark/time.hpp>
#include <span>

namespace shark::editor {

struct PhysicsPreviewState {
    bool active{false};
    bool running{false};
    bool uncertain{false};
    bool faulted{false};
    std::optional<StableId> session_id;
    SceneRevision source_revision{};
    std::uint64_t tick{0};
    std::uint64_t next_sequence{1};
    std::size_t events{0};
    double alpha{1};
};

// Editor-private adapter over the retained shared scene.physics service. It owns
// scheduling/display observations only; the protocol owns the runtime Scene.
class PhysicsPanel final {
  public:
    explicit PhysicsPanel(ProjectSession&);
    ~PhysicsPanel();
    PhysicsPanel(const PhysicsPanel&) = delete;
    PhysicsPanel& operator=(const PhysicsPanel&) = delete;
    [[nodiscard]] const PhysicsPreviewState& state() const noexcept;
    // Bounded owned details from the last nonempty operation, retained after
    // stop for inspection and cleared by the next successful start.
    [[nodiscard]] std::span<const std::string> contact_details() const noexcept;
    // Bounded display-only script observations; retained across Stop.
    [[nodiscard]] std::span<const std::string> script_details() const noexcept;
    [[nodiscard]] Result<void> start(AuthoredEntityId camera, SceneRevision expected,
                                     std::string_view target, MonotonicTime now);
    void pause(bool paused) noexcept;
    [[nodiscard]] Result<void> poll(MonotonicTime now, bool blocked);
    [[nodiscard]] Result<void> step(MonotonicTime now);
    [[nodiscard]] Result<void> remove(AuthoredEntityId entity);
    [[nodiscard]] Result<void> reconcile(MonotonicTime now);
    [[nodiscard]] Result<void> stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::editor
