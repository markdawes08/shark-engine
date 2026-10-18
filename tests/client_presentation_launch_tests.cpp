#include <shark/client/presentation_launch.hpp>

#include "../engine/client/src/window_loop.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <sstream>
#include <string>

using namespace std::literals;

namespace {
constexpr std::string_view uuid = "0123456789abcdef0123456789abcdef";

void invalid(std::span<const std::string_view> arguments) {
    const auto result = shark::client::parse_presentation_arguments(arguments);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == shark::ErrorCode::invalid_argument);
    CHECK(result.error().context.operation == "client.graphics.arguments.parse");
    CHECK(result.error().message.size() < 256);
}
} // namespace

TEST_CASE("presentation sample options preserve explicit human and terminal launch modes",
          "[presentation]") {
    const std::array interactive{"--graphics"sv};
    const auto opened = shark::client::parse_presentation_arguments(interactive);
    REQUIRE(opened);
    CHECK_FALSE(opened.value().duration);
    CHECK(opened.value().adapter_uuid.empty());
    CHECK(opened.value().validation == shark::client::GraphicsValidation::off);
    for (const auto text : {"1"sv, "250"sv, "60000"sv}) {
        const std::array arguments{"--graphics"sv, "--graphics-ms"sv, text};
        const auto parsed = shark::client::parse_presentation_arguments(arguments);
        REQUIRE(parsed);
        REQUIRE(parsed.value().duration);
        CHECK(parsed.value().duration->count() == std::stoi(std::string{text}));
    }
    const std::array explicit_device{"--graphics"sv, "--validation"sv,  "required"sv, "--device"sv,
                                     uuid,           "--graphics-ms"sv, "100"sv};
    const auto selected = shark::client::parse_presentation_arguments(explicit_device);
    REQUIRE(selected);
    CHECK(selected.value().adapter_uuid == uuid);
    CHECK(selected.value().validation == shark::client::GraphicsValidation::required);
    CHECK(selected.value().duration == 100ms);
    const std::array automatic{"--graphics"sv, "--device"sv, "auto"sv, "--validation"sv, "off"sv};
    const auto auto_selected = shark::client::parse_presentation_arguments(automatic);
    REQUIRE(auto_selected);
    CHECK(auto_selected.value().adapter_uuid.empty());
    CHECK(auto_selected.value().validation == shark::client::GraphicsValidation::off);
}

TEST_CASE("presentation launch validates bounded duration device and validation values",
          "[presentation]") {
    for (const auto text : {""sv, "0"sv, "60001"sv, "-1"sv, "+1"sv, "1.0"sv, "1ms"sv, " 1"sv,
                            "1 "sv, "999999999999999999999"sv}) {
        const std::array arguments{"--graphics"sv, "--graphics-ms"sv, text};
        invalid(arguments);
    }
    for (const auto text :
         {""sv, "AUTO"sv, "nvidia"sv, "0123456789abcdef"sv, "0123456789ABCDEF0123456789ABCDEF"sv}) {
        const std::array arguments{"--graphics"sv, "--device"sv, text};
        invalid(arguments);
    }
    for (const auto text : {""sv, "on"sv, "Required"sv, "OFF"sv, "1"sv}) {
        const std::array arguments{"--graphics"sv, "--validation"sv, text};
        invalid(arguments);
    }
    const std::string oversized(100000, 'x');
    const std::array arguments{"--graphics"sv, "--device"sv, std::string_view{oversized}};
    invalid(arguments);
}

TEST_CASE("presentation launch rejects repeated conflicting missing and unknown flags",
          "[presentation]") {
    invalid({});
    const std::array wrong{"--window"sv};
    invalid(wrong);
    for (const auto option : {"--device"sv, "--validation"sv, "--graphics-ms"sv}) {
        const std::array missing{"--graphics"sv, option};
        invalid(missing);
    }
    const std::array repeated_device{"--graphics"sv, "--device"sv, "auto"sv, "--device"sv, uuid};
    const std::array repeated_duration{"--graphics"sv, "--graphics-ms"sv, "1"sv, "--graphics-ms"sv,
                                       "2"sv};
    const std::array repeated_validation{"--graphics"sv, "--validation"sv, "off"sv,
                                         "--validation"sv, "required"sv};
    invalid(repeated_device);
    invalid(repeated_duration);
    invalid(repeated_validation);
    for (const auto option : {"--help"sv, "--window"sv, "--window-ms"sv, "--graphics"sv,
                              "--unknown"sv, "--version"sv}) {
        const std::array arguments{"--graphics"sv, option, "1"sv};
        invalid(arguments);
    }
    const std::array excessive{"--graphics"sv, "--graphics-ms"sv, "1"sv,
                               "--device"sv,   "auto"sv,          "--validation"sv,
                               "off"sv,        "--device"sv,      "auto"sv};
    invalid(excessive);
}

