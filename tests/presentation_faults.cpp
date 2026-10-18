#include <shark/client/graphics.hpp>
#include <shark/time.hpp>

#include "engine/client/src/graphics_backend.hpp"
#include "engine/client/src/graphics_native_test.hpp"
#include "probes/graphics/report.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace shark::client;
using namespace shark::client::detail;
using shark::Error;
using shark::ErrorCode;
using shark::Result;
using shark::probe::Json;
using Object = Json::Object;

bool expected_termination = false;
const std::uint32_t* teardown_count = nullptr;

class TerminationObservation final {
  public:
    explicit TerminationObservation(const std::uint32_t& count) noexcept {
        expected_termination = false;
        teardown_count = &count;
    }
    ~TerminationObservation() noexcept {
        clear();
    }
    TerminationObservation(const TerminationObservation&) = delete;
    TerminationObservation& operator=(const TerminationObservation&) = delete;

    void expect() noexcept {
        expected_termination = true;
    }
    void clear() noexcept {
        expected_termination = false;
        teardown_count = nullptr;
    }
};

// Test observer only: production GraphicsDevice destruction calls terminate.
// Avoid OS crash dialogs and allocation/destruction inside that fatal handler.
[[noreturn]] void observe_termination() noexcept {
    std::printf("{\"kind\":\"terminated\",\"expected\":%s,\"teardown_calls\":%u}\n",
                expected_termination ? "true" : "false",
                expected_termination && teardown_count ? *teardown_count : 999U);
    std::fflush(stdout);
    std::_Exit(expected_termination ? 86 : 87);
}

void emit(Object value) {
    Json{std::move(value)}.write(std::cout);
    std::cout << '\n' << std::flush;
    if (!std::cout) {
        throw std::runtime_error("Cannot write fault qualification report");
    }
}
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <typename T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(result).value();
}
void wait_retry(shark::SteadyMonotonicClock& clock) {
    auto waited = clock.wait_for(std::chrono::milliseconds{5});
    if (!waited) {
        throw std::runtime_error(waited.error().message);
    }
}
Object error_json(const Error& error) {
    return {{"code", std::string(shark::error_code_name(error.code))},
            {"message", error.message},
            {"operation", error.context.operation}};
}
Object stats_json(const PresentationStats& stats) {
    return {{"submitted", stats.submitted},   {"completed", stats.completed},
            {"presented", stats.presented},   {"active", stats.active},
            {"closed", stats.closed},         {"faulted", stats.faulted},
            {"generation", stats.generation}, {"retired_generations", stats.retired_generations}};
}
#if SHARK_FAULT_NATIVE_GRAPHICS
Object resources_json(const GraphicsResourceSnapshot& resources) {
    return {{"live_swapchains", resources.live_swapchains},
            {"image_wrappers", resources.image_wrappers},
            {"image_views", resources.image_views},
            {"present_semaphores", resources.present_semaphores},
            {"present_fences", resources.present_fences},
            {"command_lists", resources.command_lists},
            {"acquire_semaphores", resources.acquire_semaphores},
            {"acquire_fences", resources.acquire_fences},
            {"pending_queue_workers", resources.pending_queue_workers},
            {"retained_recordings", resources.retained_recordings}};
}
Object diagnostics_json(GraphicsDevice& owner) {
    const auto diagnostics = take(owner.diagnostics());
    Json::Array messages;
    for (const auto& message : diagnostics.messages) {
        messages.emplace_back(message);
    }
    return {{"warnings", diagnostics.warnings},
            {"errors", diagnostics.errors},
            {"dropped", diagnostics.dropped},
            {"messages", std::move(messages)}};
}
#endif
void healthy_frame(GraphicsDevice& owner) {
    shark::SteadyMonotonicClock clock;
    const auto deadline = clock.now() + std::chrono::seconds{2};
    for (unsigned int attempt = 0; attempt < 128 && clock.now() < deadline; ++attempt) {
        static_cast<void>(take(owner.poll_window()));
        const auto report = take(owner.present());
        if (report.status == PresentationStatus::presented) {
            return;
        }
        wait_retry(clock);
    }
    throw std::runtime_error("Healthy pre-fault frame exceeded its 128-attempt bound");
}

