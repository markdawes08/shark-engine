#include <shark/client/graphics.hpp>
#include <shark/time.hpp>

#include "graphics_backend.hpp"
#include "probes/graphics/report.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace shark::client;
using shark::probe::Json;
using Array = Json::Array;
using Object = Json::Object;
constexpr std::uint32_t resize_count = 100;
constexpr std::uint32_t minimize_count = 20;
constexpr std::uint32_t lifetime_count = 20;
constexpr std::uint32_t lifetime_warmup = 5;
constexpr std::uint32_t lifetime_frames = 16;
constexpr std::uint32_t active_warmup_frames = 32;
constexpr std::uint32_t attempt_limit = 16;
constexpr std::uint64_t handle_allowance = 8;
constexpr std::array sizes{WindowExtent{700, 420}, WindowExtent{530, 350}, WindowExtent{800, 450},
                           WindowExtent{640, 400}};

class Failure final : public std::exception {
  public:
    explicit Failure(shark::Error error) : error_(std::move(error)) {}
    const char* what() const noexcept override {
        return error_.message.c_str();
    }
    const shark::Error& error() const noexcept {
        return error_;
    }

  private:
    shark::Error error_;
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw Failure(
            {shark::ErrorCode::execution_failed, message, {.operation = "presentation-stress"}});
    }
}

template <typename T> T take(shark::Result<T> result) {
    if (!result)
        throw Failure(std::move(result).error());
    return std::move(result).value();
}
void check(shark::Result<void> result) {
    if (!result)
        throw Failure(std::move(result).error());
}

void emit(Object record) {
    Json{std::move(record)}.write(std::cout);
    std::cout << '\n' << std::flush;
    if (!std::cout)
        throw std::runtime_error("Cannot write presentation stress report");
}

Object error_json(const shark::Error& error) {
    return {{"code", std::string(shark::error_code_name(error.code))},
            {"message", error.message},
            {"operation", error.context.operation}};
}
Object extent_json(WindowExtent extent) {
    return {{"width", extent.width}, {"height", extent.height}};
}
Object window_json(const WindowState& state) {
    return {{"content", extent_json(state.content)},
            {"framebuffer", extent_json(state.framebuffer)},
            {"visible", state.visible},
            {"iconified", state.iconified},
            {"close_requested", state.close_requested}};
}
Object stats_json(const PresentationStats& stats) {
    return {{"synchronization", stats.synchronization == PresentationSync::present_fence
                                    ? "present_fence"
                                    : "host_completion"},
            {"extent", extent_json(stats.extent)},
            {"image_count", stats.image_count},
            {"frame_limit", stats.frame_limit},
            {"in_flight", stats.in_flight},
            {"peak_in_flight", stats.peak_in_flight},
            {"submitted", stats.submitted},
            {"completed", stats.completed},
            {"presented", stats.presented},
            {"generation", stats.generation},
            {"retired_generations", stats.retired_generations},
            {"suspended_frames", stats.suspended_frames},
            {"waiting_frames", stats.waiting_frames},
            {"active", stats.active},
            {"faulted", stats.faulted},
            {"closed", stats.closed}};
}
std::string serialized_stats(const PresentationStats& stats) {
    std::ostringstream stream;
    Json{stats_json(stats)}.write(stream);
    return stream.str();
}
Array strings(const std::vector<std::string>& values) {
    Array result;
    for (const auto& value : values)
        result.emplace_back(value);
    return result;
}
Object diagnostics_json(const GraphicsDiagnostics& diagnostics) {
    return {{"warnings", diagnostics.warnings},
            {"errors", diagnostics.errors},
            {"dropped", diagnostics.dropped},
            {"messages", strings(diagnostics.messages)}};
}
bool clean(const GraphicsDiagnostics& diagnostics) {
    return diagnostics.warnings == 0 && diagnostics.errors == 0 && diagnostics.dropped == 0 &&
           diagnostics.messages.empty();
}
template <typename T, std::size_t Size>
Object named_values(const std::array<const char*, Size>& names, const std::array<T, Size>& values) {
    Object result;
    for (std::size_t index = 0; index < Size; ++index)
        result.emplace(names[index], values[index]);
    return result;
}
const char* adapter_kind(AdapterKind kind) {
    switch (kind) {
    case AdapterKind::other:
        return "other";
    case AdapterKind::integrated:
        return "integrated";
    case AdapterKind::discrete:
        return "discrete";
    case AdapterKind::software:
        return "software";
    }
    return "unknown";
}
Object adapter_json(const GraphicsAdapter& adapter) {
    constexpr std::array features{"vulkan_1_3",          "timeline_semaphore",
                                  "synchronization2",    "dynamic_rendering",
                                  "swapchain_extension", "graphics_compute_present_queue",
                                  "srgb_surface",        "fifo_present"};
    constexpr std::array formats{"rgba8_srgb", "rgba8_unorm", "rgba16_float",
                                 "d32_float_depth_array"};
    constexpr std::array limits{
        "maxImageDimension2D",           "maxImageDimensionCube",
        "maxImageArrayLayers",           "maxColorAttachments",
        "maxBoundDescriptorSets",        "maxPerStageDescriptorSampledImages",
        "maxPerStageDescriptorSamplers", "maxUniformBufferRange",
        "maxStorageBufferRange",         "maxComputeWorkGroupInvocations"};
    return {{"uuid", adapter.uuid},
            {"name", adapter.name},
            {"driver_name", adapter.driver_name},
            {"driver_info", adapter.driver_info},
            {"vendor_id", adapter.vendor_id},
            {"device_id", adapter.device_id},
            {"driver_version", adapter.driver_version},
            {"api_version", adapter.api_version},
            {"queue_family", adapter.queue_family},
            {"kind", adapter_kind(adapter.kind)},
            {"features", named_values(features, adapter.capabilities.features)},
            {"formats", named_values(formats, adapter.capabilities.formats)},
            {"limits", named_values(limits, adapter.capabilities.limits)},
            {"missing_requirements", strings(missing_graphics_requirements(adapter))}};
}

