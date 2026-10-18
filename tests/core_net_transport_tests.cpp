#include <shark/net/transport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using shark::MonotonicTime;
using shark::net::MessageKind;
using shark::net::TlsListener;
using shark::net::TlsTransport;
using shark::net::TransportLimits;
using shark::net::TransportState;

std::string fixture(const char* name) {
    std::ifstream stream{std::filesystem::path{SHARK_NETWORK_FIXTURE_DIRECTORY} / name,
                         std::ios::binary};
    REQUIRE(stream);
    return {std::istreambuf_iterator<char>{stream}, {}};
}

shark::net::ClientTrust trust() {
    return {fixture("ca.pem"), "shark.test"};
}
shark::net::ServerIdentity identity() {
    return {fixture("server.pem"), fixture("server-key.pem")};
}

template <typename T> void rejects(const shark::Result<T>& result, shark::ErrorCode code) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().context.operation.empty());
}

struct Pair {
    std::unique_ptr<TlsListener> listener;
    std::unique_ptr<TlsTransport> client;
    std::unique_ptr<TlsTransport> server;
    MonotonicTime now{0};

    explicit Pair(const TransportLimits& limits = {}, std::size_t connections = 16) {
        auto listen = TlsListener::listen_loopback(0, identity(), limits, connections);
        REQUIRE(listen);
        listener = std::move(listen).value();
        auto port = listener->port();
        REQUIRE(port);
        auto connected = TlsTransport::connect_loopback(port.value(), trust(), now, limits);
        REQUIRE(connected);
        client = std::move(connected).value();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!server || client->stats().value().state != TransportState::ready ||
               server->stats().value().state != TransportState::ready) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            if (!server) {
                auto accepted = listener->accept(now);
                REQUIRE(accepted);
                if (accepted.value()) {
                    server = std::move(*accepted.value());
                }
            }
            step();
        }
    }

    void step(std::size_t budget = 16) {
        now += 1us;
        auto first = client->poll(now, budget);
        REQUIRE(first);
        CHECK(first.value().operations <= budget);
        if (server) {
            auto second = server->poll(now, budget);
            REQUIRE(second);
            CHECK(second.value().operations <= budget);
        }
        std::this_thread::yield();
    }

    shark::net::Frame transfer(TlsTransport& receiver) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            auto received = receiver.receive();
            REQUIRE(received);
            if (received.value()) {
                return std::move(*received.value());
            }
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            step(1);
        }
    }
};
} // namespace

TEST_CASE("TLS transport validates bounds and explicit trust before connection",
          "[net-transport]") {
    using shark::ErrorCode;
    auto settings = trust();
    rejects(TlsTransport::connect_loopback(0, settings, 0ns), ErrorCode::invalid_configuration);
    rejects(TlsTransport::connect_loopback(1, settings, -1ns), ErrorCode::invalid_configuration);
    settings.ca_pem.clear();
    rejects(TlsTransport::connect_loopback(1, settings, 0ns), ErrorCode::invalid_configuration);
    settings = trust();
    for (const auto* name : {"", "shark.test/evil", "shark.test\n", "*"}) {
        settings.server_name = name;
        rejects(TlsTransport::connect_loopback(1, settings, 0ns), ErrorCode::invalid_configuration);
    }
    settings = trust();
    settings.ca_pem.push_back('\0');
    rejects(TlsTransport::connect_loopback(1, settings, 0ns), ErrorCode::invalid_configuration);
    settings.ca_pem.assign(65537, 'x');
    rejects(TlsTransport::connect_loopback(1, settings, 0ns), ErrorCode::invalid_configuration);
    for (const auto frames :
         {std::size_t{0}, std::size_t{65}, std::numeric_limits<std::size_t>::max()}) {
        TransportLimits limits;
        limits.queue_frames = frames;
        rejects(TlsListener::listen_loopback(0, identity(), limits),
                ErrorCode::invalid_configuration);
    }
    for (const auto bytes :
         {std::size_t{0}, shark::net::wire_max_frame_bytes - 1, std::size_t{262145}}) {
        TransportLimits limits;
        limits.queue_bytes = bytes;
        rejects(TlsListener::listen_loopback(0, identity(), limits),
                ErrorCode::invalid_configuration);
    }
    for (const auto duration : {-1ns, 0ns, MonotonicTime{61s}}) {
        TransportLimits limits;
        limits.handshake_timeout = duration;
        rejects(TlsListener::listen_loopback(0, identity(), limits),
                ErrorCode::invalid_configuration);
        limits = {};
        limits.write_timeout = duration;
        rejects(TlsListener::listen_loopback(0, identity(), limits),
                ErrorCode::invalid_configuration);
    }
    rejects(TlsListener::listen_loopback(0, identity(), {}, 0), ErrorCode::invalid_configuration);
    rejects(TlsListener::listen_loopback(0, identity(), {}, 17), ErrorCode::invalid_configuration);
    auto bad_identity = identity();
    bad_identity.private_key_pem = fixture("ca-key.pem");
    rejects(TlsListener::listen_loopback(0, bad_identity), ErrorCode::invalid_configuration);
    bad_identity.private_key_pem = "invalid";
    rejects(TlsListener::listen_loopback(0, bad_identity), ErrorCode::invalid_configuration);
}

