#include <shark/net/transport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

namespace {
using namespace std::chrono_literals;
using namespace shark::net;

std::string pem(const char* name) {
    std::ifstream input{std::filesystem::path{SHARK_NETWORK_FIXTURE_DIRECTORY} / name,
                        std::ios::binary};
    REQUIRE(input);
    return {std::istreambuf_iterator<char>{input}, {}};
}

struct MutualPair {
    std::unique_ptr<TlsListener> listener;
    std::unique_ptr<TlsTransport> client;
    std::unique_ptr<TlsTransport> server;
    shark::MonotonicTime now{};
    std::string failure;

    explicit MutualPair(const char* certificate = "session/client-a.pem",
                        bool authenticated_listener = true) {
        const ServerIdentity identity{pem("server.pem"), pem("server-key.pem")};
        auto created = authenticated_listener
                           ? TlsListener::listen_loopback_authenticated(0, identity, pem("ca.pem"))
                           : TlsListener::listen_loopback(0, identity);
        REQUIRE(created);
        listener = std::move(created).value();
        const auto port = listener->port().value();
        const ClientTrust trust{pem("ca.pem"), "shark.test"};
        auto connected =
            certificate != nullptr
                ? TlsTransport::connect_loopback_authenticated(
                      port, trust, {pem(certificate), pem("session/client-a-key.pem")}, now)
                : TlsTransport::connect_loopback(port, trust, now);
        REQUIRE(connected);
        client = std::move(connected).value();
    }

    bool ready() {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            now += 1us;
            if (!server) {
                auto accepted = listener->accept(now);
                REQUIRE(accepted);
                if (accepted.value())
                    server = std::move(*accepted.value());
            }
            auto first = client->poll(now);
            if (!first) {
                failure = first.error().context.operation;
                return false;
            }
            if (server) {
                auto second = server->poll(now);
                if (!second) {
                    failure = second.error().context.operation;
                    return false;
                }
                if (first.value().state == TransportState::ready &&
                    second.value().state == TransportState::ready)
                    return true;
            }
            std::this_thread::yield();
        }
        failure = "fixture.watchdog";
        return false;
    }
};
} // namespace

TEST_CASE("mutual TLS exposes only verified live server-side certificate identity", "[net-mtls]") {
    MutualPair pair;
    REQUIRE(pair.ready());
    auto peer = pair.server->authenticated_peer();
    REQUIRE(peer);
    CHECK(peer.value().connection_id != 0);
    CHECK(peer.value().certificate == certificate_fingerprint(pem("session/client-a.pem")).value());
    // Independent OpenSSL DER SHA-256, not another call to the production hash.
    constexpr unsigned char digest[32]{0x96, 0xba, 0xaf, 0xc1, 0xe5, 0xc4, 0xef, 0x11,
                                       0x73, 0xb8, 0xd8, 0xc0, 0xff, 0xa4, 0x91, 0x6b,
                                       0x39, 0xc6, 0x33, 0xcc, 0x3b, 0x88, 0xb1, 0x00,
                                       0xf8, 0x6f, 0xd4, 0x8b, 0x98, 0x10, 0xfb, 0x20};
    for (std::size_t index = 0; index < peer.value().certificate.size(); ++index) {
        CHECK(std::to_integer<unsigned char>(peer.value().certificate[index]) == digest[index]);
    }
    CHECK_FALSE(pair.client->authenticated_peer());
    REQUIRE(pair.client->send(MessageKind::session_request, {}));
    CHECK_FALSE(pair.client->send(MessageKind::probe, {}));
    CHECK_FALSE(pair.server->send(MessageKind::session_request, {}));
    REQUIRE(pair.listener->close());
    pair.listener.reset();
    CHECK(pair.server->authenticated_peer().value() == peer.value());
    bool rejected_thread = false;
    std::thread other{[&] { rejected_thread = !pair.server->authenticated_peer(); }};
    other.join();
    CHECK(rejected_thread);
    REQUIRE(pair.server->close());
    CHECK_FALSE(pair.server->authenticated_peer());
    MutualPair replacement;
    REQUIRE(replacement.ready());
    CHECK(replacement.server->authenticated_peer().value().connection_id >
          peer.value().connection_id);
}

TEST_CASE("mutual TLS rejects absent untrusted expired and unsuitable client credentials",
          "[net-mtls]") {
    for (const auto* name :
         {static_cast<const char*>(nullptr), "session/expired-client.pem",
          "session/wrong-ca-client.pem", "session/wrong-eku-client.pem",
          "session/no-eku-client.pem", "session/wrong-ku-client.pem", "session/ca-client.pem"}) {
        CAPTURE(name != nullptr ? name : "absent client certificate");
        MutualPair pair{name};
        CHECK_FALSE(pair.ready());
        CHECK(pair.failure != "fixture.watchdog");
        if (pair.server)
            CHECK_FALSE(pair.server->authenticated_peer());
    }
}

TEST_CASE("diagnostic TLS cannot furnish a player identity or admit session traffic",
          "[net-mtls]") {
    MutualPair pair{nullptr, false};
    REQUIRE(pair.ready());
    CHECK_FALSE(pair.client->authenticated_peer());
    CHECK_FALSE(pair.server->authenticated_peer());
    CHECK_FALSE(pair.client->send(MessageKind::session_request, {}));
    CHECK_FALSE(pair.server->send(MessageKind::session_reply, {}));
}

TEST_CASE("enrollment fingerprint and mutual TLS configuration reject ambiguous or invalid inputs",
          "[net-mtls]") {
    CHECK_FALSE(certificate_fingerprint(""));
    CHECK_FALSE(certificate_fingerprint("invalid certificate"));
    CHECK_FALSE(certificate_fingerprint(std::string(65537, 'x')));
    CHECK_FALSE(certificate_fingerprint(pem("ca.pem") + pem("server.pem")));
    auto embedded = pem("ca.pem");
    embedded.push_back('\0');
    CHECK_FALSE(certificate_fingerprint(embedded));
    const ServerIdentity server{pem("server.pem"), pem("server-key.pem")};
    CHECK_FALSE(TlsListener::listen_loopback_authenticated(0, server, ""));
    CHECK_FALSE(TlsListener::listen_loopback_authenticated(0, server, "bad CA"));
    const ClientTrust trust{pem("ca.pem"), "shark.test"};
    CHECK_FALSE(TlsTransport::connect_loopback_authenticated(1, trust, {}, 0ns));
    CHECK_FALSE(TlsTransport::connect_loopback_authenticated(
        1, trust, {pem("session/client-a.pem"), pem("session/client-b-key.pem")}, 0ns));
}
