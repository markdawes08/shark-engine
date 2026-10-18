#pragma once

#include <shark/client/graphics.hpp>

#include "window_backend.hpp"
#include <atomic>

namespace shark::client::detail {

// Callback writers reserve one fixed slot, copy at most511 bytes and publish.
// No locks, allocation, I/O, user callbacks or retry loops inside record().
// Snapshot readers only read published immutable slots. Storage outlives all
// backend objects, including instance destruction callbacks.
class GraphicsDiagnosticSink final {
  public:
    void record(bool error, const char* message) noexcept;
    [[nodiscard]] GraphicsDiagnostics snapshot() const;
    [[nodiscard]] bool has_errors() const noexcept {
        return errors_.load(std::memory_order_relaxed) != 0;
    }

  private:
    struct Slot {
        std::array<char, 512> text{};
        std::atomic<bool> claimed{false};
        std::atomic<bool> ready{false};
    };
    std::array<Slot, 64> slots_{};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> warnings_{0};
    std::atomic<std::uint64_t> errors_{0};
};

enum class AcquireStatus { ready, suboptimal, out_of_date, waiting };
struct AcquiredImage {
    AcquireStatus status{AcquireStatus::waiting};
    std::uint32_t image_index{0};
};
enum class PresentStatus { presented, suboptimal, out_of_date, waiting };
enum class DrainStatus { complete, waiting, device_lost };
struct SwapchainInfo {
    WindowExtent extent{};
    std::uint32_t image_count{0};
};

// Private qualification observations of engine-owned native handles. These do
// not estimate driver allocations, resident GPU bytes or hidden NVRHI storage.
struct GraphicsResourceSnapshot {
    std::uint32_t live_swapchains{0};
    std::uint32_t image_wrappers{0};
    std::uint32_t image_views{0};
    std::uint32_t present_semaphores{0};
    std::uint32_t present_fences{0};
    std::uint32_t command_lists{0};
    std::uint32_t acquire_semaphores{0};
    std::uint32_t acquire_fences{0};
    std::uint32_t pending_queue_workers{0};
    std::uint32_t retained_recordings{0};
};

// Failed/throwing stages may partially acquire resources. The owner marks each
// attempted phase and calls its noexcept cleanup once, in reverse order.
// Native backends reuse the existing Window owner, not a second GLFW lifecycle.
class GraphicsBackend {
  public:
    virtual ~GraphicsBackend() = default;
    [[nodiscard]] virtual Result<void> create_window(const WindowConfig&) = 0;
    [[nodiscard]] virtual Result<void> create_instance(GraphicsValidation,
                                                       GraphicsDiagnosticSink&) = 0;
    [[nodiscard]] virtual Result<void> create_surface() = 0;
    [[nodiscard]] virtual Result<std::vector<GraphicsAdapter>> enumerate_adapters() = 0;
    [[nodiscard]] virtual Result<void> create_device(std::size_t adapter_index) = 0;
    [[nodiscard]] virtual Result<void> create_nvrhi(GraphicsDiagnosticSink&) = 0;
    // Optional device-free seam defaults preserve foundation-only fixtures.
    [[nodiscard]] virtual Result<GraphicsWindowFrame> poll_window();
    [[nodiscard]] virtual Result<WindowState> window_state();
    [[nodiscard]] virtual Result<void> window_command(WindowCommand, WindowExtent);
    [[nodiscard]] virtual Result<PresentationSync> create_presentation();
    // Empty optional means a transient unavailable/zero extent or out-of-date
    // surface. No live new swapchain resources remain in that case.
    [[nodiscard]] virtual Result<std::optional<SwapchainInfo>> create_swapchain(WindowExtent);
    [[nodiscard]] virtual Result<bool> wait_slot(std::uint32_t slot, std::uint64_t timeout_ns);
    [[nodiscard]] virtual Result<AcquiredImage> acquire(std::uint32_t slot,
                                                        std::uint64_t timeout_ns);
    [[nodiscard]] virtual Result<void> submit(std::uint32_t slot, std::uint32_t image, ClearColor);
    [[nodiscard]] virtual Result<PresentStatus> present(std::uint32_t slot, std::uint32_t image,
                                                        std::uint64_t timeout_ns);
    // Includes acquisition completion, submitted GPU use and (when used)
    // presentation fences. Uses a single total deadline. Device-lost permits
    // destruction but is never success. Failed/timeout drain MUST retain owners.
    [[nodiscard]] virtual Result<DrainStatus> drain_presentation(std::uint64_t timeout_ns);
    [[nodiscard]] virtual GraphicsResourceSnapshot presentation_resources() const noexcept {
        return {};
    }
    virtual void destroy_swapchain() noexcept;
    virtual void destroy_presentation() noexcept;
    virtual void destroy_nvrhi() noexcept = 0;
    virtual void destroy_device() noexcept = 0;
    virtual void destroy_surface() noexcept = 0;
    virtual void destroy_instance() noexcept = 0;
    virtual void destroy_window() noexcept = 0;
};

[[nodiscard]] std::unique_ptr<GraphicsBackend> make_graphics_backend();
[[nodiscard]] Result<std::vector<GraphicsAdapter>>
inventory_graphics(const GraphicsConfig&, std::unique_ptr<GraphicsBackend>);

} // namespace shark::client::detail
