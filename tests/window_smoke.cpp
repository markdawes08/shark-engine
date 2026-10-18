#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <shark/client/window.hpp>
#include <shark/log.hpp>
#include <shark/time.hpp>

#include "window_backend.hpp"
#include "window_loop.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace shark::client;

void require(bool value, const char* check) {
    if (!value) {
        throw std::runtime_error(check);
    }
}

template <typename T> T take(shark::Result<T> result) {
    if (!result) {
        shark::Logger logger{std::cerr};
        static_cast<void>(logger.write(shark::LogLevel::error, result.error()));
        throw std::runtime_error("window API operation failed");
    }
    return std::move(result).value();
}

void check(shark::Result<void> result) {
    if (!result) {
        shark::Logger logger{std::cerr};
        static_cast<void>(logger.write(shark::LogLevel::error, result.error()));
        throw std::runtime_error("window API operation failed");
    }
}

void passed(const char* name) {
    std::cout << "{\"case\":\"" << name << "\",\"status\":\"PASS\"}\n" << std::flush;
}

template <typename Predicate> void await_state(Window& window, Predicate predicate) {
    shark::SteadyMonotonicClock clock;
    while (clock.now() < std::chrono::seconds{2}) {
        check(window.poll_events());
        if (predicate(take(window.state()))) {
            return;
        }
        check(clock.wait_for(std::chrono::milliseconds{5}));
    }
    throw std::runtime_error("native window state deadline expired");
}

class AuxiliaryWindow {
  public:
    AuxiliaryWindow()
        : previous_(GetForegroundWindow()),
          handle_(CreateWindowExW(0, L"STATIC", L"Shark focus smoke", WS_OVERLAPPEDWINDOW, 40, 40,
                                  180, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)) {
        require(handle_ != nullptr, "create owned focus fixture");
    }
    ~AuxiliaryWindow() {
        if (handle_) {
            DestroyWindow(handle_);
        }
        if (previous_ && IsWindow(previous_)) {
            SetForegroundWindow(previous_);
        }
    }
    AuxiliaryWindow(const AuxiliaryWindow&) = delete;
    AuxiliaryWindow& operator=(const AuxiliaryWindow&) = delete;
    void focus() {
        ShowWindow(handle_, SW_SHOWNOACTIVATE);
        SetActiveWindow(handle_);
        SetFocus(handle_);
    }

  private:
    HWND previous_;
    HWND handle_;
};

void key(HWND handle, UINT message, bool repeated = false, UINT code = 'A') {
    // Owned window messages exercise GLFW's real Win32 translation. They are
    // synthetic events, not proof of a physical keyboard or system-wide input.
    const auto scan = MapVirtualKeyW(code, MAPVK_VK_TO_VSC);
    auto flags = static_cast<LPARAM>(1 | (scan << 16));
    if (repeated || message == WM_KEYUP) {
        flags |= static_cast<LPARAM>(1ULL << 30);
    }
    if (message == WM_KEYUP) {
        flags |= static_cast<LPARAM>(1ULL << 31);
    }
    SendMessageW(handle, message, code, flags);
}

