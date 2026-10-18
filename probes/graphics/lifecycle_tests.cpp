#include "lifecycle_policy.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string_view>

namespace {

struct ObservationClock final {
    using time_point = std::chrono::steady_clock::time_point;
    static inline time_point current{};
    static time_point now() noexcept {
        return current;
    }
};

} // namespace

TEST_CASE("Presentation suspends for zero framebuffer extents and minimize", "[graphics-probe]") {
    using shark::probe::drawableFramebuffer;
    REQUIRE_FALSE(drawableFramebuffer(0, 64, false));
    REQUIRE_FALSE(drawableFramebuffer(64, 0, false));
    REQUIRE_FALSE(drawableFramebuffer(0, 0, false));
    REQUIRE_FALSE(drawableFramebuffer(64, 64, true));
    REQUIRE_FALSE(drawableFramebuffer(-1, 64, false));
    REQUIRE(drawableFramebuffer(64, 64, false));
    REQUIRE(drawableFramebuffer(128, 96, false));
}

TEST_CASE("Presentation failure retains progress and elapsed time before cleanup",
          "[graphics-probe]") {
    shark::probe::LifecycleProgress progress;
    ObservationClock::current = ObservationClock::time_point{};
    try {
        shark::probe::PresentationObservation<ObservationClock> observation(progress);
        progress.resizeCycles = 3;
        progress.minimized = 4;
        progress.zeroExtents = 4;
        progress.completedDevices = 2;
        progress.deviceCycle = 2;
        progress.resizeCycle = 3;
        progress.frames = 10;
        ObservationClock::current += std::chrono::seconds(7);
        throw std::runtime_error("controlled presentation failure");
    } catch (const std::runtime_error&) {
        // Later teardown time must not inflate the presentation duration.
        ObservationClock::current += std::chrono::seconds(30);
    }
    REQUIRE(std::string_view(progress.phase) == "timed_presentation");
    REQUIRE(progress.seconds == 7.0);
    REQUIRE(progress.resizeCycles == 3);
    REQUIRE(progress.minimized == 4);
    REQUIRE(progress.zeroExtents == 4);
    REQUIRE(progress.completedDevices == 2);
    REQUIRE(progress.deviceCycle == 2);
    REQUIRE(progress.resizeCycle == 3);
    REQUIRE(progress.frames == 10);
    REQUIRE_FALSE(progress.fullWorkloadObserved());
}

TEST_CASE("Successful presentation records the same clock interval", "[graphics-probe]") {
    shark::probe::LifecycleProgress progress;
    ObservationClock::current = ObservationClock::time_point{} + std::chrono::seconds(100);
    {
        shark::probe::PresentationObservation<ObservationClock> observation(progress);
        REQUIRE(observation.started() == ObservationClock::current);
        ObservationClock::current += std::chrono::seconds(600);
    }
    REQUIRE(progress.seconds == 600.0);
}
