#include <shark/client/window_launch.hpp>

#include <shark/client/window.hpp>
#include <shark/log.hpp>
#include <shark/time.hpp>

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
        Logger diagnostics{std::cerr};
        while (true) {
            if (auto polled = window->poll_events(); !polled) {
                return polled.error();
            }
            auto state = window->state();
            if (!state) {
                return state.error();
            }
            // This demonstration drains captures without gameplay consumers.
            // Checkpoint 04c owns latching edges across simulation ticks.
            auto input = window->capture_input();
            if (!input) {
                return input.error();
            }
            auto pending = window->take_diagnostics();
            if (!pending) {
                return pending.error();
            }
            for (const auto* error :
                 {&pending.value().input_error, &pending.value().window_error}) {
                if (*error) {
                    if (auto logged = diagnostics.write(LogLevel::warning, **error); !logged) {
                        return logged.error();
                    }
                }
            }
            const auto now = clock.now();
            if (state.value().close_requested || (options.duration && now >= *options.duration)) {
                return window->close();
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