struct ProcessResources {
    std::uint64_t private_bytes{0};
    std::uint64_t handles{0};
    std::uint64_t gdi_objects{0};
    std::uint64_t user_objects{0};
};
std::uint64_t gui_resources(DWORD flags) {
    SetLastError(ERROR_SUCCESS);
    const auto count = GetGuiResources(GetCurrentProcess(), flags);
    require(count != 0 || GetLastError() == ERROR_SUCCESS,
            "Cannot measure Windows GUI resource count");
    return count;
}
ProcessResources process_resources() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    DWORD handles = 0;
    require(K32GetProcessMemoryInfo(GetCurrentProcess(),
                                    reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                    sizeof(memory)) != FALSE &&
                GetProcessHandleCount(GetCurrentProcess(), &handles) != FALSE,
            "Cannot measure Windows private bytes and process handle count");
    require(memory.PrivateUsage > 0 && handles > 0, "Windows resource counters are empty");
    return {static_cast<std::uint64_t>(memory.PrivateUsage), handles, gui_resources(GR_GDIOBJECTS),
            gui_resources(GR_USEROBJECTS)};
}
Object process_json(ProcessResources resources) {
    return {{"private_bytes", resources.private_bytes},
            {"handles", resources.handles},
            {"gdi_objects", resources.gdi_objects},
            {"user_objects", resources.user_objects}};
}
Object process_inventory() {
    const DWORD process_id = GetCurrentProcessId();
    Array threads;
    const HANDLE handle = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    require(handle != INVALID_HANDLE_VALUE, "Cannot create bounded process thread inventory");
    std::unique_ptr<void, decltype(&CloseHandle)> snapshot(handle, CloseHandle);
    THREADENTRY32 thread{};
    thread.dwSize = sizeof(thread);
    bool thread_overflow = false;
    const bool queried = Thread32First(snapshot.get(), &thread) != FALSE;
    if (queried) {
        do {
            if (thread.th32OwnerProcessID == process_id) {
                if (threads.size() == 512) {
                    thread_overflow = true;
                    break;
                }
                threads.emplace_back(static_cast<std::uint32_t>(thread.th32ThreadID));
            }
        } while (Thread32Next(snapshot.get(), &thread) != FALSE);
    }
    const bool closed = CloseHandle(snapshot.release()) != FALSE;
    require(queried && closed && !thread_overflow,
            "Process thread inventory failed or exceeded its fixed 512-thread bound");
    std::array<HMODULE, 1024> modules{};
    DWORD bytes = 0;
    require(K32EnumProcessModules(GetCurrentProcess(), modules.data(), sizeof(modules), &bytes) !=
                    FALSE &&
                bytes <= sizeof(modules) && bytes % sizeof(HMODULE) == 0,
            "Process module inventory failed or exceeded its fixed 1024-module bound");
    Array names;
    const auto count = static_cast<std::size_t>(bytes) / sizeof(HMODULE);
    for (std::size_t index = 0; index < count; ++index) {
        std::array<char, 512> name{};
        const auto length = K32GetModuleBaseNameA(GetCurrentProcess(), modules[index], name.data(),
                                                  static_cast<DWORD>(name.size()));
        require(length != 0 && length < name.size(),
                "Process module basename is absent or oversized");
        names.emplace_back(std::string{name.data(), length});
    }
    return {{"thread_count", static_cast<std::uint64_t>(threads.size())},
            {"thread_ids", std::move(threads)},
            {"module_count", static_cast<std::uint64_t>(count)},
            {"module_names", std::move(names)},
            {"gdi_objects",
             static_cast<std::uint32_t>(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS))},
            {"user_objects",
             static_cast<std::uint32_t>(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS))},
            {"snapshot_closed_before_measurement", true}};
}
void diagnostic_process_stage(std::string_view phase, std::uint32_t index) {
    const auto inventory = process_inventory();
    const auto resources = process_resources();
    emit({{"kind", "process_stage"},
          {"diagnostic", true},
          {"phase", std::string(phase)},
          {"index", index},
          {"process_inventory", inventory},
          {"process", process_json(resources)}});
}
const char* awareness_name(DPI_AWARENESS awareness) noexcept {
    switch (awareness) {
    case DPI_AWARENESS_UNAWARE:
        return "unaware";
    case DPI_AWARENESS_SYSTEM_AWARE:
        return "system";
    case DPI_AWARENESS_PER_MONITOR_AWARE:
        return "per_monitor";
    default:
        return "invalid";
    }
}
BOOL CALLBACK find_stress_window(HWND window, LPARAM context) noexcept {
    wchar_t title[128]{};
    if (GetWindowTextW(window, title, 128) > 0 &&
        std::wstring_view{title} == L"Shark 05c presentation stress") {
        *reinterpret_cast<HWND*>(context) = window;
        return FALSE;
    }
    return TRUE;
}
Object native_window_json(WindowExtent requested = {}) {
    const auto thread_context = GetThreadDpiAwarenessContext();
    Object result{
        {"thread_awareness", awareness_name(GetAwarenessFromDpiAwarenessContext(thread_context))},
        {"thread_per_monitor_v2",
         AreDpiAwarenessContextsEqual(thread_context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) !=
             FALSE},
        {"system_dpi", GetDpiForSystem()}};
    HWND window = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), find_stress_window, reinterpret_cast<LPARAM>(&window));
    result.emplace("available", window != nullptr);
    if (window == nullptr)
        return result;
    const auto window_context = GetWindowDpiAwarenessContext(window);
    const auto dpi = GetDpiForWindow(window);
    const auto style = static_cast<std::uint32_t>(GetWindowLongPtrW(window, GWL_STYLE));
    const auto ex_style = static_cast<std::uint32_t>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    result.emplace("window_awareness",
                   awareness_name(GetAwarenessFromDpiAwarenessContext(window_context)));
    result.emplace("window_per_monitor_v2",
                   AreDpiAwarenessContextsEqual(
                       window_context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE);
    result.emplace("window_dpi", dpi);
    result.emplace("style", style);
    result.emplace("ex_style", ex_style);
    RECT client{}, outer{};
    const bool measured =
        GetClientRect(window, &client) != FALSE && GetWindowRect(window, &outer) != FALSE;
    result.emplace("rectangles_available", measured);
    const auto rectangle_extent = [](const RECT& rectangle) {
        return extent_json({static_cast<std::uint32_t>(rectangle.right - rectangle.left),
                            static_cast<std::uint32_t>(rectangle.bottom - rectangle.top)});
    };
    if (measured) {
        result.emplace("client", rectangle_extent(client));
        result.emplace("outer", rectangle_extent(outer));
    }
    if (requested.width != 0 && requested.height != 0 && dpi != 0) {
        RECT actual{0, 0, static_cast<LONG>(requested.width), static_cast<LONG>(requested.height)};
        RECT glfw = actual;
        constexpr DWORD glfw_style = WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_SYSMENU |
                                     WS_MINIMIZEBOX | WS_CAPTION | WS_MAXIMIZEBOX | WS_THICKFRAME;
        if (AdjustWindowRectExForDpi(&actual, style, FALSE, ex_style, dpi) != FALSE)
            result.emplace("actual_style_expected_outer", rectangle_extent(actual));
        if (AdjustWindowRectExForDpi(&glfw, glfw_style, FALSE, WS_EX_APPWINDOW, dpi) != FALSE)
            result.emplace("glfw_style_expected_outer", rectangle_extent(glfw));
        result.emplace("requested", extent_json(requested));
    }
    return result;
}
Object resources_json(const detail::GraphicsResourceSnapshot& snapshot) {
    return {{"live_swapchains", snapshot.live_swapchains},
            {"image_wrappers", snapshot.image_wrappers},
            {"image_views", snapshot.image_views},
            {"present_semaphores", snapshot.present_semaphores},
            {"present_fences", snapshot.present_fences},
            {"command_lists", snapshot.command_lists},
            {"acquire_semaphores", snapshot.acquire_semaphores},
            {"acquire_fences", snapshot.acquire_fences},
            {"pending_queue_workers", snapshot.pending_queue_workers},
            {"retained_recordings", snapshot.retained_recordings}};
}
void check_resources(const detail::GraphicsResourceSnapshot& snapshot,
                     const PresentationStats& stats, bool closed) {
    if (closed) {
        require(snapshot.live_swapchains == 0 && snapshot.image_wrappers == 0 &&
                    snapshot.image_views == 0 && snapshot.present_semaphores == 0 &&
                    snapshot.present_fences == 0 && snapshot.command_lists == 0 &&
                    snapshot.acquire_semaphores == 0 && snapshot.acquire_fences == 0 &&
                    snapshot.pending_queue_workers == 0 && snapshot.retained_recordings == 0,
                "Closed device retains engine-owned presentation resources");
        return;
    }
    require(snapshot.live_swapchains == 1 && snapshot.image_wrappers == stats.image_count &&
                snapshot.image_views == stats.image_count && stats.image_count >= 1 &&
                stats.image_count <= 8 && snapshot.command_lists == 2 &&
                snapshot.acquire_semaphores == 2 && snapshot.acquire_fences == 2 &&
                snapshot.pending_queue_workers == 0 && snapshot.retained_recordings == 0,
            "Active engine-owned presentation resources exceed their exact owner bounds");
    const auto expected =
        stats.synchronization == PresentationSync::present_fence ? stats.image_count : 0;
    require(snapshot.present_semaphores == expected && snapshot.present_fences == expected,
            "Present synchronization resource count disagrees with the selected strategy");
}

