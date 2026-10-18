#pragma once

#include <chrono>
#include <optional>
#include <span>
#include <string_view>

namespace shark {

enum class ServerAction { run, help, version, invalid };

struct ServerConfig {
    // An absent duration runs until a shutdown signal is received.
    std::optional<std::chrono::milliseconds> run_for;
};

struct ServerParseResult {
    ServerAction action{ServerAction::invalid};
    ServerConfig config;
    // Errors refer to static text, never to the caller's argument storage.
    std::string_view error;
};

// Arguments exclude argv[0]. Duration text is 1-6 ASCII decimal digits and its
// value must be in [1, 600000] milliseconds. Informational options stand alone.
[[nodiscard]] ServerParseResult
parse_server_arguments(std::span<const std::string_view> arguments) noexcept;

} // namespace shark
