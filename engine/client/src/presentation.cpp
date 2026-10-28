#include "presentation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace shark::client::detail {
namespace {
constexpr std::uint64_t frame_wait_ns = 5'000'000;
constexpr std::uint64_t acquire_wait_ns = 1'000'000;
constexpr std::uint64_t close_wait_ns = 1'000'000'000;

Error failure(ErrorCode code, const char* operation, const char* text) {
    return {code, text, {.operation = operation}};
}
Error unavailable() {
    return failure(ErrorCode::capability_unavailable, "graphics.presentation.backend",
                   "Presentation backend is unavailable");
}
bool valid_color(ClearColor color) noexcept {
    for (const auto value : {color.red, color.green, color.blue, color.alpha}) {
        if (!std::isfinite(value) || value < 0.0F || value > 1.0F) {
            return false;
        }
    }
    return true;
}
void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}
} // namespace

Result<GraphicsWindowFrame> GraphicsBackend::poll_window() {
    return unavailable();
}
Result<WindowState> GraphicsBackend::window_state() {
    return unavailable();
}
Result<void> GraphicsBackend::window_command(WindowCommand, WindowExtent) {
    return unavailable();
}
Result<PresentationSync> GraphicsBackend::create_presentation() {
    return unavailable();
}
Result<std::optional<SwapchainInfo>> GraphicsBackend::create_swapchain(WindowExtent) {
    return unavailable();
}
Result<bool> GraphicsBackend::wait_slot(std::uint32_t, std::uint64_t) {
    return unavailable();
}
Result<AcquiredImage> GraphicsBackend::acquire(std::uint32_t, std::uint64_t) {
    return unavailable();
}
Result<void> GraphicsBackend::submit(std::uint32_t, std::uint32_t, ClearColor) {
    return unavailable();
}
Result<PresentStatus> GraphicsBackend::present(std::uint32_t, std::uint32_t, std::uint64_t) {
    return unavailable();
}
Result<DrainStatus> GraphicsBackend::drain_presentation(std::uint64_t) {
    return DrainStatus::complete;
}
void GraphicsBackend::destroy_swapchain() noexcept {}
void GraphicsBackend::destroy_presentation() noexcept {}