void check_stats(const PresentationStats& stats) {
    require(!stats.faulted && stats.completed <= stats.submitted &&
                stats.presented <= stats.submitted && stats.frame_limit >= 1 &&
                stats.frame_limit <= 2 && stats.in_flight <= stats.frame_limit &&
                stats.peak_in_flight <= stats.frame_limit &&
                stats.submitted - stats.completed <= stats.frame_limit &&
                stats.retired_generations <= stats.generation,
            "Presentation counters or frame bounds are inconsistent");
    if (stats.active)
        require(stats.retired_generations + 1 == stats.generation,
                "An old swapchain generation remains owned after recreation");
}
WindowState poll(GraphicsDevice& device) {
    const auto frame = take(device.poll_window());
    require(frame.diagnostics.rejected_input_events == 0 &&
                frame.diagnostics.invalid_window_events == 0 && !frame.diagnostics.input_error &&
                !frame.diagnostics.window_error,
            "Native window callback diagnostics must remain clean");
    require(!frame.state.close_requested, "A close request leaves the workload incomplete");
    return frame.state;
}
template <typename Predicate>
WindowState await_state(GraphicsDevice& device, Predicate predicate, std::string_view phase,
                        std::uint32_t index, WindowExtent expected, Object context = {}) {
    shark::SteadyMonotonicClock clock;
    const auto start = clock.now();
    const auto deadline = start + std::chrono::seconds{2};
    auto last = take(device.window_state());
    std::uint32_t observations = 0;
    while (clock.now() < deadline) {
        const auto state = poll(device);
        last = state;
        ++observations;
        if (predicate(state))
            return state;
        check(clock.wait_for(std::chrono::milliseconds{5}));
    }
    emit({{"kind", "window_timeout"},
          {"status", "FAIL"},
          {"phase", std::string(phase)},
          {"index", index},
          {"expected_content", extent_json(expected)},
          {"deadline_ms", 2000U},
          {"elapsed_ms",
           static_cast<std::uint64_t>(
               std::chrono::duration_cast<std::chrono::milliseconds>(clock.now() - start).count())},
          {"observations", observations},
          {"window", window_json(last)},
          {"focused", last.focused},
          {"scale_x", static_cast<double>(last.scale.x)},
          {"scale_y", static_cast<double>(last.scale.y)},
          {"native_window", native_window_json(expected)},
          {"context", std::move(context)},
          {"stats", stats_json(take(device.presentation_stats()))}});
    throw Failure({shark::ErrorCode::execution_failed,
                   "Native window state deadline expired",
                   {.operation = "presentation-stress.window"}});
}
void frames(GraphicsDevice& device, std::uint32_t count) {
    shark::SteadyMonotonicClock clock;
    for (std::uint32_t frame = 0; frame < count; ++frame) {
        const auto frame_start = clock.now();
        bool completed = false;
        for (std::uint32_t attempt = 0; attempt < attempt_limit; ++attempt) {
            const auto window = poll(device);
            require(!window.iconified, "Window was minimized outside the declared workload");
            const auto before = take(device.presentation_stats());
            const auto report = take(device.present());
            check_stats(report.stats);
            require(report.stats.submitted >= before.submitted &&
                        report.stats.completed >= before.completed &&
                        report.stats.generation >= before.generation,
                    "Presentation counters must not regress");
            if (report.status == PresentationStatus::presented) {
                require(report.stats.presented == before.presented + 1 &&
                            report.stats.extent == window.framebuffer,
                        "Presented frame must advance its counter and match the framebuffer");
                completed = true;
                break;
            }
            require(report.status == PresentationStatus::waiting ||
                        report.status == PresentationStatus::recreate_pending,
                    "Visible presentation unexpectedly stopped or suspended");
            check(clock.wait_for(std::chrono::milliseconds{5}));
        }
        require(completed, "Presentation exhausted the declared 16-attempt frame budget");
        require(clock.now() - frame_start <= std::chrono::seconds{5},
                "One frame exceeded the declared five-second progress deadline");
    }
}

