#include <shark/client/graphics.hpp>

#include "graphics_backend.hpp"
#include "presentation.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <limits>
#include <thread>
#include <utility>

namespace shark::client {
namespace {
constexpr std::array<const char*, 8> feature_names{
    "Vulkan API 1.3",      "timelineSemaphore", "synchronization2",
    "dynamicRendering",    "VK_KHR_swapchain",  "one graphics+compute+present queue family",
    "sRGB surface format", "FIFO presentation"};
constexpr std::array<const char*, 4> format_names{
    "R8G8B8A8_SRGB combined optimal-tiling features/image usage",
    "R8G8B8A8_UNORM combined optimal-tiling features/image usage",
    "R16G16B16A16_SFLOAT combined optimal-tiling features/image usage",
    "D32_SFLOAT combined features/image usage and 2048x2048x12 depth array"};
constexpr std::array<std::uint32_t, 10> minimum_limits{4096, 2048, 12,    4,         4,
                                                       16,   16,   16384, 134217728, 128};
constexpr std::array<const char*, 10> limit_names{
    "maxImageDimension2D",           "maxImageDimensionCube",  "maxImageArrayLayers",
    "maxColorAttachments",           "maxBoundDescriptorSets", "maxPerStageDescriptorSampledImages",
    "maxPerStageDescriptorSamplers", "maxUniformBufferRange",  "maxStorageBufferRange",
    "maxComputeWorkGroupInvocations"};

Error graphics_error(ErrorCode code, const char* operation, std::string message) {
    return {code, std::move(message), {.operation = operation}};
}

Result<void> validate_config(const GraphicsConfig& config) {
    if ((!config.adapter_uuid.empty() && !valid_graphics_uuid(config.adapter_uuid)) ||
        (config.validation != GraphicsValidation::required &&
         config.validation != GraphicsValidation::off)) {
        return graphics_error(
            ErrorCode::invalid_argument, "graphics.configuration",
            "Expected a lowercase 32-hex adapter UUID and a valid validation mode");
    }
    return {};
}

bool valid_metadata(std::string_view text) noexcept {
    if (text.size() > 256) {
        return false;
    }
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<std::uint8_t>(text[index]);
        if (first == 0) {
            return false;
        }
        if (first < 0x80U) {
            ++index;
            continue;
        }
        std::size_t count = 0;
        std::uint32_t scalar = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xC2U && first <= 0xDFU) {
            count = 2;
            scalar = first & 0x1FU;
            minimum = 0x80U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            count = 3;
            scalar = first & 0x0FU;
            minimum = 0x800U;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            count = 4;
            scalar = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (count > text.size() - index) {
            return false;
        }
        for (std::size_t offset = 1; offset < count; ++offset) {
            const auto next = static_cast<std::uint8_t>(text[index + offset]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            scalar = (scalar << 6U) | (next & 0x3FU);
        }
        if (scalar < minimum || scalar > 0x10FFFFU || (scalar >= 0xD800U && scalar <= 0xDFFFU)) {
            return false;
        }
        index += count;
    }
    return true;
}

Result<void> validate_adapters(const std::vector<GraphicsAdapter>& adapters) {
    if (adapters.size() > 64) {
        return graphics_error(ErrorCode::execution_failed, "graphics.inventory",
                              "Adapter enumeration exceeds the 64-device bound");
    }
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        const auto& adapter = adapters[i];
        if (!valid_graphics_uuid(adapter.uuid) || adapter.name.empty() ||
            !valid_metadata(adapter.name) || !valid_metadata(adapter.driver_name) ||
            !valid_metadata(adapter.driver_info) ||
            (adapter.kind != AdapterKind::other && adapter.kind != AdapterKind::integrated &&
             adapter.kind != AdapterKind::discrete && adapter.kind != AdapterKind::software)) {
            return graphics_error(
                ErrorCode::execution_failed, "graphics.inventory",
                "Adapter metadata has an invalid identity, type or bounded string");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (adapters[j].uuid == adapter.uuid) {
                return graphics_error(ErrorCode::conflict, "graphics.inventory",
                                      "Adapter UUID is ambiguous: " + adapter.uuid);
            }
        }
    }
    return {};
}

Result<std::size_t> select_adapter(const std::vector<GraphicsAdapter>& adapters,
                                   const std::string& uuid) {
    std::optional<std::size_t> selected;
    const auto rank = [](AdapterKind kind) {
        return kind == AdapterKind::discrete ? 0 : kind == AdapterKind::integrated ? 1 : 2;
    };
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        const auto& adapter = adapters[i];
        if (!uuid.empty() && uuid != adapter.uuid) {
            continue;
        }
        auto missing = missing_graphics_requirements(adapter);
        if (!missing.empty()) {
            if (!uuid.empty()) {
                std::string message = "Selected adapter " + uuid + " is missing: ";
                for (const auto& requirement : missing) {
                    if (message.size() + requirement.size() + 2 > 1000) {
                        message += "...";
                        break;
                    }
                    message += requirement + "; ";
                }
                return graphics_error(ErrorCode::capability_unavailable, "graphics.capabilities",
                                      std::move(message));
            }
            continue;
        }
        if (!selected || rank(adapter.kind) < rank(adapters[*selected].kind) ||
            (rank(adapter.kind) == rank(adapters[*selected].kind) &&
             adapter.uuid < adapters[*selected].uuid)) {
            selected = i;
        }
    }
    if (!selected) {
        return graphics_error(ErrorCode::capability_unavailable, "graphics.selection",
                              uuid.empty() ? "No hardware adapter satisfies the graphics profile; "
                                             "inspect inventory requirements"
                                           : "Requested adapter UUID was not found: " + uuid);
    }
    return *selected;
}