TEST_CASE("TLS wire negotiation gates sends and times out without a peer handshake",
          "[net-transport]") {
    auto listened = TlsListener::listen_loopback(0, identity());
    REQUIRE(listened);
    auto& listener = *listened.value();
    auto connected = TlsTransport::connect_loopback(listener.port().value(), trust(), 0ns);
    REQUIRE(connected);
    auto& client = *connected.value();
    rejects(client.send(MessageKind::probe, {}), shark::ErrorCode::conflict);
    rejects(client.poll(-1ns), shark::ErrorCode::invalid_argument);
    rejects(client.poll(0ns, 0), shark::ErrorCode::invalid_argument);
    rejects(client.poll(0ns, 65), shark::ErrorCode::invalid_argument);
    auto timed_out = client.poll(5s);
    rejects(timed_out, shark::ErrorCode::io_failure);
    CHECK(timed_out.error().context.operation == "net.transport.timeout");
    REQUIRE(client.stats());
    CHECK(client.stats().value().state == TransportState::failed);
    rejects(client.receive(), shark::ErrorCode::conflict);
    rejects(client.poll(6s), shark::ErrorCode::conflict);
    REQUIRE(client.close());
    REQUIRE(client.close());
    CHECK(client.stats().value().state == TransportState::closed);
}

TEST_CASE("TLS peers exchange owned ordered bytes after listener destruction", "[net-transport]") {
    Pair pair;
    REQUIRE(pair.listener->close());
    pair.listener.reset();
    std::vector<std::byte> bytes(shark::net::wire_max_payload, std::byte{0x5a});
    REQUIRE(pair.client->send(MessageKind::probe, bytes));
    bytes.assign(bytes.size(), std::byte{0x77});
    auto received = pair.transfer(*pair.server);
    CHECK(received.kind == MessageKind::probe);
    CHECK(received.sequence == 2);
    CHECK(received.payload ==
          std::vector<std::byte>(shark::net::wire_max_payload, std::byte{0x5a}));
    REQUIRE(pair.server->send(MessageKind::probe_reply, received.payload));
    auto reply = pair.transfer(*pair.client);
    CHECK(reply.sequence == 2);
    CHECK(reply.payload == received.payload);
    REQUIRE(pair.client->send(MessageKind::probe, {}));
    CHECK(pair.transfer(*pair.server).sequence == 3);
    CHECK(pair.client->stats().value().socket_bytes_sent >
          2 * shark::net::wire_header_bytes + shark::net::wire_max_payload);
    CHECK(pair.server->stats().value().frames_received == 3);
    REQUIRE(pair.client->close());
    CHECK(reply.payload == received.payload);
}

TEST_CASE("TLS send count and byte pressure do not consume sequences", "[net-transport]") {
    TransportLimits limits;
    limits.queue_frames = 2;
    Pair pair{limits};
    rejects(pair.client->send(MessageKind::hello, {}), shark::ErrorCode::invalid_argument);
    rejects(pair.client->send(MessageKind::probe_reply, {}), shark::ErrorCode::invalid_argument);
    rejects(pair.server->send(MessageKind::probe, {}), shark::ErrorCode::invalid_argument);
    const std::vector<std::byte> oversized(shark::net::wire_max_payload + 1);
    rejects(pair.client->send(MessageKind::probe, oversized), shark::ErrorCode::invalid_argument);
    REQUIRE(pair.client->send(MessageKind::probe, {}));
    REQUIRE(pair.client->send(MessageKind::probe, {}));
    rejects(pair.client->send(MessageKind::probe, {}), shark::ErrorCode::conflict);
    CHECK(pair.client->stats().value().send_queue_frames == 2);
    CHECK(pair.client->stats().value().send_queue_bytes == 40);
    CHECK(pair.transfer(*pair.server).sequence == 2);
    CHECK(pair.transfer(*pair.server).sequence == 3);
    REQUIRE(pair.client->send(MessageKind::probe, {}));
    CHECK(pair.transfer(*pair.server).sequence == 4);

    limits.queue_frames = 64;
    limits.queue_bytes = shark::net::wire_max_frame_bytes;
    Pair byte_pair{limits};
    std::vector<std::byte> maximum(shark::net::wire_max_payload, std::byte{0x01});
    REQUIRE(byte_pair.client->send(MessageKind::probe, maximum));
    rejects(byte_pair.client->send(MessageKind::probe, {}), shark::ErrorCode::conflict);
    CHECK(byte_pair.client->stats().value().send_queue_bytes == limits.queue_bytes);
    CHECK(byte_pair.transfer(*byte_pair.server).sequence == 2);
    REQUIRE(byte_pair.client->send(MessageKind::probe, {}));
    CHECK(byte_pair.transfer(*byte_pair.server).sequence == 3);
}