void run() {
    require(native_window_available(), "native capability");
    WindowConfig invalid;
    invalid.content.width = 0;
    require(!Window::create(invalid), "invalid configuration must fail before native startup");
    AuxiliaryWindow auxiliary;
    WindowConfig config;
    config.title = "Shark 04c - \u6D4B\u8BD5";
    config.content = {640, 400};
    auto window = take(Window::create(config));
    const auto handle = static_cast<HWND>(take(shark::client::detail::native_test_handle(*window)));
    require(IsWindow(handle) != FALSE, "real native HWND");
    check(window->show());
    check(window->focus());
    await_state(*window, [](const auto& state) { return state.visible && state.focused; });
    wchar_t title[128]{};
    require(GetWindowTextW(handle, title, 128) > 0 &&
                std::wstring{title} == L"Shark 04c - \u6D4B\u8BD5",
            "UTF-8 window title");
    auto conflict = Window::create(config);
    require(!conflict && conflict.error().code == shark::ErrorCode::conflict,
            "second GLFW owner must not replace first");
    require(IsWindow(handle) != FALSE, "first owner survives second rejection");
    passed("startup-owner-and-title");

    for (const auto size : {WindowExtent{700, 420}, WindowExtent{530, 350}}) {
        check(window->resize(size));
        await_state(*window, [size](const auto& state) { return state.content == size; });
        RECT rect{};
        require(GetClientRect(handle, &rect) != FALSE, "native client rectangle");
        const auto state = take(window->state());
        require(state.framebuffer.width == static_cast<std::uint32_t>(rect.right - rect.left) &&
                    state.framebuffer.height == static_cast<std::uint32_t>(rect.bottom - rect.top),
                "framebuffer observes actual client pixels");
    }
    const auto state = take(window->state());
    const auto dpi = GetDpiForWindow(handle);
    require(dpi != 0, "native DPI available");
    require(AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(handle),
                                         DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE &&
                AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                             DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE,
            "native window and owner thread retain the manifest PerMonitorV2 policy");
    const auto actual_scale = static_cast<float>(dpi) / 96.0F;
    require(std::abs(state.scale.x - actual_scale) < 0.01F &&
                std::abs(state.scale.y - actual_scale) < 0.01F,
            "content scale agrees with native window DPI");
    const auto pixel = take(cursor_to_framebuffer(state, {23.5, 41.25, true}));
    require(std::abs(pixel.x - 23.5 * state.framebuffer.width / state.content.width) < 0.0001 &&
                std::abs(pixel.y - 41.25 * state.framebuffer.height / state.content.height) <
                    0.0001,
            "cursor transforms by framebuffer/content ratio");
    std::cout << "{\"case\":\"resize-and-dpi\",\"status\":\"PASS\",\"dpi\":" << dpi
              << ",\"scale_x\":" << state.scale.x << ",\"scale_y\":" << state.scale.y
              << ",\"content_width\":" << state.content.width
              << ",\"content_height\":" << state.content.height
              << ",\"framebuffer_width\":" << state.framebuffer.width
              << ",\"framebuffer_height\":" << state.framebuffer.height << "}\n"
              << std::flush;

    static_cast<void>(take(window->capture_input()));
    key(handle, WM_KEYDOWN);
    auto input = take(window->capture_input());
    require(input.key(Key::a).held && input.key(Key::a).pressed, "native key press mapping");
    key(handle, WM_KEYDOWN, true);
    input = take(window->capture_input());
    require(input.key(Key::a).held && input.key(Key::a).repeated && !input.key(Key::a).pressed,
            "native key repeat mapping");
    key(handle, WM_KEYUP);
    SendMessageW(handle, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(23, 41));
    SendMessageW(handle, WM_LBUTTONUP, 0, MAKELPARAM(23, 41));
    SendMessageW(handle, WM_MOUSEMOVE, 0, MAKELPARAM(23, 41));
    SendMessageW(handle, WM_MOUSEWHEEL, MAKEWPARAM(0, 120), 0);
    SendMessageW(handle, WM_MOUSEWHEEL, MAKEWPARAM(0, 60), 0);
    SendMessageW(handle, WM_CHAR, 0x03A9, 0);
    SendMessageW(handle, WM_CHAR, 0xD83D, 0);
    SendMessageW(handle, WM_CHAR, 0xDE80, 0);
    input = take(window->capture_input());
    require(!input.key(Key::a).held && input.key(Key::a).released, "native key release");
    require(input.mouse_button(MouseButton::left).pressed &&
                input.mouse_button(MouseButton::left).released &&
                !input.mouse_button(MouseButton::left).held,
            "native mouse tap");
    require(input.cursor.valid && input.cursor.x == 23 && input.cursor.y == 41,
            "native cursor coordinates");
    require(input.scroll.y == 1.5, "native fractional scroll");
    require(input.text_size == 2 && input.text[0] == U'\u03A9' && input.text[1] == U'\U0001F680',
            "native Unicode scalar translation");
    passed("native-input-callbacks");

    key(handle, WM_KEYDOWN);
    SendMessageW(handle, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(23, 41));
    static_cast<void>(take(window->capture_input()));
    auxiliary.focus();
    await_state(*window, [](const auto& current) { return !current.focused; });
    input = take(window->capture_input());
    require(input.focus_lost && !input.key(Key::a).held && input.key(Key::a).released &&
                !input.mouse_button(MouseButton::left).held &&
                input.mouse_button(MouseButton::left).released,
            "actual focus transfer releases held controls");
    check(window->focus());
    await_state(*window, [](const auto& current) { return current.focused; });
    input = take(window->capture_input());
    require(input.focus_gained && !input.key(Key::a).held &&
                !input.mouse_button(MouseButton::left).held,
            "focus regain cannot resurrect controls");
    passed("focus-release-and-regain");

    for (unsigned int cycle = 0; cycle < 2; ++cycle) {
        check(window->iconify());
        await_state(*window, [](const auto& current) { return current.iconified; });
        require(IsIconic(handle) != FALSE, "real minimized HWND");
        check(window->restore());
        await_state(*window, [](const auto& current) { return !current.iconified; });
        require(IsIconic(handle) == FALSE, "real restored HWND");
    }
    passed("minimize-and-restore");
    check(window->focus());
    await_state(*window, [](const auto& current) { return current.focused; });
    static_cast<void>(take(window->capture_input()));
    key(handle, WM_KEYDOWN);
    for (std::size_t index = 0; index <= input_text_capacity; ++index) {
        SendMessageW(handle, WM_CHAR, 'x', 0);
    }
    key(handle, WM_KEYUP);
    input = take(window->capture_input());
    require(input.text_size == input_text_capacity && input.text_overflow &&
                !input.key(Key::a).held && input.key(Key::a).released,
            "native text overflow cannot block release");
    const auto diagnostics = take(window->take_diagnostics());
    require(diagnostics.rejected_input_events >= 1 && diagnostics.input_error.has_value(),
            "callback rejection visible outside callback");
    passed("bounded-callback-diagnostics");

    using namespace std::chrono_literals;
    auto loop = take(OfflineFrameLoop::create(0ns));
    bool manual_pause = false;
    shark::Logger logger{std::cerr};
    std::vector<InputSnapshot> delivered;
    const OfflineInputCallback tick = [&](std::uint64_t,
                                          const InputSnapshot& value) -> shark::Result<void> {
        delivered.push_back(value);
        return {};
    };
    const auto pump = [&](shark::MonotonicTime now) {
        auto report =
            take(detail::pump_offline_window(*window, *loop, now, manual_pause, tick, logger));
        require(report.has_value(), "window remains open during frame smoke");
        return *report;
    };
    key(handle, WM_KEYDOWN);
    require(pump(1ms).timing.executed == 0, "zero tick retains native input");
    require(pump(50ms).timing.executed == 3 && delivered.size() == 3,
            "native input consumed by three offline ticks");
    require(delivered[0].key(Key::a).pressed && !delivered[1].key(Key::a).pressed &&
                !delivered[2].key(Key::a).pressed && delivered[2].key(Key::a).held,
            "first native catch-up tick alone receives pressed edge");
    key(handle, WM_KEYUP);
    key(handle, WM_KEYDOWN, false, VK_F6);
    require(pump(60ms).paused && manual_pause, "F6 pauses actual shared sample pump");
    key(handle, WM_KEYUP, false, VK_F6);
    key(handle, WM_KEYDOWN, false, VK_F7);
    require(pump(10s).timing.executed == 1 && manual_pause,
            "F7 steps exactly once after a long paused interval");
    require(delivered.back().key(Key::a).released && !delivered.back().key(Key::f6).held &&
                !delivered.back().key(Key::f7).pressed,
            "sample controls are excluded from local tick input");
    key(handle, WM_KEYDOWN, true, VK_F7);
    require(pump(11s).timing.executed == 0, "F7 OS repeat does not create repeated steps");
    key(handle, WM_KEYUP, false, VK_F7);
    key(handle, WM_KEYDOWN, false, VK_F6);
    require(pump(12s).timing.executed == 0 && !manual_pause, "resume discards paused wall time");
    key(handle, WM_KEYUP, false, VK_F6);
    require(pump(12017ms).timing.executed == 1, "offline ticks resume at normal rate");
    passed("native-frame-input-and-controls");

    key(handle, WM_KEYDOWN);
    require(pump(12018ms).timing.executed == 0, "pending press before focus loss");
    auxiliary.focus();
    await_state(*window, [](const auto& current) { return !current.focused; });
    require(pump(13s).paused, "real focus loss pauses only local offline loop");
    auto server = take(shark::ServerScheduler::create({}, 0ns));
    std::uint32_t server_ticks = 0;
    const shark::TickCallback server_tick = [&](std::uint64_t) -> shark::Result<void> {
        ++server_ticks;
        return {};
    };
    require(take(server->poll(50ms, server_tick)).executed == 1,
            "server clock progresses during local focus pause");
    check(window->iconify());
    await_state(*window, [](const auto& current) { return current.iconified; });
    const auto minimized = pump(14s);
    require(minimized.paused && minimized.presentation_suspended,
            "real iconify suspends local presentation and offline callbacks");
    require(take(server->poll(100ms, server_tick)).executed == 1 && server_ticks == 2,
            "server clock progresses during minimize");
    check(window->restore());
    check(window->focus());
    await_state(*window, [](const auto& current) { return !current.iconified && current.focused; });
    require(pump(15s).timing.executed == 0, "restore has no paused catch-up");
    require(pump(15017ms).timing.executed == 1 && !delivered.back().key(Key::a).held &&
                !delivered.back().key(Key::a).pressed && delivered.back().key(Key::a).released,
            "focus cancellation survives frame latch");
    passed("native-frame-focus-and-server-independence");

    delivered.clear();
    key(handle, WM_KEYDOWN);
    const auto stalled = pump(20s);
    require(stalled.timing.executed == 8 && stalled.timing.discarded == 7 &&
                stalled.timing.clamped_delta && delivered.size() == 8,
            "native pump uses bounded long-frame offline policy");
    require(delivered.front().key(Key::a).pressed && !delivered.back().key(Key::a).pressed,
            "long frame cannot replay a native action");
    key(handle, WM_KEYUP);
    passed("native-frame-long-stall");

    SendMessageW(handle, WM_CLOSE, 0, 0);
    require(take(window->state()).close_requested, "native close request observed");
    require(IsWindow(handle) != FALSE, "close request retains owned resource until teardown");
    const auto before_close = loop->next_tick();
    require(!take(detail::pump_offline_window(*window, *loop, 30s, manual_pause, tick, logger)) &&
                loop->next_tick() == before_close,
            "close prevents another local tick");
    check(window->close());
    require(IsWindow(handle) == FALSE, "explicit close destroys HWND");
    check(window->close());
    window.reset();
    passed("native-close-and-idempotent-cleanup");
    config.visible = false;
    for (unsigned int cycle = 0; cycle < 16; ++cycle) {
        auto repeated = take(Window::create(config));
        const auto owned =
            static_cast<HWND>(take(shark::client::detail::native_test_handle(*repeated)));
        require(IsWindow(owned) != FALSE, "repeated lifetime creates HWND");
        repeated.reset();
        require(IsWindow(owned) == FALSE, "RAII teardown destroys HWND");
    }
    passed("repeated-native-lifetimes");
    std::cout << "{\"status\":\"PASS\",\"native_windows\":17,\"cases\":11,"
                 "\"physical_input\":false,\"rendering\":false}\n"
              << std::flush;
    require(static_cast<bool>(std::cout), "evidence output");
}
} // namespace

int main() {
    try {
        run();
        return 0;
    } catch (const std::exception& error) {
        shark::Logger logger{std::cerr};
        static_cast<void>(logger.write(
            shark::LogLevel::error,
            {shark::ErrorCode::execution_failed, error.what(), {.operation = "window.smoke"}}));
    } catch (...) {
        std::cerr << "window smoke failed with unknown exception\n";
    }
    std::cout << "{\"status\":\"FAIL\"}\n" << std::flush;
    return 1;
}