std::uint32_t bounded_count(std::uint64_t count) noexcept {
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(count, std::numeric_limits<std::uint32_t>::max()));
}
} // namespace

bool valid_graphics_uuid(std::string_view uuid) noexcept {
    return uuid.size() == 32 && std::all_of(uuid.begin(), uuid.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

std::vector<std::string> missing_graphics_requirements(const GraphicsAdapter& adapter) {
    std::vector<std::string> missing;
    if (adapter.kind == AdapterKind::software) {
        missing.emplace_back("hardware GPU (software adapters are not supported)");
    }
    for (std::size_t i = 0; i < feature_names.size(); ++i) {
        if (!adapter.capabilities.features[i]) {
            missing.emplace_back(feature_names[i]);
        }
    }
    for (std::size_t i = 0; i < format_names.size(); ++i) {
        if (!adapter.capabilities.formats[i]) {
            missing.emplace_back(format_names[i]);
        }
    }
    for (std::size_t i = 0; i < minimum_limits.size(); ++i) {
        if (adapter.capabilities.limits[i] < minimum_limits[i]) {
            missing.emplace_back(std::string(limit_names[i]) +
                                 " >= " + std::to_string(minimum_limits[i]));
        }
    }
    return missing;
}

void detail::GraphicsDiagnosticSink::record(bool error, const char* message) noexcept {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    (error ? errors_ : warnings_).fetch_add(1, std::memory_order_relaxed);
    for (auto& slot : slots_) {
        bool expected = false;
        if (!slot.claimed.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
            continue;
        }
        const char* text = message ? message : "(no message)";
        std::size_t count = 0;
        while (count < slot.text.size() - 1 && text[count] != '\0') {
            slot.text[count] = text[count];
            ++count;
        }
        slot.text[count] = '\0';
        if (count == slot.text.size() - 1 && text[count] != '\0') {
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        slot.ready.store(true, std::memory_order_release);
        return;
    }
    dropped_.fetch_add(1, std::memory_order_relaxed);
}

GraphicsDiagnostics detail::GraphicsDiagnosticSink::snapshot() const {
    GraphicsDiagnostics result;
    result.warnings = bounded_count(warnings_.load(std::memory_order_relaxed));
    result.errors = bounded_count(errors_.load(std::memory_order_relaxed));
    result.dropped = bounded_count(dropped_.load(std::memory_order_relaxed));
    for (const auto& slot : slots_) {
        if (slot.ready.load(std::memory_order_acquire)) {
            // Diagnostics are native byte strings. Escape non-ASCII/control
            // bytes into readable hex outside callbacks to keep owned messages
            // valid UTF-8 even when a native prefix ends inside a code point.
            std::string message;
            constexpr char hex[] = "0123456789abcdef";
            for (char c : slot.text) {
                if (c == '\0') {
                    break;
                }
                const auto byte = static_cast<unsigned char>(c);
                if (byte >= 32 && byte < 127) {
                    message.push_back(c);
                } else {
                    message += "\\x";
                    message.push_back(hex[byte >> 4]);
                    message.push_back(hex[byte & 15]);
                }
            }
            result.messages.push_back(std::move(message));
        }
    }
    return result;
}

struct GraphicsDevice::Impl {
    detail::GraphicsDiagnosticSink sink;
    std::unique_ptr<detail::GraphicsBackend> backend;
    detail::Presentation presentation;
    std::thread::id thread{std::this_thread::get_id()};
    GraphicsAdapter selected;
    std::array<bool, 5> attempted{};
    const char* operation{"graphics.startup"};

    explicit Impl(std::unique_ptr<detail::GraphicsBackend> value)
        : backend(std::move(value)), presentation(*backend, sink) {}
    ~Impl() noexcept {
        const auto closed = presentation.close();
        if (!presentation.safe_to_destroy()) {
            // The caller may be unwinding before it can print its primary
            // error. Publish bounded context before fail-fast process teardown.
            std::fputs("Shark graphics cleanup cannot safely release live resources: ", stderr);
            if (!closed) {
                const auto& message = closed.error().message;
                std::fwrite(message.data(), 1, std::min(message.size(), std::size_t{1024}), stderr);
            }
            std::fputs("\nTerminating with GPU owners retained.\n", stderr);
            std::fflush(stderr);
            std::terminate();
        }
        unwind();
    }
    bool live() const noexcept {
        return std::any_of(attempted.begin(), attempted.end(), [](bool value) { return value; });
    }
    void unwind() noexcept {
        if (attempted[4]) {
            backend->destroy_nvrhi();
            attempted[4] = false;
        }
        if (attempted[3]) {
            backend->destroy_device();
            attempted[3] = false;
        }
        if (attempted[2]) {
            backend->destroy_surface();
            attempted[2] = false;
        }
        if (attempted[1]) {
            backend->destroy_instance();
            attempted[1] = false;
        }
        if (attempted[0]) {
            backend->destroy_window();
            attempted[0] = false;
        }
    }
    Result<void> check_thread() const {
        if (std::this_thread::get_id() != thread) {
            return graphics_error(ErrorCode::conflict, "graphics.thread",
                                  "Graphics calls require the creating main thread");
        }
        return {};
    }
    Result<void> check_live() const {
        if (auto checked = check_thread(); !checked) {
            return checked;
        }
        if (!live()) {
            return graphics_error(ErrorCode::conflict, "graphics.closed",
                                  "Graphics device is closed");
        }
        return {};
    }
    Result<void> command(detail::WindowCommand command, WindowExtent extent = {}) {
        if (auto checked = check_live(); !checked) {
            return checked;
        }
        try {
            return backend->window_command(command, extent);
        } catch (...) {
            return graphics_error(ErrorCode::execution_failed, "graphics.window.command",
                                  "Window command threw");
        }
    }
    Result<void> check_diagnostics() const {
        auto report = sink.snapshot();
        if (report.errors != 0) {
            auto message =
                "Graphics validation reported " + std::to_string(report.errors) + " error(s)";
            if (!report.messages.empty()) {
                message += ": " + report.messages.front().substr(0, 768);
            }
            return graphics_error(ErrorCode::execution_failed, "graphics.validation",
                                  std::move(message));
        }
        return {};
    }
    Error fail(Error error) {
        const auto before = sink.snapshot().errors;
        unwind();
        const auto report = sink.snapshot();
        if (report.errors > before) {
            if (error.message.size() > 640) {
                std::size_t end = 640;
                while (end != 0 &&
                       (static_cast<unsigned char>(error.message[end]) & 0xC0U) == 0x80U) {
                    --end;
                }
                error.message.resize(end);
            }
            error.message +=
                "; teardown validation errors: " + std::to_string(report.errors - before);
            if (!report.messages.empty()) {
                error.message += ": " + report.messages.back().substr(0, 256);
            }
        }
        return error;
    }
    template <class Action> Result<void> stage(std::size_t index, const char* name, Action action) {
        operation = name;
        attempted[index] = true;
        auto result = action();
        return result ? check_diagnostics() : result;
    }
    Result<std::vector<GraphicsAdapter>> prepare(const GraphicsConfig& config) {
        if (auto value =
                stage(0, "graphics.window", [&] { return backend->create_window(config.window); });
            !value) {
            return value.error();
        }
        if (auto value = stage(1, "graphics.instance",
                               [&] { return backend->create_instance(config.validation, sink); });
            !value) {
            return value.error();
        }
        if (auto value = stage(2, "graphics.surface", [&] { return backend->create_surface(); });
            !value) {
            return value.error();
        }
        operation = "graphics.inventory";
        auto adapters = backend->enumerate_adapters();
        if (!adapters) {
            return adapters.error();
        }
        if (auto value = validate_adapters(adapters.value()); !value) {
            return value.error();
        }
        if (auto value = check_diagnostics(); !value) {
            return value.error();
        }
        return adapters;
    }
};

GraphicsDevice::GraphicsDevice(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GraphicsDevice::~GraphicsDevice() noexcept {
    if (impl_->live() && impl_->thread != std::this_thread::get_id()) {
        std::terminate();
    }
}

Result<std::unique_ptr<GraphicsDevice>> GraphicsDevice::create(const GraphicsConfig& config) {
    return detail::create_graphics(config, detail::make_graphics_backend());
}

Result<std::unique_ptr<GraphicsDevice>>
detail::create_graphics(const GraphicsConfig& config, std::unique_ptr<GraphicsBackend> backend) {
    if (auto valid = validate_config(config); !valid) {
        return valid.error();
    }
    if (!backend) {
        return graphics_error(ErrorCode::capability_unavailable, "graphics.backend",
                              "Native graphics backend is unavailable");
    }
    auto impl = std::make_unique<GraphicsDevice::Impl>(std::move(backend));
    try {
        auto adapters = impl->prepare(config);
        if (!adapters) {
            return impl->fail(adapters.error());
        }
        auto selected = select_adapter(adapters.value(), config.adapter_uuid);
        if (!selected) {
            return impl->fail(selected.error());
        }
        impl->selected = adapters.value()[selected.value()];
        if (auto value =
                impl->stage(3, "graphics.device",
                            [&] { return impl->backend->create_device(selected.value()); });
            !value) {
            return impl->fail(value.error());
        }
        if (auto value = impl->stage(4, "graphics.nvrhi",
                                     [&] { return impl->backend->create_nvrhi(impl->sink); });
            !value) {
            return impl->fail(value.error());
        }
        return std::unique_ptr<GraphicsDevice>{new GraphicsDevice{std::move(impl)}};
    } catch (...) {
        return impl->fail(
            graphics_error(ErrorCode::execution_failed, impl->operation,
                           "Graphics startup threw an exception; acquired resources are unwound"));
    }
}

Result<std::vector<GraphicsAdapter>> GraphicsDevice::inventory(const GraphicsConfig& config) {
    return detail::inventory_graphics(config, detail::make_graphics_backend());
}

Result<std::vector<GraphicsAdapter>>
detail::inventory_graphics(const GraphicsConfig& config, std::unique_ptr<GraphicsBackend> backend) {
    if (auto valid = validate_config(config); !valid) {
        return valid.error();
    }
    if (!config.adapter_uuid.empty()) {
        return graphics_error(ErrorCode::invalid_argument, "graphics.inventory.configuration",
                              "Inventory does not accept an adapter selector");
    }
    if (!backend) {
        return graphics_error(ErrorCode::capability_unavailable, "graphics.backend",
                              "Native graphics backend is unavailable");
    }
    // Inventory uses the same ownership/cleanup path without a logical device.
    auto impl = std::make_unique<GraphicsDevice::Impl>(std::move(backend));
    try {
        auto hidden = config;
        hidden.window.visible = false;
        auto adapters = impl->prepare(hidden);
        if (!adapters) {
            return impl->fail(adapters.error());
        }
        impl->unwind();
        if (auto value = impl->check_diagnostics(); !value) {
            return value.error();
        }
        return adapters;
    } catch (...) {
        return impl->fail(graphics_error(
            ErrorCode::execution_failed, impl->operation,
            "Graphics inventory threw an exception; acquired resources are unwound"));
    }
}

Result<GraphicsAdapter> GraphicsDevice::adapter() const {
    if (auto value = impl_->check_thread(); !value) {
        return value.error();
    }
    return impl_->selected;
}
Result<GraphicsDiagnostics> GraphicsDevice::diagnostics() const {
    if (auto value = impl_->check_thread(); !value) {
        return value.error();
    }
    return impl_->sink.snapshot();
}
Result<void> GraphicsDevice::close() {
    if (auto value = impl_->check_thread(); !value) {
        return value.error();
    }
    auto closed = impl_->presentation.close();
    if (!impl_->presentation.safe_to_destroy()) {
        return closed;
    }
    impl_->unwind();
    if (!closed) {
        return closed;
    }
    return impl_->check_diagnostics();
}

Result<GraphicsWindowFrame> GraphicsDevice::poll_window() {
    if (auto value = impl_->check_live(); !value) {
        return value.error();
    }
    try {
        return impl_->backend->poll_window();
    } catch (...) {
        return graphics_error(ErrorCode::execution_failed, "graphics.window.poll",
                              "Window poll threw");
    }
}
Result<WindowState> GraphicsDevice::window_state() const {
    if (auto value = impl_->check_live(); !value) {
        return value.error();
    }
    try {
        return impl_->backend->window_state();
    } catch (...) {
        return graphics_error(ErrorCode::execution_failed, "graphics.window.state",
                              "Window query threw");
    }
}
Result<void> GraphicsDevice::resize(WindowExtent content) {
    return impl_->command(detail::WindowCommand::resize, content);
}
Result<void> GraphicsDevice::iconify() {
    return impl_->command(detail::WindowCommand::iconify);
}
Result<void> GraphicsDevice::restore() {
    return impl_->command(detail::WindowCommand::restore);
}
Result<void> GraphicsDevice::show() {
    return impl_->command(detail::WindowCommand::show);
}
Result<void> GraphicsDevice::focus() {
    return impl_->command(detail::WindowCommand::focus);
}
Result<void> GraphicsDevice::request_close() {
    return impl_->command(detail::WindowCommand::request_close);
}
Result<PresentationReport> GraphicsDevice::present(ClearColor color) {
    if (auto value = impl_->check_live(); !value) {
        return value.error();
    }
    if (impl_->backend->offscreen_pending()) {
        return graphics_error(ErrorCode::conflict, "graphics.presentation.offscreen_pending",
                              "Offscreen resources require an explicit drain before presentation");
    }
    return impl_->presentation.frame(color);
}
Result<PresentationStats> GraphicsDevice::presentation_stats() const {
    if (auto value = impl_->check_thread(); !value) {
        return value.error();
    }
    return impl_->presentation.stats();
}
Result<void> GraphicsDevice::drain() {
    if (auto value = impl_->check_thread(); !value) {
        return value.error();
    }
    return impl_->presentation.drain();
}

Result<OffscreenReadback> detail::GraphicsBackend::render_offscreen(const OffscreenRequest&) {
    return graphics_error(ErrorCode::capability_unavailable, "graphics.offscreen.backend",
                          "Offscreen rendering backend is unavailable");
}
Result<OffscreenReadback> GraphicsDevice::render_offscreen(const OffscreenRequest& request) {
    if (auto checked = impl_->check_live(); !checked) {
        return checked.error();
    }
    if (auto checked = validate_offscreen_request(request); !checked) {
        return checked.error();
    }
    if (impl_->presentation.stats().faulted) {
        return graphics_error(ErrorCode::conflict, "graphics.offscreen.device",
                              "Presentation fault must be resolved before offscreen rendering");
    }
    try {
        if (auto checked = impl_->check_diagnostics(); !checked) {
            return checked.error();
        }
        auto result = impl_->backend->render_offscreen(request);
        if (!result) {
            return result.error();
        }
        if (auto checked = impl_->check_diagnostics(); !checked) {
            return checked.error();
        }
        return result;
    } catch (...) {
        return graphics_error(
            ErrorCode::execution_failed, "graphics.offscreen.render",
            "Offscreen rendering threw; GPU resources retain ownership until safe cleanup");
    }
}

} // namespace shark::client
