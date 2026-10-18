#include <shark/bootstrap.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <initializer_list>
#include <string>
#include <string_view>

using namespace std::literals;

TEST_CASE("Bootstrap identifies the built engine version", "[bootstrap]") {
    REQUIRE(shark::version_string() == SHARK_EXPECTED_VERSION);
}

TEST_CASE("Bootstrap selects only supported informational actions", "[bootstrap]") {
    REQUIRE(shark::parse_bootstrap_arguments({}).value() == shark::BootstrapAction::help);
    const std::array help{"--help"sv};
    const std::array version{"--version"sv};
    REQUIRE(shark::parse_bootstrap_arguments(help).value() == shark::BootstrapAction::help);
    REQUIRE(shark::parse_bootstrap_arguments(version).value() == shark::BootstrapAction::version);
}

TEST_CASE("Bootstrap rejects unknown empty or conflicting arguments", "[bootstrap]") {
    for (const auto invalid : {""sv, "--launch"sv, "--Version"sv, "project.sharkproject"sv}) {
        const std::array arguments{invalid};
        INFO("argument: " << invalid);
        const auto result = shark::parse_bootstrap_arguments(arguments);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == shark::ErrorCode::invalid_argument);
        REQUIRE(result.error().context.operation == "bootstrap.arguments.parse");
        REQUIRE_FALSE(result.error().message.empty());
    }
    const std::array conflicting{"--help"sv, "--version"sv};
    const std::array repeated{"--version"sv, "--version"sv};
    REQUIRE_FALSE(shark::parse_bootstrap_arguments(conflicting));
    REQUIRE_FALSE(shark::parse_bootstrap_arguments(repeated));
}

TEST_CASE("Bootstrap diagnostics own text and exclude untrusted argument payloads",
          "[bootstrap][core][configuration]") {
    auto result = [] {
        const std::string input(100000, 'x');
        const std::array arguments{std::string_view{input}};
        return shark::parse_bootstrap_arguments(arguments);
    }();
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == shark::ErrorCode::invalid_argument);
    REQUIRE(result.error().message == "Unsupported arguments: expected --help or --version");
    REQUIRE(result.error().context.operation == "bootstrap.arguments.parse");
}

TEST_CASE("CI failure sentinel is disabled in normal builds", "[failure-sentinel]") {
    INFO("SHARK_INJECT_TEST_FAILURE must fail CTest and the enclosing CI command when enabled");
    REQUIRE(SHARK_INJECT_TEST_FAILURE == 0);
}