TEST_CASE("programmatic invalid presentation launch rejects before creating a native device",
          "[presentation]") {
    for (unsigned int scenario = 0; scenario < 4; ++scenario) {
        shark::client::PresentationLaunchOptions options;
        if (scenario == 0) {
            options.duration = 0ms;
        } else if (scenario == 1) {
            options.duration = 60001ms;
        } else if (scenario == 2) {
            options.adapter_uuid = "invalid";
        } else {
            options.validation = static_cast<shark::client::GraphicsValidation>(99);
        }
        const auto result = shark::client::run_presentation_sample("CPU validation", options);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == shark::ErrorCode::invalid_argument);
        CHECK(result.error().context.operation == "client.graphics.arguments.parse");
    }
}

TEST_CASE("graphics and window snapshots share offline F6 F7 consumption", "[presentation]") {
    using namespace shark::client;
    auto created = OfflineFrameLoop::create(0ns);
    REQUIRE(created);
    auto loop = std::move(created).value();
    std::ostringstream output;
    shark::Logger diagnostics{output};
    bool paused = false;
    std::uint64_t callbacks = 0;
    const OfflineInputCallback tick = [&](std::uint64_t,
                                          const InputSnapshot& input) -> shark::Result<void> {
        ++callbacks;
        CHECK_FALSE(input.key(Key::f6).pressed);
        CHECK_FALSE(input.key(Key::f7).pressed);
        return {};
    };
    GraphicsWindowFrame snapshot;
    snapshot.state.framebuffer = {640, 480};
    snapshot.state.focused = true;
    snapshot.input.focused = true;
    snapshot.input.keys[static_cast<std::size_t>(Key::f6) - 1].pressed = true;
    const auto pause =
        detail::advance_offline_window(snapshot, *loop, 50ms, paused, tick, diagnostics);
    REQUIRE(pause);
    REQUIRE(pause.value());
    CHECK(paused);
    CHECK(callbacks == 0);
    snapshot.input.keys[static_cast<std::size_t>(Key::f6) - 1] = {};
    snapshot.input.keys[static_cast<std::size_t>(Key::f7) - 1].pressed = true;
    const auto step =
        detail::advance_offline_window(snapshot, *loop, 100ms, paused, tick, diagnostics);
    REQUIRE(step);
    REQUIRE(step.value());
    CHECK(step.value()->timing.executed == 1);
    CHECK(callbacks == 1);
    snapshot.state.iconified = true;
    const auto minimized =
        detail::advance_offline_window(snapshot, *loop, 150ms, paused, tick, diagnostics);
    REQUIRE(minimized);
    CHECK(callbacks == 1);
    snapshot.state.close_requested = true;
    const auto closed =
        detail::advance_offline_window(snapshot, *loop, 200ms, paused, tick, diagnostics);
    REQUIRE(closed);
    CHECK_FALSE(closed.value());
    CHECK(callbacks == 1);
    CHECK(output.str().empty());
}

TEST_CASE("offline sample controls share bounded machine output and sink failures",
          "[presentation]") {
    shark::client::OfflineFrameReport frame;
    std::ostringstream output;
    REQUIRE(shark::client::detail::write_offline_control(false, false, frame, 0, output));
    CHECK(output.str().empty());
    REQUIRE(shark::client::detail::write_offline_control(false, true, frame, 17, output));
    CHECK(output.str() ==
          "{\"event\":\"offline-sample-control\",\"paused\":true,\"next_tick\":17}\n");
    frame.timing.executed = 1;
    output.str("");
    REQUIRE(shark::client::detail::write_offline_control(true, true, frame, 18, output));
    CHECK(output.str() ==
          "{\"event\":\"offline-sample-control\",\"paused\":true,\"next_tick\":18}\n");
    output.setstate(std::ios::badbit);
    const auto failed =
        shark::client::detail::write_offline_control(true, false, frame, 18, output);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == shark::ErrorCode::io_failure);
}

