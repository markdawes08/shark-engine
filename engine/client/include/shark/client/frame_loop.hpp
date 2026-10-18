#pragma once

#include <shark/client/input.hpp>
#include <shark/time.hpp>

namespace shark::client {

struct OfflineFrameControl {
    bool paused{false};
    bool single_step{false};
    bool iconified{false};
};

struct OfflineFrameReport {
    StepReport timing{};
    bool paused{false};
    bool presentation_suspended{false};
};

// The snapshot is borrowed only during the synchronous callback. Copy it to
// retain it. This local 60 Hz contract is not a multiplayer intent protocol.
using OfflineInputCallback =
    std::function<Result<void>(std::uint64_t tick, const InputSnapshot& input)>;

class OfflineFrameLoop final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<OfflineFrameLoop>>
    create(MonotonicTime origin, std::uint64_t first_tick = 0);

    OfflineFrameLoop(const OfflineFrameLoop&) = delete;
    OfflineFrameLoop& operator=(const OfflineFrameLoop&) = delete;
    OfflineFrameLoop(OfflineFrameLoop&&) = delete;
    OfflineFrameLoop& operator=(OfflineFrameLoop&&) = delete;

    // One serialized owner, normally the window's main thread. Supply each
    // capture once, after polling events. The caller retains its independent
    // frame snapshot for presentation, even if zero or several ticks execute.
    // Unfocused/iconified windows pause offline ticks. Step requires an explicit
    // manual pause and an active window; paused interval actions never replay.
    [[nodiscard]] Result<OfflineFrameReport> advance(MonotonicTime now, const InputSnapshot& input,
                                                     OfflineFrameControl control,
                                                     const OfflineInputCallback& callback,
                                                     std::stop_token stop = {});
    [[nodiscard]] std::uint64_t next_tick() const noexcept;
    [[nodiscard]] bool faulted() const noexcept;

  private:
    explicit OfflineFrameLoop(std::unique_ptr<OfflineScheduler> scheduler) noexcept;
    std::unique_ptr<OfflineScheduler> scheduler_;
    InputSnapshot pending_{};
    bool advancing_{false};
    bool paused_{false};
};

} // namespace shark::client
