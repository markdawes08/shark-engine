#include "shark/stable_id.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <type_traits>

namespace {
class SeededEntropy final : public shark::IdEntropy {
  public:
    explicit SeededEntropy(std::uint32_t seed) : state_(seed) {}
    shark::Result<void> fill(std::span<std::byte, 16> bytes) override {
        for (auto& byte : bytes) {
            state_ ^= state_ << 13U;
            state_ ^= state_ >> 17U;
            state_ ^= state_ << 5U;
            byte = static_cast<std::byte>(state_ & 0xffU);
        }
        return {};
    }

  private:
    std::uint32_t state_;
};

class FailedEntropy final : public shark::IdEntropy {
  public:
    shark::Result<void> fill(std::span<std::byte, 16> bytes) override {
        std::fill(bytes.begin(), bytes.begin() + 5, std::byte{0xab});
        shark::ErrorContext context;
        context.request_id = "creation-17";
        return shark::Error{shark::ErrorCode::io_failure, "injected entropy failure", context};
    }
};

struct AuthoredDomain {};
struct DurableDomain {};
using AuthoredId = shark::TypedId<AuthoredDomain>;
using DurableId = shark::TypedId<DurableDomain>;
static_assert(!std::is_default_constructible_v<shark::StableId>);
static_assert(!std::is_convertible_v<shark::StableId, AuthoredId>);
static_assert(!std::is_constructible_v<AuthoredId, DurableId>);
static_assert(!std::is_convertible_v<AuthoredId, DurableId>);
} // namespace

TEST_CASE("Stable IDs round trip canonical UUIDv4 bytes", "[core][identity]") {
    constexpr auto text = "12345678-9abc-4def-8123-456789abcdef";
    const auto parsed = shark::StableId::parse(text);
    REQUIRE(parsed);
    CHECK(parsed.value().to_string() == text);
    CHECK(parsed.value().bytes()[0] == std::byte{0x12});
    CHECK(parsed.value().bytes()[15] == std::byte{0xef});
    CHECK(shark::StableId::parse(parsed.value().to_string()).value() == parsed.value());
    const AuthoredId authored{parsed.value()};
    const DurableId durable{parsed.value()};
    CHECK(authored.value() == durable.value());
}

TEST_CASE("Stable ID parsing rejects noncanonical and non-v4 values", "[core][identity]") {
    const std::array invalid{"",
                             "00000000-0000-0000-0000-000000000000",
                             "12345678-9ABC-4def-8123-456789abcdef",
                             "123456789abc4def8123456789abcdef",
                             "{12345678-9abc-4def-8123-456789abcdef}",
                             "12345678_9abc-4def-8123-456789abcdef",
                             "12345678-9abc-1def-8123-456789abcdef",
                             "12345678-9abc-4def-7123-456789abcdef",
                             "12345678-9abc-4def-c123-456789abcdef",
                             "12345678-9abc-4def-8123-456789abcdeg",
                             " 12345678-9abc-4def-8123-456789abcdef",
                             "12345678-9abc-4def-8123-456789abcdef\n"};
    for (const auto* text : invalid) {
        CAPTURE(text);
        const auto parsed = shark::StableId::parse(text);
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error().code == shark::ErrorCode::invalid_id);
        CHECK(parsed.error().context.operation == "stable_id.parse");
    }
    std::string embedded_null = "12345678-9abc-4def-8123-456789abcdef";
    embedded_null[5] = '\0';
    CHECK_FALSE(shark::StableId::parse(embedded_null));
    for (const auto variant : {'8', '9', 'a', 'b'}) {
        auto valid = std::string{"12345678-9abc-4def-8123-456789abcdef"};
        valid[19] = variant;
        CHECK(shark::StableId::parse(valid));
    }
}

TEST_CASE("Identity diagnostics own their correlation context", "[core][identity]") {
    shark::ErrorContext context;
    context.operation = "fixture.import";
    context.request_id = "request-19";
    context.asset_id = "invalid-source-id";
    const auto result = shark::StableId::parse("bad", context);
    context.operation = "changed";
    context.request_id.clear();
    REQUIRE_FALSE(result);
    CHECK(result.error().context.operation == "fixture.import");
    CHECK(result.error().context.request_id == "request-19");
    CHECK(result.error().context.asset_id == "invalid-source-id");
}

TEST_CASE("Injected ID entropy gives reproducible independent sequences", "[core][identity]") {
    SeededEntropy first{1337};
    SeededEntropy second{1337};
    SeededEntropy different{7331};
    auto previous = shark::generate_stable_id(first);
    REQUIRE(previous);
    REQUIRE(shark::generate_stable_id(second).value() == previous.value());
    REQUIRE(shark::generate_stable_id(different).value() != previous.value());
    for (int index = 0; index < 128; ++index) {
        const auto next = shark::generate_stable_id(first);
        REQUIRE(next);
        CHECK(next.value() == shark::generate_stable_id(second).value());
        CHECK(next.value() != previous.value());
        CHECK(shark::StableId::parse(next.value().to_string()));
        previous = next;
    }
    SeededEntropy zeros{0};
    CHECK(shark::generate_stable_id(zeros).value().to_string() ==
          "00000000-0000-4000-8000-000000000000");
}

TEST_CASE("Failed entropy never publishes a partial identity", "[core][identity]") {
    FailedEntropy entropy;
    const auto result = shark::generate_stable_id(entropy);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == shark::ErrorCode::entropy_unavailable);
    CHECK(result.error().message == "injected entropy failure");
    CHECK(result.error().context.operation == "stable_id.generate");
    CHECK(result.error().context.request_id == "creation-17");
}

TEST_CASE("Production ID generation obtains canonical UUIDv4 from the OS", "[core][identity]") {
    const auto first = shark::generate_stable_id();
    const auto second = shark::generate_stable_id();
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first.value() != second.value());
    CHECK(shark::StableId::parse(first.value().to_string()).value() == first.value());
    CHECK(shark::StableId::parse(second.value().to_string()).value() == second.value());
}