TEST_CASE("presentation completion reports measured work without claiming qualification",
          "[presentation]") {
    using namespace shark::client;
    PresentationStats stats;
    stats.closed = true;
    std::ostringstream output;
    REQUIRE(detail::write_presentation_completion(uuid, GraphicsValidation::off, stats, output));
    CHECK(output.str() ==
          "{\"event\":\"graphics-sample-complete\",\"validation\":\"off\",\"adapter_uuid\":\""
          "0123456789abcdef0123456789abcdef\",\"submitted\":0,\"presented\":0,\"completed\":0,"
          "\"generations\":0,\"retired_generations\":0,\"synchronization\":\"not_started\","
          "\"qualification\":\"NOT_RUN\"}\n");
    stats.submitted = 123;
    stats.presented = 120;
    stats.completed = 123;
    stats.generation = 3;
    stats.retired_generations = 3;
    for (const auto synchronization :
         {PresentationSync::present_fence, PresentationSync::host_completion}) {
        stats.synchronization = synchronization;
        output.str("");
        REQUIRE(detail::write_presentation_completion(uuid, GraphicsValidation::required, stats,
                                                      output));
        CHECK(output.str().find("\"submitted\":123,\"presented\":120,\"completed\":123") !=
              std::string::npos);
        CHECK(output.str().find("\"validation\":\"required\"") != std::string::npos);
        CHECK(output.str().find("\"qualification\":\"NOT_RUN\"") != std::string::npos);
        CHECK(output.str().find(synchronization == PresentationSync::present_fence
                                    ? "\"synchronization\":\"present_fence\""
                                    : "\"synchronization\":\"host_completion\"") !=
              std::string::npos);
        CHECK(output.str().size() < 512);
    }
}

TEST_CASE("presentation completion rejects unclosed failed or inconsistent evidence",
          "[presentation]") {
    using namespace shark::client;
    for (unsigned int scenario = 0; scenario < 8; ++scenario) {
        PresentationStats stats;
        stats.closed = true;
        if (scenario == 0) {
            stats.closed = false;
        } else if (scenario == 1) {
            stats.faulted = true;
        } else if (scenario == 2) {
            stats.active = true;
        } else if (scenario == 3) {
            stats.in_flight = 1;
        } else if (scenario == 4) {
            stats.submitted = 1;
        } else if (scenario == 5) {
            stats.presented = 1;
        } else if (scenario == 6) {
            stats.generation = 1;
        } else {
            stats.synchronization = static_cast<PresentationSync>(99);
        }
        std::ostringstream output;
        const auto result =
            detail::write_presentation_completion(uuid, GraphicsValidation::off, stats, output);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == shark::ErrorCode::invalid_argument);
        CHECK(output.str().empty());
    }
    PresentationStats stats;
    stats.closed = true;
    std::ostringstream output;
    REQUIRE_FALSE(
        detail::write_presentation_completion("invalid", GraphicsValidation::off, stats, output));
    REQUIRE_FALSE(detail::write_presentation_completion(uuid, static_cast<GraphicsValidation>(99),
                                                        stats, output));
    CHECK(output.str().empty());
}

TEST_CASE("presentation completion propagates failed and throwing output sinks", "[presentation]") {
    using namespace shark::client;
    PresentationStats stats;
    stats.closed = true;
    for (const bool throwing : {false, true}) {
        std::ostringstream output;
        output.setstate(std::ios::badbit);
        if (throwing) {
            try {
                output.exceptions(std::ios::badbit);
            } catch (const std::ios_base::failure&) {
                // Installing the exception mask observes the existing badbit.
            }
        }
        const auto result =
            detail::write_presentation_completion(uuid, GraphicsValidation::off, stats, output);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == shark::ErrorCode::io_failure);
        CHECK(result.error().context.operation == "client.graphics.output");
        CHECK(output.str().empty());
    }
}
