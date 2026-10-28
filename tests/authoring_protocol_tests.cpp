#include <shark/tools/authoring_protocol.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <thread>

TEST_CASE("Authoring protocol access stays on its creating thread", "[authoring-protocol]") {
    auto created = shark::tools::AuthoringProtocol::create();
    REQUIRE(created);
    auto owner = std::move(created).value();
    constexpr auto request =
        R"({"jsonrpc":"2.0","protocol_version":1,"id":"public-api","method":"protocol.discover"})";
    std::optional<shark::Result<shark::tools::AuthoringProtocolReply>> foreign;
    std::thread other([&] { foreign.emplace(owner->handle(request)); });
    other.join();
    REQUIRE(foreign.has_value());
    REQUIRE_FALSE(*foreign);
    REQUIRE(foreign->error().code == shark::ErrorCode::conflict);
    REQUIRE(foreign->error().context.operation == "authoring.protocol.handle");
    auto local = owner->handle(request);
    REQUIRE(local);
    REQUIRE_FALSE(local.value().failed);
    REQUIRE_FALSE(local.value().json.empty());
}
