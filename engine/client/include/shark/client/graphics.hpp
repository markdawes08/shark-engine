#pragma once

#include <shark/client/gpu_resources.hpp>
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

enum class PresentationSync { present_fence, host_completion };
enum class PresentationStatus { presented, suspended, waiting, recreate_pending, closed };

// Linear clear color for the presentation-only sample; no shader/scene API.
struct ClearColor {
    float red{0.02F};
    float green{0.06F};
    float blue{0.12F};
    float alpha{1.0F};
};

struct PresentationStats {
    PresentationSync synchronization{PresentationSync::host_completion};
    WindowExtent extent{};
    std::uint32_t image_count{0};
    std::uint32_t frame_limit{2};
    std::uint32_t in_flight{0};
    std::uint32_t peak_in_flight{0};
    std::uint64_t submitted{0};
    std::uint64_t completed{0};
    std::uint64_t presented{0};
    std::uint64_t generation{0};
    std::uint64_t retired_generations{0};
    std::uint64_t suspended_frames{0};
    std::uint64_t waiting_frames{0};
    bool active{false};
    bool faulted{false};
    bool closed{false};
};

struct PresentationReport {
    PresentationStatus status{PresentationStatus::waiting};
    PresentationStats stats{};
    bool recreated{false};
};

struct GraphicsWindowFrame {
    WindowState state{};
    InputSnapshot input{};
    WindowDiagnostics diagnostics{};
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
// -> NVRHI -> presentation. No Vulkan/NVRHI handles escape.
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
    [[nodiscard]] Result<GraphicsWindowFrame> poll_window();
    [[nodiscard]] Result<WindowState> window_state() const;
    [[nodiscard]] Result<void> resize(WindowExtent content);
    [[nodiscard]] Result<void> iconify();
    [[nodiscard]] Result<void> restore();
    [[nodiscard]] Result<void> show();
    [[nodiscard]] Result<void> focus();
    [[nodiscard]] Result<void> request_close();
    // Lazy presentation startup. One bounded attempt per call; waiting or
    // recreate_pending requires a later event-pump pass, never an internal spin.
    [[nodiscard]] Result<PresentationReport> present(ClearColor color = {});
    [[nodiscard]] Result<PresentationStats> presentation_stats() const;
    // Bounded offscreen diagnostic draw on this device's owned graphics queue.
    // Uploads and returns tightly packed RGBA8/D32 readback. A one-second GPU
    // wait timeout retains owners until drain/close proves completion; retry
    // rendering requires first draining the retained submission.
    [[nodiscard]] Result<OffscreenReadback> render_offscreen(const OffscreenRequest& request);
    // Waits up to one second for application GPU/presentation synchronization.
    // A timeout retains live resources; callers must exit or retry explicitly.
    [[nodiscard]] Result<void> drain();
    // Retains diagnostics/stats after close. Destruction fails fast if a final
    // bounded drain cannot prove resources safe to release.
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
