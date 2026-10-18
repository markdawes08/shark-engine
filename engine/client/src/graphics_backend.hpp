#pragma once

#include <shark/client/graphics.hpp>

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