Error Presentation::fault(Error error) {
    stats_.faulted = true;
    if (!failure_) {
        failure_ = std::move(error);
    }
    return *failure_;
}
Result<void> Presentation::check_diagnostics() {
    if (!sink_.has_errors()) {
        return {};
    }
    const auto diagnostics = sink_.snapshot();
    auto message =
        "Graphics validation reported " + std::to_string(diagnostics.errors) + " error(s)";
    if (!diagnostics.messages.empty()) {
        message += ": " + diagnostics.messages.front().substr(0, 768);
    }
    return fault(
        {ErrorCode::execution_failed, std::move(message), {.operation = "graphics.validation"}});
}
void Presentation::update_in_flight() noexcept {
    stats_.in_flight = 0;
    for (const auto serial : serials_) {
        if (serial > stats_.completed) {
            ++stats_.in_flight;
        }
    }
    stats_.peak_in_flight = std::max(stats_.peak_in_flight, stats_.in_flight);
}
void Presentation::complete_slot(std::uint32_t slot) noexcept {
    // One queue: completion of a later submission also completes all earlier use.
    stats_.completed = std::max(stats_.completed, serials_[slot]);
    update_in_flight();
}
PresentationReport Presentation::report(PresentationStatus status, bool recreated) {
    if (status == PresentationStatus::waiting) {
        increment(stats_.waiting_frames);
    }
    if (status == PresentationStatus::suspended) {
        increment(stats_.suspended_frames);
    }
    return {status, stats_, recreated};
}
Result<DrainStatus> Presentation::drain_for(std::uint64_t timeout) {
    if (!attempted_ && !backend_.offscreen_pending()) {
        return DrainStatus::complete;
    }
    auto drained = backend_.drain_presentation(timeout);
    if (!drained) {
        return fault(drained.error());
    }
    // Diagnostics remain fatal, but must not discard a valid completion proof
    // needed to release owned resources during close.
    (void)check_diagnostics();
    if (drained.value() == DrainStatus::complete) {
        stats_.completed = stats_.submitted;
        update_in_flight();
        // Acquired but unpresented work may be discarded by explicit drain.
        // Native drain retains it safely until swapchain retirement.
        return DrainStatus::complete;
    }
    if (drained.value() == DrainStatus::device_lost) {
        (void)fault(failure(ErrorCode::execution_failed, "graphics.presentation.device_lost",
                            "Graphics device was lost; application resources may be destroyed"));
        return DrainStatus::device_lost;
    }
    if (drained.value() != DrainStatus::waiting) {
        return fault(failure(ErrorCode::execution_failed, "graphics.presentation.drain",
                             "Backend returned an invalid drain outcome"));
    }
    return DrainStatus::waiting;
}
Result<void> Presentation::drain() {
    if (stats_.closed) {
        return failure_ ? Result<void>{*failure_} : Result<void>{};
    }
    try {
        auto result = drain_for(close_wait_ns);
        if (!result) {
            return result.error();
        }
        if (result.value() == DrainStatus::waiting) {
            return failure(ErrorCode::execution_failed, "graphics.presentation.drain_timeout",
                           "GPU drain timed out; live resources are retained");
        }
        if (failure_) {
            return *failure_;
        }
        return {};
    } catch (...) {
        return fault(failure(ErrorCode::execution_failed, "graphics.presentation.drain",
                             "GPU drain threw; live resources are retained"));
    }
}
void Presentation::retire_swapchain() noexcept {
    if (swapchain_attempted_) {
        backend_.destroy_swapchain();
        swapchain_attempted_ = false;
    }
    if (stats_.active) {
        increment(stats_.retired_generations);
    }
    stats_.active = false;
    stats_.image_count = 0;
    stats_.extent = {};
    pending_.reset();
}
Result<void> Presentation::prepare_draw_update() {
    if (auto drained = drain(); !drained)
        return drained.error();
    if (pending_)
        retire_swapchain();
    return {};
}
Result<void> Presentation::close() {
    if (stats_.closed) {
        return failure_ ? Result<void>{*failure_} : Result<void>{};
    }
    try {
        auto drained = drain_for(close_wait_ns);
        if (!drained) {
            return drained.error();
        }
        if (drained.value() == DrainStatus::waiting) {
            return failure(ErrorCode::execution_failed, "graphics.presentation.drain_timeout",
                           "GPU drain timed out; live resources are retained");
        }
        retire_swapchain();
        if (attempted_) {
            backend_.destroy_presentation();
            attempted_ = false;
        }
        stats_.closed = true;
        if (failure_) {
            return *failure_;
        }
        return {};
    } catch (...) {
        return fault(failure(ErrorCode::execution_failed, "graphics.presentation.close",
                             "Presentation cleanup threw; live resources are retained"));
    }
}
Result<PresentationReport> Presentation::finish_present(bool recreated) {
    const auto [slot, image] = *pending_;
    auto presented = backend_.present(slot, image, frame_wait_ns);
    if (!presented) {
        return fault(presented.error());
    }
    if (presented.value() == PresentStatus::presented ||
        presented.value() == PresentStatus::suboptimal) {
        increment(stats_.presented);
    }
    if (auto checked = check_diagnostics(); !checked) {
        return checked.error();
    }
    if (presented.value() == PresentStatus::waiting) {
        return report(PresentationStatus::waiting, recreated);
    }
    pending_.reset();
    if (presented.value() == PresentStatus::out_of_date) {
        dirty_ = true;
        return report(PresentationStatus::recreate_pending, recreated);
    }
    if (presented.value() != PresentStatus::presented &&
        presented.value() != PresentStatus::suboptimal) {
        return fault(failure(ErrorCode::execution_failed, "graphics.presentation.present",
                             "Backend returned an invalid present outcome"));
    }
    dirty_ = dirty_ || presented.value() == PresentStatus::suboptimal;
    auto complete = checked(backend_.wait_slot(slot, 0));
    if (!complete) {
        return fault(complete.error());
    }
    if (complete.value()) {
        complete_slot(slot);
    }
    return report(PresentationStatus::presented, recreated);
}
Result<PresentationReport> Presentation::frame_impl(ClearColor color) {
    auto state = checked(backend_.window_state());
    if (!state) {
        return fault(state.error());
    }
    if (state.value().close_requested) {
        return report(PresentationStatus::closed);
    }
    const auto extent = state.value().framebuffer;
    if (extent.width == 0 || extent.height == 0 || state.value().iconified) {
        return report(PresentationStatus::suspended);
    }
    if (pending_) {
        return finish_present(false);
    }
    if (!attempted_) {
        attempted_ = true;
        auto created = checked(backend_.create_presentation());
        if (!created) {
            return fault(created.error());
        }
        if (created.value() != PresentationSync::present_fence &&
            created.value() != PresentationSync::host_completion) {
            return fault(failure(ErrorCode::execution_failed, "graphics.presentation.create",
                                 "Backend returned an invalid synchronization strategy"));
        }
        stats_.synchronization = created.value();
        stats_.frame_limit = created.value() == PresentationSync::present_fence ? 2U : 1U;
    }
    bool recreated = false;
    if (!stats_.active || dirty_ || extent != requested_) {
        auto drained = drain_for(frame_wait_ns);
        if (!drained) {
            return drained.error();
        }
        if (failure_) {
            return *failure_;
        }
        if (drained.value() == DrainStatus::waiting) {
            return report(PresentationStatus::waiting);
        }
        if (drained.value() == DrainStatus::device_lost) {
            return *failure_;
        }
        retire_swapchain();
        if (auto checked = check_diagnostics(); !checked) {
            return checked.error();
        }
        if (stats_.generation == std::numeric_limits<std::uint64_t>::max()) {
            return fault(failure(ErrorCode::execution_failed, "graphics.presentation.generation",
                                 "Swapchain generation exhausted"));
        }
        swapchain_attempted_ = true;
        auto created = checked(backend_.create_swapchain(extent));
        if (!created) {
            return fault(created.error());
        }
        if (!created.value()) {
            backend_.destroy_swapchain();
            swapchain_attempted_ = false;
            if (auto checked = check_diagnostics(); !checked) {
                return checked.error();
            }
            return report(PresentationStatus::recreate_pending);
        }
        const auto& info = *created.value();
        if (info.extent.width == 0 || info.extent.height == 0 || info.image_count == 0 ||
            info.image_count > 16) {
            return fault(failure(ErrorCode::execution_failed, "graphics.presentation.swapchain",
                                 "Backend returned an invalid bounded swapchain"));
        }
        stats_.extent = info.extent;
        stats_.image_count = info.image_count;
        stats_.active = true;
        ++stats_.generation;
        requested_ = extent;
        dirty_ = false;
        recreated = true;
    }
    const auto slot = next_slot_;
    auto ready = checked(backend_.wait_slot(slot, frame_wait_ns));
    if (!ready) {
        return fault(ready.error());
    }
    if (!ready.value()) {
        return report(PresentationStatus::waiting, recreated);
    }
    complete_slot(slot);
    auto acquired = checked(backend_.acquire(slot, acquire_wait_ns));
    if (!acquired) {
        return fault(acquired.error());
    }
    if (acquired.value().status == AcquireStatus::waiting) {
        return report(PresentationStatus::waiting, recreated);
    }
    if (acquired.value().status == AcquireStatus::out_of_date) {
        dirty_ = true;
        return report(PresentationStatus::recreate_pending, recreated);
    }
    if ((acquired.value().status != AcquireStatus::ready &&
         acquired.value().status != AcquireStatus::suboptimal) ||
        acquired.value().image_index >= stats_.image_count) {
        return fault(failure(ErrorCode::execution_failed, "graphics.presentation.acquire",
                             "Backend returned an invalid acquired image"));
    }
    dirty_ = dirty_ || acquired.value().status == AcquireStatus::suboptimal;
    if (stats_.submitted == std::numeric_limits<std::uint64_t>::max()) {
        return fault(failure(ErrorCode::execution_failed, "graphics.presentation.submit",
                             "Submission serial exhausted"));
    }
    if (auto submitted = backend_.submit(slot, acquired.value().image_index, color); !submitted) {
        return fault(submitted.error());
    }
    serials_[slot] = ++stats_.submitted;
    update_in_flight();
    if (auto checked = check_diagnostics(); !checked) {
        return checked.error();
    }
    pending_ = {{slot, acquired.value().image_index}};
    next_slot_ = (slot + 1) % stats_.frame_limit;
    return finish_present(recreated);
}
Result<PresentationReport> Presentation::frame(ClearColor color) {
    if (!valid_color(color)) {
        return failure(ErrorCode::invalid_argument, "graphics.presentation.color",
                       "Clear color requires four finite linear values in [0, 1]");
    }
    if (stats_.closed) {
        return failure(ErrorCode::conflict, "graphics.presentation.closed",
                       "Graphics device is closed");
    }
    if (failure_) {
        return *failure_;
    }
    try {
        if (auto checked = check_diagnostics(); !checked) {
            return checked.error();
        }
        return frame_impl(color);
    } catch (...) {
        return fault(
            failure(ErrorCode::execution_failed, "graphics.presentation.frame",
                    "Presentation threw; acquired resources remain owned for safe cleanup"));
    }
}
} // namespace shark::client::detail
