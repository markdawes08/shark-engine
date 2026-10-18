#pragma once

#include <shark/client/graphics.hpp>

#include <chrono>
#include <optional>
#include <span>
#include <string_view>

namespace shark::client {

struct PresentationLaunchOptions {
    // Absence keeps the sample open until close. This is not an authoring API.
    std::optional<std::chrono::milliseconds> duration{};
    std::string adapter_uuid;
    // Interactive diagnostics do not require separately installed validation
    // layers. Hardware qualification always requests required validation.
    GraphicsValidation validation{GraphicsValidation::off};
};

[[nodiscard]] Result<PresentationLaunchOptions>
parse_presentation_arguments(std::span<const std::string_view> arguments);

// Clear-only presentation through the owned device and shared offline timing.
// F6/F7 retain the window sample's pause/step controls. No scene or editor UI.
[[nodiscard]] Result<void> run_presentation_sample(std::string_view title,
                                                   const PresentationLaunchOptions& options);

} // namespace shark::client
