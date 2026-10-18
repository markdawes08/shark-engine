#include <shark/net/transport.hpp>
#include <shark/time.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using shark::net::MessageKind;
using shark::net::TlsTransport;
using shark::net::TransportState;
using Options = std::map<std::string, std::string>;

std::string file(const std::string& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error("Cannot read fixture PEM");
    }
    std::string value{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if (value.empty() || value.size() > 64 * 1024) {
        throw std::runtime_error("Fixture PEM is empty or oversized");
    }
    return value;
}

unsigned number(const Options& options, const char* key, unsigned fallback) {
    const auto found = options.find(key);
    if (found == options.end()) {
        return fallback;
    }
    unsigned value = 0;
    const auto& text = found->second;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::runtime_error("Invalid numeric fixture option");
    }
    return value;
}

void pause(shark::SteadyMonotonicClock& clock) {
    // Real nonblocking I/O cadence, not a deterministic fault-model sleep.
    auto result = clock.wait_for(1ms);
    if (!result) {
        throw std::runtime_error("Fixture wait failed");
    }
}

int client(const Options& options) {
    const auto port = number(options, "--port", 0);
    const auto tag = number(options, "--tag", 1);
    const bool expect_failure = number(options, "--expect-failure", 0) != 0;
    const bool expect_graceful = number(options, "--expect-graceful", 0) != 0;
    if (port == 0 || port > 65535 || tag > 255) {
        throw std::runtime_error("Invalid client fixture endpoint/tag");
    }
    shark::SteadyMonotonicClock clock;
    shark::net::TransportLimits limits;
    limits.handshake_timeout = std::chrono::milliseconds{number(options, "--handshake-ms", 2000)};
    shark::net::ClientTrust trust{file(options.at("--ca")), options.at("--name")};
    bool ready = false;
    auto failed = [&](const shark::Error& error) {
        std::cout << "{\"status\":\"" << (expect_failure ? "PASS" : "FAIL")
                  << "\",\"role\":\"client\",\"expected_failure\":"
                  << (expect_failure ? "true" : "false")
                  << ",\"ready\":" << (ready ? "true" : "false") << ",\"error_code\":\""
                  << shark::error_code_name(error.code) << "\",\"error_operation\":\""
                  << error.context.operation << "\"}\n";
        return expect_failure ? 0 : 1;
    };
    auto created = TlsTransport::connect_loopback(static_cast<std::uint16_t>(port), trust,
                                                  clock.now(), limits);
    if (!created) {
        return failed(created.error());
    }
    auto connection = std::move(created).value();
    std::vector<std::byte> outgoing(512, static_cast<std::byte>(tag));
    const auto expected = outgoing;
    bool sent = false;
    std::optional<shark::net::Frame> owned;
    bool drained_after_close = false;
    while (clock.now() < 8s) {
        auto polled = connection->poll(clock.now(), 16);
        if (!polled) {
            return failed(polled.error());
        }
        if (polled.value().operations > 16) {
            throw std::runtime_error("Transport exceeded poll work budget");
        }
        const bool closed = polled.value().state == TransportState::closed;
        if (polled.value().state == TransportState::ready || closed) {
            ready = ready || polled.value().state == TransportState::ready;
            if (!sent) {
                if (closed) {
                    return failed({shark::ErrorCode::io_failure, "Peer closed before negotiation",
                                   shark::ErrorContext{"net.fixture.peer-closed"}});
                }
                auto admitted = connection->send(MessageKind::probe, outgoing);
                if (!admitted) {
                    return failed(admitted.error());
                }
                std::fill(outgoing.begin(), outgoing.end(), std::byte{0});
                sent = true;
            }
            auto received = connection->receive();
            if (!received) {
                return failed(received.error());
            }
            if (received.value()) {
                const auto& frame = *received.value();
                if (expect_failure || owned || frame.kind != MessageKind::probe_reply ||
                    frame.payload != expected) {
                    throw std::runtime_error("Unexpected or corrupted probe reply");
                }
                owned = std::move(*received.value());
                drained_after_close = closed;
            }
            if (closed && !owned) {
                return failed({shark::ErrorCode::io_failure, "Peer closed without a reply",
                               shark::ErrorContext{"net.fixture.peer-closed"}});
            }
            if (owned && (!expect_graceful || closed)) {
                const auto snapshot = connection->stats();
                if (!snapshot || snapshot.value().frames_sent != 2 ||
                    snapshot.value().frames_received != 2 ||
                    snapshot.value().socket_bytes_sent <= expected.size() ||
                    snapshot.value().socket_bytes_received <= expected.size()) {
                    throw std::runtime_error("Missing negotiated frame or socket metrics");
                }
                if (!connection->close() || !connection->close()) {
                    throw std::runtime_error("Fixture close failed");
                }
                connection.reset();
                if (owned->payload != expected) {
                    throw std::runtime_error(
                        "Received payload did not survive transport destruction");
                }
                std::cout << "{\"status\":\"PASS\",\"role\":\"client\",\"ready\":true,"
                             "\"echoes\":1,\"owned_payload\":true,\"graceful_close\":"
                          << (closed ? "true" : "false")
                          << ",\"drained_after_close\":" << (drained_after_close ? "true" : "false")
                          << ",\"frames_sent\":" << snapshot.value().frames_sent
                          << ",\"frames_received\":" << snapshot.value().frames_received
                          << ",\"socket_bytes_sent\":" << snapshot.value().socket_bytes_sent
                          << ",\"socket_bytes_received\":" << snapshot.value().socket_bytes_received
                          << "}\n";
                return 0;
            }
        }
        pause(clock);
    }
    throw std::runtime_error("Client fixture watchdog expired without a transport outcome");
}

