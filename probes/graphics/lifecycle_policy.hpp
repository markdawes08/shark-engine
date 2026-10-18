#pragma once

#include <chrono>
#include <cstdint>

namespace shark::probe {

// The same guard is used before real presentation. A minimized or zero-sized
// framebuffer must not acquire an image or create a zero-extent swapchain.
constexpr bool drawableFramebuffer(int width, int height, bool minimized) noexcept {
    return width > 0 && height > 0 && !minimized;
}

// The caller owns progress so stack unwinding cannot discard completed work.
// Counters advance only after the corresponding native operation succeeds.
struct LifecycleProgress final {
    std::uint32_t resizeCycles = 0;
    std::uint32_t minimized = 0;
    std::uint32_t zeroExtents = 0;
    std::uint32_t frames = 0;
    std::uint32_t completedDevices = 0;
    std::uint32_t deviceCycle = 0;
    std::uint32_t resizeCycle = 0;
    double seconds = 0;
    const char* phase = "not_started";

    [[nodiscard]] bool fullWorkloadObserved() const noexcept {
        return resizeCycles >= 100 && completedDevices >= 20 && zeroExtents > 0 &&
               minimized >= 100 && frames > 0 && seconds >= 600;
    }
};

// Capture elapsed presentation time before surrounding GPU cleanup, including
// when presentation throws. The clock is injectable for deterministic tests.
template <class Clock = std::chrono::steady_clock> class PresentationObservation final {
  public:
    explicit PresentationObservation(LifecycleProgress& progress) noexcept
        : progress_(progress), started_(Clock::now()) {
        progress_.phase = "timed_presentation";
    }

    ~PresentationObservation() noexcept {
        progress_.seconds = std::chrono::duration<double>(Clock::now() - started_).count();
    }

    PresentationObservation(const PresentationObservation&) = delete;
    PresentationObservation& operator=(const PresentationObservation&) = delete;

    [[nodiscard]] typename Clock::time_point started() const noexcept {
        return started_;
    }

  private:
    LifecycleProgress& progress_;
    typename Clock::time_point started_;
};

} // namespace shark::probe