class CpuBackend final : public GraphicsBackend {
  public:
    bool armed{false};
    std::uint32_t teardown{0};
    explicit CpuBackend(std::string_view fault) : fault_(fault) {}
    Result<void> create_window(const WindowConfig&) override {
        return {};
    }
    Result<void> create_instance(GraphicsValidation, GraphicsDiagnosticSink&) override {
        return {};
    }
    Result<void> create_surface() override {
        return {};
    }
    Result<std::vector<GraphicsAdapter>> enumerate_adapters() override {
        GraphicsAdapter adapter;
        adapter.uuid = "0123456789abcdef0123456789abcdef";
        adapter.name = "CPU fatal cleanup fixture";
        adapter.kind = AdapterKind::discrete;
        adapter.capabilities.features.fill(true);
        adapter.capabilities.formats.fill(true);
        adapter.capabilities.limits = {4096, 2048, 12, 4, 4, 16, 16, 16384, 134217728, 128};
        return std::vector{adapter};
    }
    Result<void> create_device(std::size_t) override {
        return {};
    }
    Result<void> create_nvrhi(GraphicsDiagnosticSink&) override {
        return {};
    }
    Result<WindowState> window_state() override {
        return WindowState{.content = {640, 400}, .framebuffer = {640, 400}, .visible = true};
    }
    Result<GraphicsWindowFrame> poll_window() override {
        return GraphicsWindowFrame{take(window_state())};
    }
    Result<PresentationSync> create_presentation() override {
        return PresentationSync::present_fence;
    }
    Result<std::optional<SwapchainInfo>> create_swapchain(WindowExtent extent) override {
        return std::optional{SwapchainInfo{extent, 3}};
    }
    Result<bool> wait_slot(std::uint32_t, std::uint64_t) override {
        return true;
    }
    Result<AcquiredImage> acquire(std::uint32_t, std::uint64_t) override {
        return AcquiredImage{AcquireStatus::ready, 0};
    }
    Result<void> submit(std::uint32_t, std::uint32_t, ClearColor) override {
        return {};
    }
    Result<PresentStatus> present(std::uint32_t, std::uint32_t, std::uint64_t) override {
        return PresentStatus::presented;
    }
    Result<DrainStatus> drain_presentation(std::uint64_t timeout) override {
        require(timeout <= 1'000'000'000, "Cleanup exceeded the finite deadline contract");
        if (!armed) {
            return DrainStatus::complete;
        }
        if (fault_ == "waiting") {
            return DrainStatus::waiting;
        }
        if (fault_ == "exception") {
            throw std::runtime_error("Injected CPU drain exception");
        }
        return Error{ErrorCode::execution_failed,
                     "Injected CPU drain error",
                     {.operation = "graphics.test.cpu_drain"}};
    }
    void destroy_swapchain() noexcept override {
        ++teardown;
    }
    void destroy_presentation() noexcept override {
        ++teardown;
    }
    void destroy_nvrhi() noexcept override {
        ++teardown;
    }
    void destroy_device() noexcept override {
        ++teardown;
    }
    void destroy_surface() noexcept override {
        ++teardown;
    }
    void destroy_instance() noexcept override {
        ++teardown;
    }
    void destroy_window() noexcept override {
        ++teardown;
    }

  private:
    std::string_view fault_;
};

int cpu_fault(std::string_view fault) {
    emit({{"kind", "identity"},
          {"schema_version", std::uint32_t{1}},
          {"mode", "cpu"},
          {"fault", std::string(fault)},
          {"device_accessed", false}});
    auto backend = std::make_unique<CpuBackend>(fault);
    auto* observed = backend.get();
    TerminationObservation observation{observed->teardown};
    auto owner = take(create_graphics({}, std::move(backend)));
    healthy_frame(*owner);
    observed->armed = true;
    const auto closed = owner->close();
    require(!closed, "Injected failed drain incorrectly closed successfully");
    const auto stats = take(owner->presentation_stats());
    require(stats.active && !stats.closed && observed->teardown == 0,
            "Failed drain released owners");
    emit({{"kind", "retained"},
          {"error", error_json(closed.error())},
          {"stats", stats_json(stats)},
          {"teardown_calls", observed->teardown}});
    observation.expect();
    owner.reset();
    observation.clear();
    return 88;
}

#if SHARK_FAULT_NATIVE_GRAPHICS
int native_fault(std::string_view fault, const std::string& uuid) {
    emit({{"kind", "identity"},
          {"schema_version", std::uint32_t{1}},
          {"mode", "native"},
          {"fault", std::string(fault)},
          {"device_accessed", true},
          {"selection", uuid},
          {"validation", "required"},
          {"dependency_lock_sha256", SHARK_GRAPHICS_LOCK_SHA256},
          {"source_sha256", SHARK_GRAPHICS_SOURCE_SHA256},
          {"compiler", SHARK_GRAPHICS_COMPILER}});
    NativeGraphicsTestControl control;
    TerminationObservation observation{control.teardown_calls};
    control.fault = fault == "recording-error"       ? NativeGraphicsFault::recording_error
                    : fault == "recording-exception" ? NativeGraphicsFault::recording_exception
                    : fault == "surface-error"       ? NativeGraphicsFault::surface_error
                                                     : NativeGraphicsFault::device_error;
    auto backend = make_fault_graphics_backend(control);
    auto* observed = backend.get();
    GraphicsConfig config;
    config.adapter_uuid = uuid;
    config.window.title = "Shark 05c fault qualification";
    config.window.content = {640, 400};
    auto owner = take(create_graphics(config, std::move(backend)));
    healthy_frame(*owner);
    const auto before = take(owner->presentation_stats());
    const auto adapter = take(owner->adapter());
    emit({{"kind", "healthy"},
          {"stats", stats_json(before)},
          {"adapter", Object{{"uuid", adapter.uuid},
                             {"name", adapter.name},
                             {"driver_name", adapter.driver_name},
                             {"driver_info", adapter.driver_info},
                             {"driver_version", adapter.driver_version}}},
          {"diagnostics", diagnostics_json(*owner)}});
    // Observe releases from the fault boundary; initial WSI negotiation may
    // have retired an unavailable candidate before the healthy frame.
    control.teardown_calls = 0;
    control.armed = true;
    shark::SteadyMonotonicClock clock;
    const auto deadline = clock.now() + std::chrono::seconds{2};
    auto failed = owner->present();
    for (unsigned int attempt = 1; failed && attempt < 128 && clock.now() < deadline; ++attempt) {
        wait_retry(clock);
        static_cast<void>(take(owner->poll_window()));
        failed = owner->present();
    }
    require(!failed && control.fired, "Native injected fault was not reached within 128 calls");
    const auto repeated = owner->present();
    require(!repeated && repeated.error().message == failed.error().message &&
                repeated.error().context.operation == failed.error().context.operation,
            "Repeated frame lost its original failure");
    const auto after = take(owner->presentation_stats());
    require(after.submitted == before.submitted && after.presented == before.presented &&
                after.faulted,
            "Faulted presentation submitted or presented additional work");
    emit({{"kind", "fault"},
          {"fired", control.fired},
          {"primary", error_json(failed.error())},
          {"repeated", error_json(repeated.error())},
          {"stats", stats_json(after)},
          {"resources", resources_json(observed->presentation_resources())},
          {"diagnostics", diagnostics_json(*owner)}});
    const auto closed = owner->close();
    require(!closed && closed.error().message == failed.error().message,
            "Cleanup erased the primary injected failure");
    const auto final = take(owner->presentation_stats());
    const bool fatal = fault == "recording-error" || fault == "recording-exception";
    emit({{"kind", fatal ? "retained" : "closed"},
          {"error", error_json(closed.error())},
          {"stats", stats_json(final)},
          {"resources", resources_json(observed->presentation_resources())},
          {"diagnostics", diagnostics_json(*owner)},
          {"teardown_calls", control.teardown_calls}});
    if (fatal) {
        require(final.active && !final.closed && control.teardown_calls == 0 &&
                    observed->presentation_resources().retained_recordings == 1,
                "An abandoned recording was released before process exit");
        observation.expect();
        owner.reset();
        observation.clear();
        return 88;
    }
    require(final.closed && !final.active && final.completed == final.submitted &&
                final.retired_generations == final.generation && control.teardown_calls == 7,
            "Injected policy error did not drain actual healthy-driver work");
    owner.reset();
    return 1; // Expected failure remains nonzero even though cleanup succeeded.
}
#endif
} // namespace

