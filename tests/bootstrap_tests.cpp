#include <shark/bootstrap.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <initializer_list>
#include <string_view>

using namespace std::literals;

TEST_CASE("Bootstrap identifies the built engine version", "[bootstrap]") {
    REQUIRE(shark::version_string() == SHARK_EXPECTED_VERSION);
}

TEST_CASE("Bootstrap selects only supported informational actions", "[bootstrap]") {
    REQUIRE(shark::parse_bootstrap_arguments({}) == shark::BootstrapAction::help);
    const std::array help{"--help"sv};
    const std::array version{"--version"sv};
    REQUIRE(shark::parse_bootstrap_arguments(help) == shark::BootstrapAction::help);
    REQUIRE(shark::parse_bootstrap_arguments(version) == shark::BootstrapAction::version);
}

TEST_CASE("Bootstrap rejects unknown empty or conflicting arguments", "[bootstrap]") {
    for (const auto invalid : {""sv, "--launch"sv, "--Version"sv, "project.sharkproject"sv}) {
        const std::array arguments{invalid};
        INFO("argument: " << invalid);
        REQUIRE(shark::parse_bootstrap_arguments(arguments) == shark::BootstrapAction::invalid);
    }
    const std::array conflicting{"--help"sv, "--version"sv};
    const std::array repeated{"--version"sv, "--version"sv};
    REQUIRE(shark::parse_bootstrap_arguments(conflicting) == shark::BootstrapAction::invalid);
    REQUIRE(shark::parse_bootstrap_arguments(repeated) == shark::BootstrapAction::invalid);
}

TEST_CASE("CI failure sentinel is disabled in normal builds", "[failure-sentinel]") {
    INFO("SHARK_INJECT_TEST_FAILURE must fail CTest and the enclosing CI command when enabled");
    REQUIRE(SHARK_INJECT_TEST_FAILURE == 0);
}