struct Options {
    std::string_view mode{"describe"};
    std::string_view profile{SHARK_GRAPHICS_PROFILE};
    bool diagnose_resource_growth{false};
    GraphicsConfig config;
    std::uint32_t seconds() const noexcept {
        return profile == "release" ? 600 : 60;
    }
    std::uint64_t byte_allowance() const noexcept {
        return (profile == "asan" ? 256ULL : 64ULL) * 1024 * 1024;
    }
};
Options parse(int argc, char* argv[]) {
    Options options;
    options.config.window.title = "Shark 05c presentation stress";
    options.config.window.content = {640, 400};
    const auto invalid = [] {
        throw Failure({shark::ErrorCode::invalid_argument,
                       "Use --help, --describe, or --stress [--profile debug|release|asan] "
                       "[--device auto|<32 lowercase hex UUID>] [--validation required|off] "
                       "[--diagnose-resource-growth]",
                       {.operation = "presentation-stress.arguments"}});
    };
    if (argc > 9)
        invalid();
    if (argc > 1) {
        const std::string_view mode{argv[1]};
        if (mode == "--help" || mode == "--describe" || mode == "--stress")
            options.mode = mode.substr(2);
        else
            invalid();
    }
    bool profile_seen = false;
    bool device_seen = false;
    bool validation_seen = false;
    for (int index = 2; index < argc;) {
        if (options.mode != "stress")
            invalid();
        const std::string_view option{argv[index]};
        if (option == "--diagnose-resource-growth" && !options.diagnose_resource_growth) {
            options.diagnose_resource_growth = true;
            ++index;
            continue;
        }
        if (index + 1 >= argc)
            invalid();
        const std::string_view value{argv[index + 1]};
        if (option == "--profile" && !profile_seen && value == SHARK_GRAPHICS_PROFILE) {
            profile_seen = true;
            options.profile = value;
        } else if (option == "--device" && !device_seen) {
            device_seen = true;
            if (value != "auto") {
                if (!valid_graphics_uuid(value))
                    invalid();
                options.config.adapter_uuid = value;
            }
        } else if (option == "--validation" && !validation_seen &&
                   (value == "required" || value == "off")) {
            validation_seen = true;
            options.config.validation =
                value == "required" ? GraphicsValidation::required : GraphicsValidation::off;
        } else
            invalid();
        index += 2;
    }
    return options;
}