int main(int argc, char** argv) {
    std::set_terminate(observe_termination);
    try {
        if (argc == 2 &&
            (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "--describe")) {
            emit({{"kind", "description"},
                  {"schema_version", std::uint32_t{1}},
                  {"device_accessed", false},
                  {"native_compiled", SHARK_FAULT_NATIVE_GRAPHICS != 0},
                  {"qualification", "NOT_RUN"},
                  {"cpu_faults", Json::Array{"waiting", "error", "exception"}},
                  {"native_faults", Json::Array{"recording-error", "recording-exception",
                                                "surface-error", "device-error"}}});
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--cpu-fault") {
            const std::string_view fault(argv[2]);
            if (fault == "waiting" || fault == "error" || fault == "exception") {
                return cpu_fault(fault);
            }
        }
        if (argc == 5 && std::string_view(argv[1]) == "--native-fault" &&
            std::string_view(argv[3]) == "--device") {
            const std::string_view fault(argv[2]);
            const std::string uuid(argv[4]);
            if ((fault == "recording-error" || fault == "recording-exception" ||
                 fault == "surface-error" || fault == "device-error") &&
                valid_graphics_uuid(uuid)) {
#if SHARK_FAULT_NATIVE_GRAPHICS
                return native_fault(fault, uuid);
#else
                std::cerr << "Native fault qualification is unavailable in this CPU build\n";
                return 3;
#endif
            }
        }
        std::cerr
            << "Expected --describe, --help, --cpu-fault waiting|error|exception, or "
               "--native-fault recording-error|recording-exception|surface-error|device-error "
               "--device UUID\n";
        return 2;
    } catch (const std::exception& error) {
        emit({{"kind", "unexpected_failure"}, {"message", std::string(error.what())}});
        return 4;
    }
}
