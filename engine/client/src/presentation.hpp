#pragma once

#include "graphics_backend.hpp"

namespace shark::client::detail {

// Device-owned policy. Backend primitives retain all actual GPU resources.
class Presentation final {
  public:
    explicit Presentation(GraphicsBackend& backend, GraphicsDiagnosticSink& sink) noexcept
        : backend_(backend), sink_(sink) {}
    [[nodiscard]] Result<PresentationReport> frame(ClearColor color);
    [[nodiscard]] Result<void> drain();
    [[nodiscard]] Result<void> close();
    [[nodiscard]] Error fault(Error error);
    [[nodiscard]] const PresentationStats& stats() const noexcept {
        return stats_;
    }
    [[nodiscard]] bool safe_to_destroy() const noexcept {
        return !attempted_ && !backend_.offscreen_pending();
    }

  private:
    [[nodiscard]] Result<void> check_diagnostics();
    template <class T> Result<T> checked(Result<T> value) {
        if (!value) {
            return value;
        }
        if (auto checked = check_diagnostics(); !checked) {
            return checked.error();
        }
        return value;
    }
    [[nodiscard]] Result<DrainStatus> drain_for(std::uint64_t timeout);
    [[nodiscard]] Result<PresentationReport> frame_impl(ClearColor color);
    [[nodiscard]] Result<PresentationReport> finish_present(bool recreated);
    [[nodiscard]] PresentationReport report(PresentationStatus status, bool recreated = false);
    void complete_slot(std::uint32_t slot) noexcept;
    void update_in_flight() noexcept;
    void retire_swapchain() noexcept;

    GraphicsBackend& backend_;
    GraphicsDiagnosticSink& sink_;
    PresentationStats stats_{};
    std::array<std::uint64_t, 2> serials_{};
    std::optional<Error> failure_{};
    WindowExtent requested_{};
    std::optional<std::pair<std::uint32_t, std::uint32_t>> pending_{};
    std::uint32_t next_slot_{0};
    bool attempted_{false};
    bool swapchain_attempted_{false};
    bool dirty_{false};
};

} // namespace shark::client::detail