struct Owner {
    std::unique_ptr<GraphicsDevice> device;
    detail::GraphicsBackend* observer{nullptr};
};
Owner create(const Options& options) {
    auto backend = detail::make_graphics_backend();
    auto* observer = backend.get();
    auto device = take(detail::create_graphics(options.config, std::move(backend)));
    check(device->resize(options.config.window.content));
    await_state(
        *device,
        [&](const auto& state) {
            return state.content == options.config.window.content && state.visible &&
                   !state.iconified && state.framebuffer.width && state.framebuffer.height;
        },
        "setup", 0, options.config.window.content);
    const auto adapter = take(device->adapter());
    require(options.config.adapter_uuid.empty() || adapter.uuid == options.config.adapter_uuid,
            "Explicit adapter selection must match");
    return {std::move(device), observer};
}

class ResourceBudget {
  public:
    explicit ResourceBudget(std::uint64_t bytes) : byte_allowance_(bytes) {}
    void start(ProcessResources baseline) {
        baseline_ = baseline;
        initialized_ = true;
    }
    bool observe(ProcessResources value) {
        peak_.private_bytes = std::max(peak_.private_bytes, value.private_bytes);
        peak_.handles = std::max(peak_.handles, value.handles);
        peak_.gdi_objects = std::max(peak_.gdi_objects, value.gdi_objects);
        peak_.user_objects = std::max(peak_.user_objects, value.user_objects);
        const bool within = initialized_ &&
                            value.private_bytes <= baseline_.private_bytes + byte_allowance_ &&
                            value.handles <= baseline_.handles + handle_allowance &&
                            value.gdi_objects <= baseline_.gdi_objects &&
                            value.user_objects <= baseline_.user_objects;
        if (!within)
            ++failures_;
        return within;
    }
    ProcessResources baseline() const {
        return baseline_;
    }
    ProcessResources peak() const {
        return peak_;
    }
    std::uint32_t failures() const noexcept {
        return failures_;
    }

  private:
    std::uint64_t byte_allowance_;
    ProcessResources baseline_{};
    ProcessResources peak_{};
    bool initialized_{false};
    std::uint32_t failures_{0};
};

std::uint64_t elapsed_ms(shark::MonotonicTime time) {
    require(time >= shark::MonotonicTime::zero(), "Monotonic time regressed");
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(time).count());
}
void sample(Owner& owner, ResourceBudget& budget, std::string_view phase, std::uint32_t index,
            shark::MonotonicTime elapsed, bool start = false, bool diagnostic = false) {
    check(owner.device->drain());
    const auto state = poll(*owner.device);
    const auto stats = take(owner.device->presentation_stats());
    const auto resources = owner.observer->presentation_resources();
    const auto native_before = native_window_json(state.content);
    const auto inventory = diagnostic ? process_inventory() : Object{};
    const auto process = process_resources();
    const auto native_after = native_window_json(state.content);
    if (start)
        budget.start(process);
    const bool within = budget.observe(process);
    emit({{"kind", "sample"},
          {"phase", std::string(phase)},
          {"index", index},
          {"elapsed_ms", elapsed_ms(elapsed)},
          {"window", window_json(state)},
          {"stats", stats_json(stats)},
          {"resources", resources_json(resources)},
          {"process", process_json(process)},
          {"native_before_measurement", native_before},
          {"native_after_measurement", native_after},
          {"process_inventory", inventory},
          {"baseline", process_json(budget.baseline())},
          {"within_budget", within}});
    check_stats(stats);
    check_resources(resources, stats, false);
    require(stats.completed == stats.submitted && stats.in_flight == 0,
            "Drained sample still owns incomplete submissions");
    require(within || diagnostic, "Measured process resource growth exceeds the declared budget");
}

Object close(Owner& owner, bool validated) {
    check(owner.device->drain());
    const auto before = take(owner.device->diagnostics());
    check(owner.device->close());
    const auto closed = take(owner.device->presentation_stats());
    const auto after = take(owner.device->diagnostics());
    check(owner.device->close());
    const auto second = take(owner.device->presentation_stats());
    const auto after_second = take(owner.device->diagnostics());
    const auto resources = owner.observer->presentation_resources();
    const bool idempotent =
        serialized_stats(closed) == serialized_stats(second) &&
        after.warnings == after_second.warnings && after.errors == after_second.errors &&
        after.dropped == after_second.dropped && after.messages == after_second.messages;
    require(closed.closed && !closed.active && !closed.faulted && closed.in_flight == 0 &&
                closed.completed == closed.submitted &&
                closed.retired_generations == closed.generation && idempotent,
            "Close did not retire submitted uses or preserve idempotent counters");
    check_resources(resources, closed, true);
    require(!validated || (clean(before) && clean(after) && clean(after_second)),
            "Native validation was not clean through final close");
    Object record{{"closed", stats_json(closed)},
                  {"second_closed", stats_json(second)},
                  {"idempotent_close", idempotent},
                  {"before_close", diagnostics_json(before)},
                  {"after_close", diagnostics_json(after)},
                  {"after_second_close", diagnostics_json(after_second)},
                  {"resources", resources_json(resources)}};
    owner.device.reset();
    owner.observer = nullptr;
    return record;
}

