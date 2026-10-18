#include <shark/client/presentation_launch.hpp>

#include "window_loop.hpp"

#include <algorithm>
#include <charconv>
#include <iostream>

namespace shark::client {
namespace {
Error argument_error() {
    return {ErrorCode::invalid_argument,
            "Expected --graphics with optional --graphics-ms 1..60000, --device UUID|auto, and "
            "--validation required|off; each option may appear once",
            {.operation = "client.graphics.arguments.parse"}};
}

bool valid_duration(const std::optional<std::chrono::milliseconds>& duration) noexcept {
    return !duration || (*duration >= std::chrono::milliseconds{1} &&
                         *duration <= std::chrono::milliseconds{60000});
}
} // namespace

Result<void> detail::write_presentation_completion(std::string_view adapter_uuid,
                                                   GraphicsValidation validation,
                                                   const PresentationStats& stats,
                                                   std::ostream& output) {
    if (!valid_graphics_uuid(adapter_uuid) ||
        (validation != GraphicsValidation::required && validation != GraphicsValidation::off) ||
        (stats.synchronization != PresentationSync::present_fence &&
         stats.synchronization != PresentationSync::host_completion) ||
        !stats.closed || stats.faulted || stats.active || stats.in_flight != 0 ||
        stats.completed != stats.submitted || stats.presented > stats.submitted ||
        stats.retired_generations != stats.generation) {
        return Error{ErrorCode::invalid_argument,
                     "Presentation completion requires a valid adapter and safely closed sample",
                     {.operation = "client.graphics.output"}};
    }
    try {
        const char* synchronization =
            stats.generation == 0
                ? "not_started"
                : (stats.synchronization == PresentationSync::present_fence ? "present_fence"
                                                                            : "host_completion");
        output << "{\"event\":\"graphics-sample-complete\",\"validation\":\""
               << (validation == GraphicsValidation::required ? "required" : "off")
               << "\",\"adapter_uuid\":\"" << adapter_uuid << "\",\"submitted\":" << stats.submitted
               << ",\"presented\":" << stats.presented << ",\"completed\":" << stats.completed
               << ",\"generations\":" << stats.generation
               << ",\"retired_generations\":" << stats.retired_generations
               << ",\"synchronization\":\"" << synchronization
               << "\",\"qualification\":\"NOT_RUN\"}\n"
               << std::flush;
        if (output) {
            return {};
        }
    } catch (...) {
        // No retry: the stream may already contain a partial record.
    }
    return Error{ErrorCode::io_failure,
                 "Presentation sample completion output failed",
                 {.operation = "client.graphics.output"}};
}

Result<PresentationLaunchOptions>
parse_presentation_arguments(std::span<const std::string_view> arguments) {
    if (arguments.empty() || arguments.front() != "--graphics" || arguments.size() > 7 ||
        arguments.size() % 2 == 0) {
        return argument_error();
    }
    PresentationLaunchOptions options;
    bool duration_seen = false;
    bool adapter_seen = false;
    bool validation_seen = false;
    for (std::size_t index = 1; index < arguments.size(); index += 2) {
        const auto key = arguments[index];
        const auto value = arguments[index + 1];
        if (key == "--graphics-ms" && !duration_seen) {
            duration_seen = true;
            if (value.empty() || value.size() > 5) {
                return argument_error();
            }
            unsigned int milliseconds = 0;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), milliseconds);
            if (error != std::errc{} || end != value.data() + value.size() || milliseconds == 0 ||
                milliseconds > 60000) {
                return argument_error();
            }
            options.duration = std::chrono::milliseconds{milliseconds};
        } else if (key == "--device" && !adapter_seen) {
            adapter_seen = true;
            if (value != "auto" && !valid_graphics_uuid(value)) {
                return argument_error();
            }
            if (value != "auto") {
                options.adapter_uuid = value;
            }
        } else if (key == "--validation" && !validation_seen) {
            validation_seen = true;
            if (value != "required" && value != "off") {
                return argument_error();
            }
            options.validation =
                value == "required" ? GraphicsValidation::required : GraphicsValidation::off;
        } else {
            return argument_error();
        }
    }
    return options;
}

Result<void> run_presentation_sample(std::string_view title,
                                     const PresentationLaunchOptions& options) {
    if (!valid_duration(options.duration) ||
        (!options.adapter_uuid.empty() && !valid_graphics_uuid(options.adapter_uuid)) ||
        (options.validation != GraphicsValidation::off &&
         options.validation != GraphicsValidation::required)) {
        return argument_error();
    }
    try {
        GraphicsConfig config;
        config.window.title = title;
        config.adapter_uuid = options.adapter_uuid;
        config.validation = options.validation;
        auto created = GraphicsDevice::create(config);
        if (!created) {
            return created.error();
        }
        auto graphics = std::move(created).value();
        const auto finish = [&]() -> Result<void> {
            if (auto closed = graphics->close(); !closed) {
                return closed.error();
            }
            const auto stats = graphics->presentation_stats();
            if (!stats) {
                return stats.error();
            }
            const auto adapter = graphics->adapter();
            if (!adapter) {
                return adapter.error();
            }
            return detail::write_presentation_completion(adapter.value().uuid, options.validation,
                                                         stats.value(), std::cout);
        };
        SteadyMonotonicClock clock;
        auto created_loop = OfflineFrameLoop::create(clock.now());
        if (!created_loop) {
            return created_loop.error();
        }
        auto loop = std::move(created_loop).value();
        Logger diagnostics{std::cerr};
        bool manual_pause = false;
        const OfflineInputCallback tick = [](std::uint64_t, const InputSnapshot&) -> Result<void> {
            return {};
        };
        while (true) {
            const auto now = clock.now();
            if (options.duration && now >= *options.duration) {
                return finish();
            }
            auto snapshot = graphics->poll_window();
            if (!snapshot) {
                return snapshot.error();
            }
            const bool prior_pause = manual_pause;
            auto frame = detail::advance_offline_window(std::move(snapshot).value(), *loop, now,
                                                        manual_pause, tick, diagnostics);
            if (!frame) {
                return frame.error();
            }
            if (!frame.value()) {
                return finish();
            }
            if (auto written = detail::write_offline_control(
                    prior_pause, manual_pause, *frame.value(), loop->next_tick(), std::cout);
                !written) {
                return written.error();
            }
            auto presented = graphics->present();
            if (!presented) {
                return presented.error();
            }
            if (presented.value().status == PresentationStatus::closed) {
                return finish();
            }
            // Presentation's waiting/suspended outcomes always return to the
            // event pump after a bounded wait; no local busy retry or GPU clock.
            auto wait = std::chrono::duration_cast<MonotonicTime>(std::chrono::milliseconds{10});
            if (options.duration) {
                const auto remaining =
                    std::chrono::duration_cast<MonotonicTime>(*options.duration) - clock.now();
                wait = std::clamp(remaining, MonotonicTime::zero(), wait);
            }
            if (auto waited = clock.wait_for(wait); !waited) {
                return waited.error();
            }
        }
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Presentation demonstration failed",
                     {.operation = "client.graphics.run"}};
    }
}

} // namespace shark::client
