#include <shark/client/window_launch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <string>
#include <string_view>

using namespace std::literals;

TEST_CASE("window launch distinguishes interactive and bounded runs", "[window]") {
    const std::array interactive{"--window"sv};
    const auto open = shark::client::parse_window_arguments(interactive);
    REQUIRE(open);
    REQUIRE_FALSE(open.value().duration);
    for (const auto text : {"1"sv, "250"sv, "60000"sv}) {
        const std::array arguments{"--window"sv, "--window-ms"sv, text};
        const auto parsed = shark::client::parse_window_arguments(arguments);
        REQUIRE(parsed);
        REQUIRE(parsed.value().duration);
        REQUIRE(parsed.value().duration->count() == std::stoi(std::string{text}));
    }
}

TEST_CASE("window launch rejects malformed duration before opening a device", "[window]") {
    for (const auto text : {""sv, "0"sv, "60001"sv, "-1"sv, "+1"sv, "1.0"sv, "1ms"sv, " 1"sv,
                            "1 "sv, "9999999999999999999999999"sv}) {
        const std::array arguments{"--window"sv, "--window-ms"sv, text};
        const auto parsed = shark::client::parse_window_arguments(arguments);
        REQUIRE_FALSE(parsed);
        REQUIRE(parsed.error().code == shark::ErrorCode::invalid_argument);
        REQUIRE(parsed.error().context.operation == "client.window.arguments.parse");
    }
}

TEST_CASE("window launch rejects conflicting or missing options", "[window]") {
    REQUIRE_FALSE(shark::client::parse_window_arguments({}));
    const std::array missing{"--window"sv, "--window-ms"sv};
    REQUIRE_FALSE(shark::client::parse_window_arguments(missing));
    for (const auto option : {"--help"sv, "--version"sv, "--window"sv, "--unknown"sv}) {
        const std::array arguments{"--window"sv, option, "1"sv};
        REQUIRE_FALSE(shark::client::parse_window_arguments(arguments));
    }
    const std::array extra{"--window"sv, "--window-ms"sv, "1"sv, "--help"sv};
    REQUIRE_FALSE(shark::client::parse_window_arguments(extra));
    const std::array wrong{"--window-ms"sv};
    REQUIRE_FALSE(shark::client::parse_window_arguments(wrong));
}