template <typename Function> void with_failure_report(Owner& owner, Function function) {
    try {
        function();
    } catch (...) {
        const auto primary = std::current_exception();
        try {
            Object record{{"kind", "owner_failure"}, {"status", "FAIL"}};
            try {
                std::rethrow_exception(primary);
            } catch (const Failure& error) {
                record.emplace("error", error_json(error.error()));
            } catch (const std::exception& error) {
                record.emplace("message", error.what());
            } catch (...) {
                record.emplace("message", "Unknown presentation exception");
            }
            if (owner.device) {
                record.emplace("window", window_json(take(owner.device->window_state())));
                record.emplace("stats", stats_json(take(owner.device->presentation_stats())));
                record.emplace("diagnostics", diagnostics_json(take(owner.device->diagnostics())));
                record.emplace("resources",
                               resources_json(owner.observer->presentation_resources()));
            }
            emit(std::move(record));
            if (owner.device) {
                const auto result = owner.device->close();
                Object cleanup{
                    {"kind", "owner_failure_cleanup"},
                    {"status", "FAIL"},
                    {"close_succeeded", static_cast<bool>(result)},
                    {"stats", stats_json(take(owner.device->presentation_stats()))},
                    {"diagnostics", diagnostics_json(take(owner.device->diagnostics()))},
                    {"resources", resources_json(owner.observer->presentation_resources())}};
                if (!result)
                    cleanup.emplace("error", error_json(result.error()));
                emit(std::move(cleanup));
            }
        } catch (...) { /* Preserve the primary failure if reporting fails. */
        }
        std::rethrow_exception(primary);
    }
}

