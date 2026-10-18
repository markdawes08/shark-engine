#include <shark/client/window_launch.hpp>

#include <shark/client/window.hpp>
#include <shark/log.hpp>
#include <shark/time.hpp>

#include "window_loop.hpp"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <string>

namespace shark::client {
namespace {
Error argument_error() {
    return {ErrorCode::invalid_argument,
            "Expected --window optionally followed by --window-ms and an integer from 1 to 60000",
            {.operation = "client.window.arguments.parse"}};
}
} // namespace

Result<std::optional<OfflineFrameReport>>
detail::pump_offline_window(Window& window, OfflineFrameLoop& loop, MonotonicTime now,
                            bool& manual_pause, const OfflineInputCallback& callback,
                            Logger& diagnostics) {
    if (auto polled = window.poll_events(); !polled) {
        return polled.error();
    }
    auto state = window.state();
    if (!state) {
        return state.error();
    }
    auto input = window.capture_input();
    if (!input) {
        return input.error();
    }
    auto pending = window.take_diagnostics();
    if (!pending) {
        return pending.error();
    }
    for (const auto* error : {&pending.value().input_error, &pending.value().window_error}) {
        if (*error) {
            if (auto logged = diagnostics.write(LogLevel::warning, **error); !logged) {
                return logged.error();
            }
        }
    }
    if (state.value().close_requested) {
        return std::optional<OfflineFrameReport>{};
    }
    // Development sample controls, not action bindings or editor commands.
    // Poll continues during pause/minimize; there is no shared server clock here.
    const bool active = input.value().focused && !state.value().iconified;
    if (active && input.value().key(Key::f6).pressed) {
        manual_pause = !manual_pause;
    }
    const bool step = active && manual_pause && input.value().key(Key::f7).pressed;
    input.value().keys[static_cast<std::size_t>(Key::f6) - 1] = {};
    input.value().keys[static_cast<std::size_t>(Key::f7) - 1] = {};
    auto report =
        loop.advance(now, input.value(), {manual_pause, step, state.value().iconified}, callback);
    if (!report) {
        return report.error();
    }
    return std::optional<OfflineFrameReport>{report.value()};
}

Result<WindowLaunchOptions> parse_window_arguments(std::span<const std::string_view> arguments) {
    if (arguments.empty() || arguments.front() != "--window") {
        return argument_error();
    }
    if (arguments.size() == 1) {
        return WindowLaunchOptions{};
    }
    if (arguments.size() != 3 || arguments[1] != "--window-ms") {
        return argument_error();
    }
    const auto text = arguments[2];
    if (text.empty() || text.size() > 5) {
        return argument_error();
    }
    unsigned int milliseconds = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), milliseconds);
    if (error != std::errc{} || end != text.data() + text.size() || milliseconds == 0 ||
        milliseconds > 60000) {
        return argument_error();
    }
    return WindowLaunchOptions{std::chrono::milliseconds{milliseconds}};
}

Result<void> run_window_sample(std::string_view title, const WindowLaunchOptions& options) {
    if (options.duration && (*options.duration < std::chrono::milliseconds{1} ||
                             *options.duration > std::chrono::milliseconds{60000})) {
        return argument_error();
    }
    try {
        WindowConfig config;
        config.title = title;
        auto created = Window::create(config);
        if (!created) {
            return created.error();
        }
        auto window = std::move(created).value();
        SteadyMonotonicClock clock;
        auto created_loop = OfflineFrameLoop::create(clock.now());
        if (!created_loop) {
            return created_loop.error();
        }
        auto loop = std::move(created_loop).value();
        Logger diagnostics{std::cerr};
        bool manual_pause = false;
        // The callback is deliberately empty until a local play world exists.
        // Scheduler indices are diagnostic callbacks, never simulated MMO ticks.
        const OfflineInputCallback tick = [](std::uint64_t, const InputSnapshot&) -> Result<void> {
            return {};
        };
        while (true) {
            const auto now = clock.now();
            if (options.duration && now >= *options.duration) {
                return window->close();
            }
            const auto prior_pause = manual_pause;
            auto frame =
                detail::pump_offline_window(*window, *loop, now, manual_pause, tick, diagnostics);
            if (!frame) {
                return frame.error();
            }
            if (!frame.value()) {
                return window->close();
            }
            if (prior_pause != manual_pause ||
                (manual_pause && frame.value()->timing.executed != 0)) {
                std::cout << "{\"event\":\"offline-sample-control\",\"paused\":"
                          << (manual_pause ? "true" : "false")
                          << ",\"next_tick\":" << loop->next_tick() << "}\n"
                          << std::flush;
                if (!std::cout) {
                    return Error{ErrorCode::io_failure,
                                 "Window control output failed",
                                 {.operation = "client.window.output"}};
                }
            }
            auto wait = std::chrono::duration_cast<MonotonicTime>(std::chrono::milliseconds{10});
            if (options.duration) {
                wait = std::min(wait,
                                std::chrono::duration_cast<MonotonicTime>(*options.duration) - now);
            }
            if (auto waited = clock.wait_for(wait); !waited) {
                return waited.error();
            }
        }
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Window demonstration failed",
                     {.operation = "client.window.run"}};
    }
}

} // namespace shark::client
