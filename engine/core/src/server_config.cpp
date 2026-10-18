#include <shark/server_config.hpp>

#include <cstdint>

namespace shark {

ServerParseResult parse_server_arguments(std::span<const std::string_view> arguments) noexcept {
    if (arguments.empty()) {
        return {ServerAction::run, {}, {}};
    }
    if (arguments.size() == 1) {
        if (arguments.front() == "--help") {
            return {ServerAction::help, {}, {}};
        }
        if (arguments.front() == "--version") {
            return {ServerAction::version, {}, {}};
        }
    }
    if (arguments.size() != 2 || arguments.front() != "--run-for-ms") {
        return {ServerAction::invalid, {}, "expected --help, --version, or one --run-for-ms value"};
    }
    const auto text = arguments[1];
    constexpr auto duration_error = "--run-for-ms requires 1-6 decimal digits with value 1..600000";
    if (text.empty() || text.size() > 6) {
        return {ServerAction::invalid, {}, duration_error};
    }
    std::uint32_t milliseconds = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') {
            return {ServerAction::invalid, {}, duration_error};
        }
        // At most six digits are processed, so this cannot overflow uint32_t.
        milliseconds = milliseconds * 10 + static_cast<std::uint32_t>(digit - '0');
    }
    if (milliseconds == 0 || milliseconds > 600000) {
        return {ServerAction::invalid, {}, duration_error};
    }
    return {ServerAction::run, {std::chrono::milliseconds{milliseconds}}, {}};
}

} // namespace shark
