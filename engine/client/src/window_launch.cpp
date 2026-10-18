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
detail::advance_offline_window(GraphicsWindowFrame frame, OfflineFrameLoop& loop, MonotonicTime now,
                               bool& manual_pause, const OfflineInputCallback& callback,
                               Logger& diagnostics) {
    for (const auto* error : {&frame.diagnostics.input_error, &frame.diagnostics.window_error}) {
        if (*error) {
            if (auto logged = diagnostics.write(LogLevel::warning, **error); !logged) {
                return logged.error();
            }
        }
    }
    if (frame.state.close_requested) {
        return std::optional<OfflineFrameReport>{};
    }
    // Development sample controls, not action bindings or editor commands.
    // Poll continues during pause/minimize; there is no shared server clock here.
    const bool active = frame.input.focused && !frame.state.iconified;
    if (active && frame.input.key(Key::f6).pressed) {
        manual_pause = !manual_pause;
    }
    const bool step = active && manual_pause && frame.input.key(Key::f7).pressed;
    frame.input.keys[static_cast<std::size_t>(Key::f6) - 1] = {};
    frame.input.keys[static_cast<std::size_t>(Key::f7) - 1] = {};
    auto report =
        loop.advance(now, frame.input, {manual_pause, step, frame.state.iconified}, callback);
    if (!report) {
        return report.error();
    }
    return std::optional<OfflineFrameReport>{report.value()};
}

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
    return advance_offline_window(
        {state.value(), std::move(input).value(), std::move(pending).value()}, loop, now,
        manual_pause, callback, diagnostics);
}

Result<void> detail::write_offline_control(bool prior_pause, bool manual_pause,
                                           const OfflineFrameReport& frame, std::uint64_t next_tick,
                                           std::ostream& output) {
    if (prior_pause != manual_pause || (manual_pause && frame.timing.executed != 0)) {
        output << "{\"event\":\"offline-sample-control\",\"paused\":"
               << (manual_pause ? "true" : "false") << ",\"next_tick\":" << next_tick << "}\n"
               << std::flush;
        if (!output) {
            return Error{ErrorCode::io_failure,
                         "Window control output failed",
                         {.operation = "client.window.output"}};
        }
    }
    return {};
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
            if (auto written = detail::write_offline_control(
                    prior_pause, manual_pause, *frame.value(), loop->next_tick(), std::cout);
                !written) {
                return written.error();
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
