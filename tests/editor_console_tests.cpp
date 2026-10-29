#include "../editor/console_log.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {
using namespace shark;
using namespace shark::editor;
using Json = nlohmann::json;

tools::AuthoringProtocolReply reply(Json value, bool failed = false) {
    return {
        Json{{"jsonrpc", "2.0"}, {"id", "console-fixture"}, {"result", std::move(value)}}.dump(),
        failed};
}
Json error() {
    return {{"code", "conflict"},
            {"message", "Source changed"},
            {"context", {{"operation", "asset.publish"}, {"path", "assets/source.bin"}}}};
}
} // namespace

TEST_CASE("Editor console distinguishes retained rejection and job publication outcomes",
          "[editor-project-session]") {
    ConsoleLog console;
    const auto rejected = reply(
        {{"outcome", {{"ok", true}, {"value", {{"result", {{"ok", false}, {"error", error()}}}}}}}},
        true);
    console.observe("scene.mutate", rejected);
    REQUIRE(console.entries().size() == 1);
    CHECK(console.entries().back().level == LogLevel::error);
    REQUIRE(console.entries().back().error);
    CHECK(console.entries().back().error->code == ErrorCode::conflict);
    CHECK(console.entries().back().request_id == "console-fixture");

    Json job{{"job_id", {{"scope", "16d00000-0000-4000-8000-000000000001"}, {"sequence", "1"}}},
             {"state", "failed"},
             {"commit", nullptr},
             {"error", error()}};
    auto observed = reply({{"jobs", Json::array({job})}, {"drained", 1}});
    console.observe("asset.jobs.drain", observed);
    REQUIRE(console.entries().size() == 2);
    CHECK(console.entries().back().level == LogLevel::error);
    CHECK(console.entries().back().message == "failed");
    console.observe("asset.jobs.list", observed);
    CHECK(console.entries().size() == 2);

    job["job_id"]["sequence"] = "2";
    job["state"] = "succeeded";
    job["error"] = nullptr;
    job["commit"] = {
        {"publication",
         {{"published", true}, {"bytes", 200}, {"durability", "directory_flush_failed"}}}};
    const auto succeeded = reply(job);
    const auto original = succeeded.json;
    console.observe("asset.jobs.take", succeeded);
    REQUIRE(console.entries().size() == 3);
    CHECK(console.entries().back().level == LogLevel::warning);
    CHECK(console.entries().back().published);
    CHECK(console.entries().back().message == "Acknowledged succeeded");
    CHECK_FALSE(console.entries().back().error);
    CHECK(succeeded.json == original);

    console.observe(
        "scene.save",
        reply({{"publication",
                {{"published", true}, {"bytes", 10}, {"durability", "file_flushed"}}}}));
    CHECK(console.entries().back().published);
    CHECK(console.entries().back().level == LogLevel::info);
    console.clear();
    CHECK(console.entries().empty());
    console.observe("asset.jobs.list", reply({{"jobs", Json::array({job})}}));
    CHECK(console.entries().empty());
}

TEST_CASE("Editor console bounds retained UTF8 diagnostics without changing errors",
          "[editor-project-session]") {
    ConsoleLog console;
    Error failure{ErrorCode::invalid_argument,
                  std::string(1023, 'x') + "\xc3\xa9",
                  {.operation = "console.fixture", .path = std::string(4200, 'p')}};
    const auto original = failure.message;
    console.record("editor.fixture", failure);
    REQUIRE(console.entries().size() == 1);
    CHECK(console.entries().back().truncated);
    CHECK(console.entries().back().message.size() == 1023);
    REQUIRE(console.entries().back().error);
    CHECK(console.entries().back().error->context.path.size() == 4096);
    CHECK(failure.message == original);
    console.record("editor.fixture", failure);
    CHECK(console.entries().size() == 1);
    CHECK(console.entries().back().repeats == 2);
    for (std::size_t index = 0; index < console_maximum_entries + 3; ++index) {
        failure.message = "Failure " + std::to_string(index);
        console.record("editor.fixture", failure);
    }
    CHECK(console.entries().size() == console_maximum_entries);
    CHECK(console.dropped() == 4);
    const auto before = console.entries().size();
    console.observe("project.status", reply({{"open", false}}));
    CHECK(console.entries().size() == before);
    console.observe("scene.inspect", {"not JSON", true});
    CHECK(console.dropped() == 5);
}
