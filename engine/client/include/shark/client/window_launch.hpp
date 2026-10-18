#pragma once

#include <shark/error.hpp>

#include <chrono>
#include <optional>
#include <span>
#include <string_view>

namespace shark::client {

struct WindowLaunchOptions {
    // A bounded development smoke mode; absence keeps the window until close.
    std::optional<std::chrono::milliseconds> duration{};
};

[[nodiscard]] Result<WindowLaunchOptions>
parse_window_arguments(std::span<const std::string_view> arguments);

// A window/event demonstration only. No rendering, scene, simulation or pause
// policy is introduced here; frame/tick consumption belongs to checkpoint 04c.
[[nodiscard]] Result<void> run_window_sample(std::string_view title,
                                             const WindowLaunchOptions& options);

} // namespace shark::client
