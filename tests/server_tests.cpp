#include <shark/server_config.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

using namespace std::literals;

TEST_CASE("Server defaults to an idle lifetime controlled by shutdown signals", "[server]") {
    const auto result = shark::parse_server_arguments({});
    REQUIRE(result.action == shark::ServerAction::run);
    REQUIRE_FALSE(result.config.run_for.has_value());
    REQUIRE(result.error.empty());
}

TEST_CASE("Server informational options are explicit and stand alone", "[server]") {
    const std::array help{"--help"sv};
    const std::array version{"--version"sv};
    REQUIRE(shark::parse_server_arguments(help).action == shark::ServerAction::help);
    REQUIRE(shark::parse_server_arguments(version).action == shark::ServerAction::version);
    for (const auto option : {"--help"sv, "--version"sv}) {
        const std::array repeated{option, option};
        const std::array duration_conflict{option, "--run-for-ms"sv, "1"sv};
        REQUIRE(shark::parse_server_arguments(repeated).action == shark::ServerAction::invalid);
        REQUIRE(shark::parse_server_arguments(duration_conflict).action ==
                shark::ServerAction::invalid);
    }
    const std::array conflict{"--help"sv, "--version"sv};
    REQUIRE(shark::parse_server_arguments(conflict).action == shark::ServerAction::invalid);
}

TEST_CASE("Server accepts bounded decimal durations including the exact limits", "[server]") {
    for (const auto& [text, expected] :
         {std::pair{"1"sv, 1}, {"20"sv, 20}, {"000001"sv, 1}, {"600000"sv, 600000}}) {
        const std::array arguments{"--run-for-ms"sv, text};
        const auto result = shark::parse_server_arguments(arguments);
        INFO("duration: " << text);
        REQUIRE(result.action == shark::ServerAction::run);
        REQUIRE(result.config.run_for == std::chrono::milliseconds{expected});
        REQUIRE(result.error.empty());
    }
}

TEST_CASE("Server rejects malformed overflow and out of range duration text", "[server]") {
    for (const auto text : {""sv,       "0"sv,       "000000"sv,  "600001"sv,
                            "999999"sv, "1000000"sv, "0000001"sv, "18446744073709551616"sv,
                            "-1"sv,     "+1"sv,      " 1"sv,      "1 "sv,
                            "1.0"sv,    "1e2"sv,     "0x10"sv,    "1ms"sv,
                            "1\n"sv,    "1\0"sv,     "\xFF"sv,    "\xD9\xA1"sv}) {
        const std::array arguments{"--run-for-ms"sv, text};
        const auto result = shark::parse_server_arguments(arguments);
        INFO("duration byte count: " << text.size());
        REQUIRE(result.action == shark::ServerAction::invalid);
        REQUIRE_FALSE(result.config.run_for.has_value());
        REQUIRE_FALSE(result.error.empty());
    }
    const std::string oversized(100000, '9');
    const std::array arguments{"--run-for-ms"sv, std::string_view{oversized}};
    REQUIRE(shark::parse_server_arguments(arguments).action == shark::ServerAction::invalid);
}

TEST_CASE("Server rejects incomplete unknown repeated and conflicting options", "[server]") {
    for (const auto option : {""sv, "--run-for-ms"sv, "--run-for-ms=1"sv, "--Run-for-ms"sv,
                              "--config"sv, "--launch"sv, "world.sharkscene"sv}) {
        const std::array arguments{option};
        REQUIRE(shark::parse_server_arguments(arguments).action == shark::ServerAction::invalid);
    }
    const std::array repeated{"--run-for-ms"sv, "1"sv, "--run-for-ms"sv, "2"sv};
    const std::array conflict{"--run-for-ms"sv, "1"sv, "--help"sv};
    const std::array wrong_order{"1"sv, "--run-for-ms"sv};
    REQUIRE(shark::parse_server_arguments(repeated).action == shark::ServerAction::invalid);
    REQUIRE(shark::parse_server_arguments(conflict).action == shark::ServerAction::invalid);
    REQUIRE(shark::parse_server_arguments(wrong_order).action == shark::ServerAction::invalid);
}
