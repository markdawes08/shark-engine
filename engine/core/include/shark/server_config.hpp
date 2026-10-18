#pragma once

#include <shark/error.hpp>

#include <chrono>
#include <optional>
#include <span>
#include <string_view>

namespace shark {

enum class ServerAction { run, help, version };

struct ServerConfig {
    // An absent duration runs until a shutdown signal is received.
    std::optional<std::chrono::milliseconds> run_for;
};

struct ServerParseResult {
    ServerAction action{ServerAction::run};
    ServerConfig config;
};

// Validate a complete candidate before activation, including programmatic inputs.
// An absent duration is unbounded; a present duration must be in [1, 600000] ms.
[[nodiscard]] Result<ServerConfig> validate_server_config(ServerConfig candidate);

// Arguments exclude argv[0]. Duration text is 1-6 ASCII decimal digits and its
// value must be in [1, 600000] milliseconds. Informational options stand alone.
// Errors own their diagnostic strings and never retain caller argument storage.
[[nodiscard]] Result<ServerParseResult>
parse_server_arguments(std::span<const std::string_view> arguments);

} // namespace shark
