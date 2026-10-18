#pragma once

#include <shark/client/window.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace shark::client {

enum class GraphicsValidation { required, off };
enum class AdapterKind { other, integrated, discrete, software };

struct GraphicsCapabilities {
    // Vulkan 1.3, timelineSemaphore, synchronization2, dynamicRendering,
    // VK_KHR_swapchain, one graphics+compute+present queue, sRGB surface, FIFO.
    std::array<bool, 8> features{};
    // Combined optimal tiling/usage support: RGBA8 sRGB, RGBA8 UNORM,
    // RGBA16 float, D32 float (including 2048x2048x12 depth array support).
    std::array<bool, 4> formats{};
    // 2D extent, cube extent, array layers, color attachments, bound descriptor
    // sets, sampled images/stage, samplers/stage, UBO bytes, SSBO bytes,
    // compute invocations. Thresholds are the unchanged assignment-02 profile.
    std::array<std::uint32_t, 10> limits{};
};

struct GraphicsAdapter {
    std::string uuid;
    std::string name;
    std::string driver_name;
    std::string driver_info;
    std::uint32_t vendor_id{0};
    std::uint32_t device_id{0};
    std::uint32_t driver_version{0};
    std::uint32_t api_version{0};
    std::uint32_t queue_family{0};
    AdapterKind kind{AdapterKind::other};
    GraphicsCapabilities capabilities{};
};

struct GraphicsConfig {
    // Empty selects a supported hardware device: discrete, integrated, other;
    // UUID sorts ties. An explicit lowercase 32-hex UUID never falls back.
    std::string adapter_uuid;
    GraphicsValidation validation{GraphicsValidation::required};
    WindowConfig window{};
};

struct GraphicsDiagnostics {
    std::uint32_t warnings{0};
    std::uint32_t errors{0};
    std::uint32_t dropped{0};
    std::vector<std::string> messages;
};

// Pure bounded profile check; it neither initializes a loader nor accesses GPU.
[[nodiscard]] std::vector<std::string>
missing_graphics_requirements(const GraphicsAdapter& adapter);
[[nodiscard]] bool valid_graphics_uuid(std::string_view uuid) noexcept;
[[nodiscard]] bool native_graphics_available() noexcept;

class GraphicsDevice;
namespace detail {
class GraphicsBackend;
[[nodiscard]] Result<std::unique_ptr<GraphicsDevice>>
create_graphics(const GraphicsConfig& config, std::unique_ptr<GraphicsBackend> backend);
[[nodiscard]] Result<std::vector<GraphicsAdapter>>
inventory_graphics(const GraphicsConfig& config, std::unique_ptr<GraphicsBackend> backend);
} // namespace detail

// One nonmovable main-thread owner of Window -> instance -> surface -> device
// -> NVRHI. No Vulkan/NVRHI handles escape. No GPU submission is exposed in05a.
// Initial creation on the process main thread is a caller precondition; methods
// enforce that creating thread. Live destruction elsewhere terminates.
class GraphicsDevice final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<GraphicsDevice>>
    create(const GraphicsConfig& config = {});
    [[nodiscard]] static Result<std::vector<GraphicsAdapter>>
    inventory(const GraphicsConfig& config = {});
    ~GraphicsDevice() noexcept;
    GraphicsDevice(const GraphicsDevice&) = delete;
    GraphicsDevice& operator=(const GraphicsDevice&) = delete;
    GraphicsDevice(GraphicsDevice&&) = delete;
    GraphicsDevice& operator=(GraphicsDevice&&) = delete;

    [[nodiscard]] Result<GraphicsAdapter> adapter() const;
    [[nodiscard]] Result<GraphicsDiagnostics> diagnostics() const;
    // Retains cumulative diagnostics, including destruction callbacks, after
    // close. No device wait is needed: this increment exposes no submissions.
    [[nodiscard]] Result<void> close();

  private:
    struct Impl;
    explicit GraphicsDevice(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
    friend Result<std::unique_ptr<GraphicsDevice>>
    detail::create_graphics(const GraphicsConfig&, std::unique_ptr<detail::GraphicsBackend>);
    friend Result<std::vector<GraphicsAdapter>>
    detail::inventory_graphics(const GraphicsConfig&, std::unique_ptr<detail::GraphicsBackend>);
};

} // namespace shark::client
