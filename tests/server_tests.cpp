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
    REQUIRE(result);
    REQUIRE(result.value().action == shark::ServerAction::run);
    REQUIRE_FALSE(result.value().config.run_for.has_value());
}

TEST_CASE("Server informational options are explicit and stand alone", "[server]") {
    const std::array help{"--help"sv};
    const std::array version{"--version"sv};
    REQUIRE(shark::parse_server_arguments(help).value().action == shark::ServerAction::help);
    REQUIRE(shark::parse_server_arguments(version).value().action == shark::ServerAction::version);
    for (const auto option : {"--help"sv, "--version"sv}) {
        const std::array repeated{option, option};
        const std::array duration_conflict{option, "--run-for-ms"sv, "1"sv};
        REQUIRE_FALSE(shark::parse_server_arguments(repeated));
        REQUIRE_FALSE(shark::parse_server_arguments(duration_conflict));
    }
    const std::array conflict{"--help"sv, "--version"sv};
    REQUIRE_FALSE(shark::parse_server_arguments(conflict));
}

TEST_CASE("Server accepts bounded decimal durations including the exact limits", "[server]") {
    for (const auto& [text, expected] :
         {std::pair{"1"sv, 1}, {"20"sv, 20}, {"000001"sv, 1}, {"600000"sv, 600000}}) {
        const std::array arguments{"--run-for-ms"sv, text};
        const auto result = shark::parse_server_arguments(arguments);
        INFO("duration: " << text);
        REQUIRE(result);
        REQUIRE(result.value().action == shark::ServerAction::run);
        REQUIRE(result.value().config.run_for == std::chrono::milliseconds{expected});
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
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == shark::ErrorCode::invalid_configuration);
        REQUIRE_FALSE(result.error().message.empty());
        REQUIRE_FALSE(result.error().context.operation.empty());
    }
    const std::string oversized(100000, '9');
    const std::array arguments{"--run-for-ms"sv, std::string_view{oversized}};
    REQUIRE_FALSE(shark::parse_server_arguments(arguments));
}

TEST_CASE("Server rejects incomplete unknown repeated and conflicting options", "[server]") {
    for (const auto option : {""sv, "--run-for-ms"sv, "--run-for-ms=1"sv, "--Run-for-ms"sv,
                              "--config"sv, "--launch"sv, "world.sharkscene"sv}) {
        const std::array arguments{option};
        REQUIRE_FALSE(shark::parse_server_arguments(arguments));
    }
    const std::array repeated{"--run-for-ms"sv, "1"sv, "--run-for-ms"sv, "2"sv};
    const std::array conflict{"--run-for-ms"sv, "1"sv, "--help"sv};
    const std::array wrong_order{"1"sv, "--run-for-ms"sv};
    REQUIRE_FALSE(shark::parse_server_arguments(repeated));
    REQUIRE_FALSE(shark::parse_server_arguments(conflict));
    REQUIRE_FALSE(shark::parse_server_arguments(wrong_order));
}

TEST_CASE("Server validates complete programmatic configuration before activation",
          "[server][core][configuration]") {
    const auto default_config = shark::validate_server_config({});
    REQUIRE(default_config);
    REQUIRE_FALSE(default_config.value().run_for.has_value());
    for (const auto duration : {1ms, 20ms, 600000ms}) {
        const shark::ServerConfig candidate{duration};
        const auto result = shark::validate_server_config(candidate);
        REQUIRE(result);
        REQUIRE(result.value().run_for == candidate.run_for);
    }
    for (const auto duration : {std::chrono::milliseconds::min(), -1ms, 0ms, 600001ms,
                                std::chrono::milliseconds::max()}) {
        const shark::ServerConfig candidate{duration};
        const auto result = shark::validate_server_config(candidate);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == shark::ErrorCode::invalid_configuration);
        REQUIRE(result.error().context.operation == "server.configuration.validate");
        REQUIRE(candidate.run_for == duration);
    }
}

TEST_CASE("Server parsing uses shared configuration validation and owns diagnostics",
          "[server][core][configuration]") {
    for (const auto text : {"0"sv, "600001"sv}) {
        const std::array arguments{"--run-for-ms"sv, text};
        const auto result = shark::parse_server_arguments(arguments);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().context.operation == "server.configuration.validate");
    }
    const auto result = [] {
        const std::string text(100000, '9');
        const std::array arguments{"--run-for-ms"sv, std::string_view{text}};
        return shark::parse_server_arguments(arguments);
    }();
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == shark::ErrorCode::invalid_configuration);
    REQUIRE(result.error().context.operation == "server.arguments.parse");
    REQUIRE(result.error().message ==
            "--run-for-ms requires 1-6 decimal digits with value 1..600000");
}