TEST_CASE("TLS receiver pressure remains bounded and resumes in order", "[net-transport]") {
    TransportLimits limits;
    limits.queue_frames = 2;
    Pair pair{limits};
    REQUIRE(pair.client->send(MessageKind::probe, {}));
    REQUIRE(pair.client->send(MessageKind::probe, {}));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (pair.server->stats().value().receive_queue_frames < 2) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        pair.step();
    }
    REQUIRE(pair.client->send(MessageKind::probe, {}));
    for (int index = 0; index < 100; ++index) {
        pair.step();
        CHECK(pair.server->stats().value().receive_queue_frames == 2);
        CHECK(pair.server->stats().value().receive_queue_bytes <= limits.queue_bytes);
    }
    CHECK(pair.transfer(*pair.server).sequence == 2);
    CHECK(pair.transfer(*pair.server).sequence == 3);
    CHECK(pair.transfer(*pair.server).sequence == 4);
}

TEST_CASE("TLS queue and idle deadlines are explicit and terminal", "[net-transport]") {
    SECTION("Queued frame has an absolute send deadline") {
        Pair pair;
        REQUIRE(pair.client->send(MessageKind::probe, {}));
        auto result = pair.client->poll(pair.now + 5s);
        rejects(result, shark::ErrorCode::io_failure);
        CHECK(result.error().context.operation == "net.transport.timeout");
        CHECK(pair.client->stats().value().state == TransportState::failed);
        CHECK(pair.client->stats().value().send_queue_bytes == 0);
    }
    SECTION("Idle peer closes and monotonic regression changes no state") {
        Pair pair;
        auto before = pair.client->stats().value();
        rejects(pair.client->poll(pair.now - 1ns), shark::ErrorCode::invalid_argument);
        CHECK(pair.client->stats().value().state == before.state);
        auto result = pair.client->poll(pair.now + 30s);
        rejects(result, shark::ErrorCode::io_failure);
        CHECK(pair.client->stats().value().state == TransportState::failed);
    }
}

TEST_CASE("TLS methods reject another thread without losing the live connection",
          "[net-transport]") {
    Pair pair;
    std::array<bool, 9> rejected{};
    std::thread worker{[&] {
        rejected[0] = !pair.client->poll(pair.now);
        rejected[1] = !pair.client->send(MessageKind::probe, {});
        rejected[2] = !pair.client->receive();
        rejected[3] = !pair.client->stats();
        rejected[4] = !pair.client->close();
        rejected[5] = !pair.listener->port();
        rejected[6] = !pair.listener->accept(pair.now);
        rejected[7] = !pair.listener->close();
        rejected[8] = !pair.server->stats();
    }};
    worker.join();
    for (const bool value : rejected) {
        CHECK(value);
    }
    REQUIRE(pair.client->send(MessageKind::probe, {}));
    CHECK(pair.transfer(*pair.server).sequence == 2);
}

TEST_CASE("TLS listener connection admission is bounded and close releases capacity",
          "[net-transport]") {
    Pair pair{{}, 1};
    rejects(pair.listener->accept(pair.now), shark::ErrorCode::conflict);
    auto second = TlsTransport::connect_loopback(pair.listener->port().value(), trust(), pair.now);
    REQUIRE(second);
    rejects(pair.listener->accept(pair.now), shark::ErrorCode::conflict);
    REQUIRE(pair.server->close());
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    std::unique_ptr<TlsTransport> accepted;
    while (!accepted) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        auto result = pair.listener->accept(pair.now);
        REQUIRE(result);
        if (result.value()) {
            accepted = std::move(*result.value());
        }
        std::this_thread::yield();
    }
    REQUIRE(accepted->close());
    REQUIRE(pair.listener->close());
    rejects(pair.listener->port(), shark::ErrorCode::conflict);
    rejects(pair.listener->accept(pair.now), shark::ErrorCode::conflict);
}
