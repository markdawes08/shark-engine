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

// Native window with the local offline frame policy: F6 toggles manual pause,
// F7 steps once while paused. No rendering, scene or game-authoring UI exists.
[[nodiscard]] Result<void> run_window_sample(std::string_view title,
                                             const WindowLaunchOptions& options);

} // namespace shark::client
