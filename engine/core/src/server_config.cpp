#include <shark/server_config.hpp>

#include <cstdint>

namespace shark {

Result<ServerConfig> validate_server_config(ServerConfig candidate) {
    if (candidate.run_for && (*candidate.run_for < std::chrono::milliseconds{1} ||
                              *candidate.run_for > std::chrono::milliseconds{600000})) {
        return Error{ErrorCode::invalid_configuration,
                     "run_for must be absent or in 1..600000 milliseconds",
                     {.operation = "server.configuration.validate"}};
    }
    return candidate;
}

Result<ServerParseResult> parse_server_arguments(std::span<const std::string_view> arguments) {
    if (arguments.empty()) {
        return ServerParseResult{ServerAction::run, {}};
    }
    if (arguments.size() == 1) {
        if (arguments.front() == "--help") {
            return ServerParseResult{ServerAction::help, {}};
        }
        if (arguments.front() == "--version") {
            return ServerParseResult{ServerAction::version, {}};
        }
    }
    if (arguments.size() != 2 || arguments.front() != "--run-for-ms") {
        return Error{ErrorCode::invalid_configuration,
                     "expected --help, --version, or one --run-for-ms value",
                     {.operation = "server.arguments.parse"}};
    }
    const auto text = arguments[1];
    const auto duration_error = [] {
        return Error{ErrorCode::invalid_configuration,
                     "--run-for-ms requires 1-6 decimal digits with value 1..600000",
                     {.operation = "server.arguments.parse"}};
    };
    if (text.empty() || text.size() > 6) {
        return duration_error();
    }
    std::uint32_t milliseconds = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') {
            return duration_error();
        }
        // At most six digits are processed, so this cannot overflow uint32_t.
        milliseconds = milliseconds * 10 + static_cast<std::uint32_t>(digit - '0');
    }
    const auto candidate = validate_server_config({std::chrono::milliseconds{milliseconds}});
    if (!candidate) {
        return candidate.error();
    }
    return ServerParseResult{ServerAction::run, candidate.value()};
}

} // namespace shark