void run(const Options& options) {
    const bool validated = options.config.validation == GraphicsValidation::required;
    emit({{"kind", "identity"},
          {"schema_version", 1U},
          {"mode", std::string(options.mode)},
          {"profile", std::string(options.profile)},
          {"diagnostic", options.diagnose_resource_growth},
          {"dependency_lock_sha256", SHARK_GRAPHICS_LOCK_SHA256},
          {"source_sha256", SHARK_GRAPHICS_SOURCE_SHA256},
          {"compiler", SHARK_GRAPHICS_COMPILER},
          {"validation", validated ? "required" : "off"},
          {"selection", options.config.adapter_uuid.empty() ? "auto" : options.config.adapter_uuid},
          {"assignment_05", "NOT_RUN"}});
    emit({{"kind", "workload"},
          {"duration_seconds", options.seconds()},
          {"active_warmup_frames", active_warmup_frames},
          {"warmup_minimize_restore_cycles", 1U},
          {"resize_transitions", resize_count},
          {"minimize_restore_cycles", minimize_count},
          {"lifetimes", lifetime_count},
          {"lifetime_frames", lifetime_frames},
          {"lifetime_warmup", lifetime_warmup},
          {"sample_interval_ms", 5000U},
          {"attempt_limit", attempt_limit},
          {"handle_allowance", handle_allowance},
          {"gdi_allowance", 0U},
          {"user_allowance", 0U},
          {"private_bytes_allowance", options.byte_allowance()},
          {"native_allocation_bytes", "unavailable"}});
    if (options.mode != "stress") {
        emit({{"kind", "description"},
              {"status", "PASS"},
              {"qualification", "NOT_RUN"},
              {"native_graphics_available", native_graphics_available()},
              {"device_accessed", false},
              {"usage",
               "--help | --describe | --stress [--profile debug|release|asan] "
               "[--device auto|UUID] [--validation required|off] [--diagnose-resource-growth]"}});
        return;
    }
    ResourceBudget active_budget(options.byte_allowance());
    ResourceBudget closed_budget(options.byte_allowance());
    auto owner = create(options);
    std::uint64_t sustained_frames = 0;
    std::uint64_t sustained_duration = 0;
    std::uint32_t periodic_samples = 0;
    with_failure_report(owner, [&] {
        emit({{"kind", "stress_begin"},
              {"adapter", adapter_json(take(owner.device->adapter()))},
              {"window", window_json(poll(*owner.device))},
              {"native_window", native_window_json(options.config.window.content)},
              {"stats", stats_json(take(owner.device->presentation_stats()))}});
        // Exercise the one-time Windows restore initialization before choosing
        // the active baseline. This is fixed, visible warmup, never retried or
        // substituted for the separately measured 100/20 transition workload.
        frames(*owner.device, 24);
        const auto before_warmup_minimize = take(owner.device->presentation_stats());
        if (options.diagnose_resource_growth)
            diagnostic_process_stage("before_warmup_iconify", 1);
        check(owner.device->iconify());
        const auto warmup_iconified = await_state(
            *owner.device, [](const auto& state) { return state.iconified; }, "warmup_minimize", 1,
            options.config.window.content);
        if (options.diagnose_resource_growth)
            diagnostic_process_stage("warmup_iconified", 1);
        const auto warmup_suspended = take(owner.device->present());
        emit({{"kind", "warmup_minimize"},
              {"index", 1U},
              {"status", "suspended"},
              {"window", window_json(warmup_iconified)},
              {"before", stats_json(before_warmup_minimize)},
              {"stats", stats_json(warmup_suspended.stats)}});
        require(warmup_suspended.status == PresentationStatus::suspended &&
                    warmup_suspended.stats.submitted == before_warmup_minimize.submitted &&
                    warmup_suspended.stats.presented == before_warmup_minimize.presented &&
                    warmup_suspended.stats.suspended_frames ==
                        before_warmup_minimize.suspended_frames + 1,
                "Warmup minimize must suspend without submitting or presenting");
        check(owner.device->restore());
        const auto warmup_restored = await_state(
            *owner.device,
            [](const auto& state) {
                return !state.iconified && state.framebuffer.width && state.framebuffer.height;
            },
            "warmup_restore", 1, options.config.window.content);
        if (options.diagnose_resource_growth)
            diagnostic_process_stage("warmup_restored_before_frames", 1);
        frames(*owner.device, 8);
        emit({{"kind", "warmup_restore"},
              {"index", 1U},
              {"window", window_json(warmup_restored)},
              {"before", stats_json(warmup_suspended.stats)},
              {"stats", stats_json(take(owner.device->presentation_stats()))}});
        sample(owner, active_budget, "warmup", 0, shark::MonotonicTime::zero(), true,
               options.diagnose_resource_growth);
        shark::SteadyMonotonicClock clock;
        const auto start = clock.now();
        const shark::MonotonicTime duration = std::chrono::seconds{options.seconds()};
        auto next_sample =
            std::chrono::duration_cast<shark::MonotonicTime>(std::chrono::seconds{5});
        auto last_progress = start;
        auto previous_periodic_time = start;
        auto previous_periodic_frames = take(owner.device->presentation_stats()).presented;
        std::uint32_t resized = 0;
        std::uint32_t minimized = 0;
        while (clock.now() - start < duration || resized < resize_count) {
            if (resized < resize_count &&
                clock.now() - start >= duration * (resized + 1) / (resize_count + 1)) {
                const auto size = sizes[resized % sizes.size()];
                const auto before = take(owner.device->presentation_stats());
                const auto native_before = native_window_json(size);
                check(owner.device->resize(size));
                const auto native_after = native_window_json(size);
                const auto window =
                    await_state(*owner.device,
                                [size](const auto& state) {
                                    return state.content == size && !state.iconified &&
                                           state.framebuffer.width && state.framebuffer.height;
                                },
                                "resize", resized + 1, size,
                                {{"native_before_resize", native_before},
                                 {"native_after_resize", native_after}});
                frames(*owner.device, 16);
                const auto after = take(owner.device->presentation_stats());
                require(after.generation > before.generation,
                        "Resize did not create a fresh swapchain generation");
                ++resized;
                emit({{"kind", "resize"},
                      {"index", resized},
                      {"requested", extent_json(size)},
                      {"window", window_json(window)},
                      {"native_before_resize", native_before},
                      {"native_after_resize", native_after},
                      {"before", stats_json(before)},
                      {"stats", stats_json(after)}});
                sample(owner, active_budget, "resize", resized, clock.now() - start, false,
                       options.diagnose_resource_growth);
                if (resized % 5 == 0) {
                    const auto before_minimize = take(owner.device->presentation_stats());
                    if (options.diagnose_resource_growth)
                        diagnostic_process_stage("before_iconify", minimized + 1);
                    check(owner.device->iconify());
                    const auto iconified = await_state(
                        *owner.device, [](const auto& state) { return state.iconified; },
                        "minimize", minimized + 1, size);
                    if (options.diagnose_resource_growth)
                        diagnostic_process_stage("iconified", minimized + 1);
                    const auto suspended = take(owner.device->present());
                    ++minimized;
                    emit({{"kind", "minimize"},
                          {"index", minimized},
                          {"window", window_json(iconified)},
                          {"status", "suspended"},
                          {"before", stats_json(before_minimize)},
                          {"stats", stats_json(suspended.stats)}});
                    require(suspended.status == PresentationStatus::suspended &&
                                suspended.stats.submitted == before_minimize.submitted &&
                                suspended.stats.presented == before_minimize.presented &&
                                suspended.stats.suspended_frames ==
                                    before_minimize.suspended_frames + 1,
                            "Minimize did not suspend submission and presentation");
                    check(owner.device->restore());
                    const auto restored = await_state(
                        *owner.device,
                        [](const auto& state) {
                            return !state.iconified && state.framebuffer.width &&
                                   state.framebuffer.height;
                        },
                        "restore", minimized, size);
                    if (options.diagnose_resource_growth)
                        diagnostic_process_stage("restored_before_frames", minimized);
                    frames(*owner.device, 8);
                    emit({{"kind", "restore"},
                          {"index", minimized},
                          {"window", window_json(restored)},
                          {"stats", stats_json(take(owner.device->presentation_stats()))}});
                    sample(owner, active_budget, "restore", minimized, clock.now() - start, false,
                           options.diagnose_resource_growth);
                }
                // Transition waits have their own explicit two-second bounds.
                last_progress = clock.now();
            }
            frames(*owner.device, 1);
            const auto now = clock.now();
            require(now - last_progress <= std::chrono::seconds{5},
                    "Continuous presentation made no progress within five seconds");
            last_progress = now;
            if (now - start >= next_sample) {
                require(now - previous_periodic_time <= std::chrono::seconds{10},
                        "Periodic observation exceeded its ten-second transition allowance");
                previous_periodic_time = now;
                const auto presented = take(owner.device->presentation_stats()).presented;
                require(presented > previous_periodic_frames,
                        "Periodic sample contains no presentation progress");
                previous_periodic_frames = presented;
                sample(owner, active_budget, "periodic", ++periodic_samples, now - start, false,
                       options.diagnose_resource_growth);
                next_sample = now - start + std::chrono::seconds{5};
            }
        }
        sample(owner, active_budget, "final", 0, clock.now() - start, false,
               options.diagnose_resource_growth);
        const auto final = take(owner.device->presentation_stats());
        sustained_frames = final.presented - active_warmup_frames;
        sustained_duration = elapsed_ms(clock.now() - start);
        require(resized == resize_count && minimized == minimize_count &&
                    sustained_duration >= static_cast<std::uint64_t>(options.seconds()) * 1000 &&
                    sustained_duration <=
                        static_cast<std::uint64_t>(options.seconds()) * 1000 + 10000 &&
                    sustained_frames >= resize_count * 16 + minimize_count * 8 &&
                    periodic_samples >= options.seconds() / 5 - 1,
                "The declared sustained workload was incomplete");
        emit({{"kind", "sustained_end"},
              {"elapsed_ms", sustained_duration},
              {"presented_frames", sustained_frames},
              {"resize_transitions", resized},
              {"minimize_restore_cycles", minimized},
              {"periodic_samples", periodic_samples},
              {"stats", stats_json(final)}});
        auto closed = close(owner, validated);
        closed.emplace("kind", "active_close");
        if (options.diagnose_resource_growth) {
            const auto inventory = process_inventory();
            const auto process = process_resources();
            closed.emplace("process_inventory", inventory);
            closed.emplace("process", process_json(process));
        }
        emit(std::move(closed));
    });
    for (std::uint32_t cycle = 1; cycle <= lifetime_count; ++cycle) {
        auto short_owner = create(options);
        with_failure_report(short_owner, [&] {
            emit({{"kind", "lifetime_begin"},
                  {"cycle", cycle},
                  {"adapter", adapter_json(take(short_owner.device->adapter()))},
                  {"window", window_json(poll(*short_owner.device))},
                  {"stats", stats_json(take(short_owner.device->presentation_stats()))}});
            frames(*short_owner.device, lifetime_frames);
            auto record = close(short_owner, validated);
            const auto inventory =
                options.diagnose_resource_growth ? process_inventory() : Object{};
            const auto process = process_resources();
            if (cycle == lifetime_warmup)
                closed_budget.start(process);
            const bool measured = cycle >= lifetime_warmup;
            const bool within = !measured || closed_budget.observe(process);
            record.emplace("kind", "lifetime_end");
            record.emplace("cycle", cycle);
            record.emplace("process", process_json(process));
            record.emplace("process_inventory", inventory);
            record.emplace("baseline", process_json(closed_budget.baseline()));
            record.emplace("measured", measured);
            record.emplace("within_budget", within);
            emit(std::move(record));
            require(within || options.diagnose_resource_growth,
                    "Post-warmup closed-device resource growth exceeds the declared budget");
        });
    }
    const bool failed_budgets = active_budget.failures() != 0 || closed_budget.failures() != 0;
    const bool qualifies = validated && !options.diagnose_resource_growth;
    emit({{"kind", "complete"},
          {"status", failed_budgets ? "FAIL" : (qualifies ? "PASS" : "NOT_RUN")},
          {"qualification", qualifies && !failed_budgets ? "PASS" : "NOT_RUN"},
          {"execution_status", failed_budgets ? "FAIL" : "PASS"},
          {"diagnostic", options.diagnose_resource_growth},
          {"active_budget_failures", active_budget.failures()},
          {"closed_budget_failures", closed_budget.failures()},
          {"sustained_duration_ms", sustained_duration},
          {"sustained_presented_frames", sustained_frames},
          {"resize_transitions", resize_count},
          {"minimize_restore_cycles", minimize_count},
          {"lifetimes", lifetime_count},
          {"lifetime_presented_frames", lifetime_count * lifetime_frames},
          {"periodic_samples", periodic_samples},
          {"active_baseline", process_json(active_budget.baseline())},
          {"active_peak", process_json(active_budget.peak())},
          {"closed_baseline", process_json(closed_budget.baseline())},
          {"closed_peak", process_json(closed_budget.peak())},
          {"assignment_05", "NOT_RUN"}});
    require(!failed_budgets, "Diagnostic run retained process resource budget failures");
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        run(parse(argc, argv));
        return 0;
    } catch (const Failure& error) {
        emit({{"kind", "failure"}, {"status", "FAIL"}, {"error", error_json(error.error())}});
    } catch (const std::exception& error) {
        emit({{"kind", "failure"}, {"status", "FAIL"}, {"message", error.what()}});
    }
    return 1;
}