int server(const Options& options) {
    const auto clients = number(options, "--clients", 2);
    const auto expected_errors = number(options, "--expected-errors", 0);
    const auto behavior = options.contains("--behavior") ? options.at("--behavior") : "echo";
    if (clients == 0 || clients > 8 || expected_errors > clients ||
        (behavior != "echo" && behavior != "close" && behavior != "stall")) {
        throw std::runtime_error("Invalid server fixture configuration");
    }
    shark::net::ServerIdentity identity{file(options.at("--cert")), file(options.at("--key"))};
    auto created = shark::net::TlsListener::listen_loopback(0, identity);
    if (!created) {
        std::cerr << created.error().message << '\n';
        throw std::runtime_error("Fixture listener creation failed");
    }
    auto listener = std::move(created).value();
    auto port = listener->port();
    if (!port) {
        throw std::runtime_error("Cannot obtain fixture listener port");
    }
    {
        std::ofstream ready{options.at("--ready"), std::ios::trunc};
        ready << "{\"port\":" << port.value() << "}\n";
        ready.close();
        if (!ready) {
            throw std::runtime_error("Cannot publish fixture readiness");
        }
    }
    struct Peer {
        std::unique_ptr<TlsTransport> connection;
        unsigned replies{0};
    };
    std::vector<Peer> peers;
    unsigned accepted = 0;
    unsigned completed = 0;
    unsigned errors = 0;
    unsigned echoes = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    shark::SteadyMonotonicClock clock;
    std::optional<shark::MonotonicTime> stalled_at;
    while (clock.now() < 6s) {
        const auto now = clock.now();
        if (accepted < clients) {
            auto next = listener->accept(now);
            if (!next) {
                throw std::runtime_error("Fixture accept failed");
            }
            if (next.value()) {
                ++accepted;
                peers.push_back({std::move(*next.value()), 0});
                if (behavior == "close") {
                    if (!peers.back().connection->close()) {
                        throw std::runtime_error("Fixture peer close failed");
                    }
                    ++completed;
                    peers.back().connection.reset();
                } else if (behavior == "stall") {
                    stalled_at = now;
                }
            }
        }
        if (behavior == "stall") {
            if (stalled_at && now - *stalled_at >= 600ms) {
                completed = accepted;
            }
        } else if (behavior == "echo") {
            for (auto& peer : peers) {
                if (!peer.connection) {
                    continue;
                }
                auto polled = peer.connection->poll(now, 16);
                if (!polled || polled.value().state == TransportState::closed) {
                    const auto snapshot = peer.connection->stats();
                    if (!snapshot) {
                        throw std::runtime_error("Cannot inspect completed peer");
                    }
                    bytes_sent += snapshot.value().socket_bytes_sent;
                    bytes_received += snapshot.value().socket_bytes_received;
                    if (peer.replies == 1 && snapshot.value().frames_sent == 2) {
                        ++completed;
                    } else {
                        ++errors;
                    }
                    peer.connection.reset();
                    continue;
                }
                if (polled.value().operations > 16) {
                    throw std::runtime_error("Server exceeded poll work budget");
                }
                auto received = peer.connection->receive();
                if (!received) {
                    throw std::runtime_error("Fixture receive failed");
                }
                if (received.value()) {
                    if (received.value()->kind != MessageKind::probe || peer.replies != 0) {
                        throw std::runtime_error("Unexpected fixture probe kind or count");
                    }
                    auto sent =
                        peer.connection->send(MessageKind::probe_reply, received.value()->payload);
                    if (!sent) {
                        throw std::runtime_error("Fixture reply admission failed");
                    }
                    ++peer.replies;
                    ++echoes;
                }
            }
        }
        if (accepted == clients && completed + errors == clients) {
            if (errors != expected_errors ||
                (behavior == "echo" && echoes != clients - expected_errors)) {
                throw std::runtime_error("Unexpected server success/failure count");
            }
            std::cout << "{\"status\":\"PASS\",\"role\":\"server\",\"accepted\":" << accepted
                      << ",\"completed\":" << completed << ",\"errors\":" << errors
                      << ",\"echoes\":" << echoes << ",\"socket_bytes_sent\":" << bytes_sent
                      << ",\"socket_bytes_received\":" << bytes_received << "}\n";
            return 0;
        }
        pause(clock);
    }
    throw std::runtime_error("Server fixture watchdog expired");
}

} // namespace

int main(int argc, char** argv) {
    try {
        Options options;
        for (int index = 1; index < argc; index += 2) {
            if (index + 1 >= argc || !options.emplace(argv[index], argv[index + 1]).second) {
                throw std::runtime_error("Invalid fixture options");
            }
        }
        const auto mode = options.at("--mode");
        if (mode == "server") {
            return server(options);
        }
        if (mode == "client") {
            return client(options);
        }
        throw std::runtime_error("Unknown fixture mode");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::cout << "{\"status\":\"FAIL\",\"error\":\"network-fixture\"}\n";
        return 1;
    }
}
