#include "shark/content_digest.hpp"
#include "shark/net/transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

namespace {
shark::Result<shark::ContentDigest> hash(std::string_view text) {
    return shark::sha256({reinterpret_cast<const std::byte*>(text.data()), text.size()});
}
} // namespace

TEST_CASE("Content SHA-256 matches independent published algorithm vectors", "[content-digest]") {
    const auto empty = hash("");
    const auto abc = hash("abc");
    REQUIRE(empty);
    REQUIRE(abc);
    CHECK(empty.value().to_hex() ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(abc.value().to_hex() ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const auto million = hash(std::string(1000000, 'a'));
    REQUIRE(million);
    CHECK(million.value().to_hex() ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    std::vector<std::byte> bytes;
    for (unsigned i = 0; i < 256; ++i)
        bytes.push_back(static_cast<std::byte>(i));
    const auto binary = shark::sha256(bytes);
    REQUIRE(binary);
    CHECK(binary.value().to_hex() ==
          "40aff2e9d2d8922e47afd4648e6967497158785fbd1da870e7110266bf944880");
}
TEST_CASE("Content digest parsing requires exact canonical hex", "[content-digest]") {
    const auto value = hash("abc");
    REQUIRE(value);
    const auto parsed = shark::ContentDigest::parse(value.value().to_hex());
    REQUIRE(parsed);
    CHECK(parsed.value() == value.value());
    CHECK_FALSE(shark::ContentDigest::parse(""));
    CHECK_FALSE(shark::ContentDigest::parse(std::string(63, 'a')));
    CHECK_FALSE(shark::ContentDigest::parse(std::string(65, 'a')));
    CHECK_FALSE(shark::ContentDigest::parse(std::string(64, 'A')));
    CHECK_FALSE(shark::ContentDigest::parse(std::string(64, 'g')));
    CHECK(shark::ContentDigest::parse(std::string(64, '0')));
}
TEST_CASE("Content hashing shares TLS initialization and remains safe across callers",
          "[content-digest]") {
    std::ifstream file{std::string{SHARK_NETWORK_FIXTURE_DIRECTORY} + "/server.pem",
                       std::ios::binary};
    REQUIRE(file.good());
    const std::string certificate{std::istreambuf_iterator<char>{file},
                                  std::istreambuf_iterator<char>{}};
    std::vector<std::thread> threads;
    std::array<bool, 4> passed{};
    for (std::size_t i = 0; i < passed.size(); ++i) {
        threads.emplace_back([&, i] {
            bool good = true;
            for (unsigned n = 0; n < 32; ++n) {
                const auto digest = hash("abc");
                const auto fingerprint = shark::net::certificate_fingerprint(certificate);
                good = good && digest && fingerprint &&
                       digest.value().to_hex() ==
                           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
            }
            passed[i] = good;
        });
    }
    for (auto& thread : threads)
        thread.join();
    for (const auto good : passed)
        CHECK(good);
}
