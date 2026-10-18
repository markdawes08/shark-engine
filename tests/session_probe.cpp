#include <shark/net/session.hpp>
#include <shark/time.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace shark::net;
using Options = std::map<std::string, std::string>;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename T> T take(shark::Result<T> value) {
    if (!value) {
        throw std::runtime_error("Session fixture operation failed: " +
                                 value.error().context.operation);
    }
    return std::move(value).value();
}

void take(shark::Result<void> value) {
    if (!value) {
        throw std::runtime_error("Session fixture operation failed: " +
                                 value.error().context.operation);
    }
}

std::string file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    require(static_cast<bool>(input), "Cannot read explicit session fixture PEM");
    std::string value{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    require(!value.empty() && value.size() <= 64 * 1024, "Invalid fixture PEM size");
    return value;
}

unsigned number(const Options& options, const char* key) {
    const auto& text = options.at(key);
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    require(result.ec == std::errc{} && result.ptr == text.data() + text.size(),
            "Invalid numeric session fixture option");
    return value;
}

void cadence(shark::SteadyMonotonicClock& clock) {
    // Real process I/O cadence. Deterministic service tests use controllable time.
    take(clock.wait_for(1ms));
}

AccountId account(bool second) {
    return AccountId{take(shark::StableId::parse(second ? "00000000-0000-4000-8000-000000000102"
                                                        : "00000000-0000-4000-8000-000000000101"))};
}

CharacterId character(bool second) {
    return CharacterId{take(shark::StableId::parse(
        second ? "00000000-0000-4000-8000-000000000202" : "00000000-0000-4000-8000-000000000201"))};
}

SessionEnvelope envelope(const SessionSnapshot& snapshot, std::uint64_t sequence) {
    return {snapshot.session, snapshot.ownership_epoch, sequence, snapshot.character};
}

class TransportFailure final : public std::runtime_error {
  public:
    explicit TransportFailure(shark::Error value)
        : std::runtime_error("Session fixture transport failure"), error(std::move(value)) {}
    shark::Error error;
};

template <typename T> T transported(shark::Result<T> value) {
    if (!value) {
        throw TransportFailure{value.error()};
    }
    return std::move(value).value();
}

void transported(shark::Result<void> value) {
    if (!value) {
        throw TransportFailure{value.error()};
    }
}

class Client final {
  public:
    Client(const std::filesystem::path& fixtures, std::uint16_t port, const std::string& identity,
           shark::SteadyMonotonicClock& clock)
        : clock_(clock) {
        ClientTrust trust{file(fixtures / "ca.pem"), "shark.test"};
        TransportLimits limits;
        limits.handshake_timeout = 2s;
        if (identity == "absent") {
            transport_ =
                transported(TlsTransport::connect_loopback(port, trust, clock.now(), limits));
        } else {
            const std::map<std::string, std::string> certificates{
                {"a", "client-a.pem"},
                {"b", "client-b.pem"},
                {"expired", "expired-client.pem"},
                {"wrong-ca", "wrong-ca-client.pem"},
                {"wrong-eku", "wrong-eku-client.pem"},
                {"no-eku", "no-eku-client.pem"},
                {"wrong-ku", "wrong-ku-client.pem"},
                {"ca-client", "ca-client.pem"}};
            ClientIdentity credential{
                file(fixtures / "session" / certificates.at(identity)),
                file(fixtures / "session" /
                     (identity == "b" ? "client-b-key.pem" : "client-a-key.pem"))};
            transport_ = transported(TlsTransport::connect_loopback_authenticated(
                port, trust, credential, clock.now(), limits));
        }
    }

    void await_ready() {
        while (clock_.now() < 8s) {
            if (pump().state == TransportState::ready) {
                ready = true;
                return;
            }
            cadence(clock_);
        }
        throw std::runtime_error("Session client readiness watchdog expired");
    }

    SessionReply exchange(const SessionRequest& request) {
        return exchange_bytes(take(encode_session_request(request)));
    }

    SessionReply exchange_bytes(std::vector<std::byte> payload) {
        const auto original = payload;
        transported(transport_->send(MessageKind::session_request, payload));
        std::fill(payload.begin(), payload.end(), std::byte{0});
        while (clock_.now() < 8s) {
            pump();
            auto frame = transported(transport_->receive());
            if (frame) {
                require(frame->kind == MessageKind::session_reply,
                        "Session client received another message kind");
                auto reply = take(decode_session_reply(frame->payload));
                if (std::holds_alternative<SessionRejected>(reply)) {
                    const std::vector<std::byte> generic{std::byte{0}, std::byte{1}, std::byte{2},
                                                         std::byte{0}};
                    require(frame->payload == generic,
                            "Session rejection leaked more than generic status");
                }
                // Keep an independently owned result through local transport destruction.
                retained_ = reply;
                ++replies;
                require(!original.empty(), "Fixture request unexpectedly empty");
                return reply;
            }
            cadence(clock_);
        }
        throw std::runtime_error("Session client reply watchdog expired");
    }

    SessionSnapshot accepted(const SessionRequest& request) {
        auto reply = exchange(request);
        require(std::holds_alternative<SessionSnapshot>(reply), "Valid session request rejected");
        return std::get<SessionSnapshot>(std::move(reply));
    }

    void rejected(const SessionRequest& request) {
        require(std::holds_alternative<SessionRejected>(exchange(request)),
                "Unauthorized session request accepted");
        ++rejections;
    }

    SessionSnapshot login(bool second) {
        auto snapshot = accepted(LoginRequest{account(second), character(second)});
        require(snapshot.account == account(second) && snapshot.character == character(second) &&
                    snapshot.ownership_epoch > 0 && snapshot.last_sequence == 0 &&
                    snapshot.active && !snapshot.ready,
                "Login returned incorrect owned session state");
        return snapshot;
    }

    void close() {
        if (!transport_) {
            return;
        }
        const auto retained = retained_;
        transported(transport_->close());
        transported(transport_->close());
        transport_.reset();
        require(retained_ == retained, "Owned session reply did not survive transport destruction");
    }

    bool ready{false};
    unsigned replies{0};
    unsigned rejections{0};

  private:
    TransportStats pump() {
        auto result = transported(transport_->poll(clock_.now(), 16));
        require(result.operations <= 16, "Session fixture exceeded transport poll budget");
        if (result.state == TransportState::closed) {
            throw TransportFailure{{shark::ErrorCode::io_failure,
                                    "Peer closed session fixture",
                                    {.operation = "net.fixture.closed"}}};
        }
        return result;
    }

    shark::SteadyMonotonicClock& clock_;
    std::unique_ptr<TlsTransport> transport_;
    std::optional<SessionReply> retained_;
};

int client(const Options& options) {
    const auto port = number(options, "--port");
    require(port > 0 && port <= 65535, "Invalid session fixture port");
    const std::filesystem::path fixtures{options.at("--fixtures")};
    const auto& scenario = options.at("--scenario");
    const auto& identity = options.at("--identity");
    const bool credential_failure = scenario.starts_with("credential-");
    shark::SteadyMonotonicClock clock;
    bool ready = false;
    try {
        Client first{fixtures, static_cast<std::uint16_t>(port), identity, clock};
        first.await_ready();
        ready = true;
        require(!credential_failure, "Invalid credential reached session readiness");
        unsigned additional_replies = 0;
        unsigned additional_rejections = 0;
        bool fenced = false;
        if (scenario == "two-identities") {
            const auto snapshot = first.login(identity == "b");
            const auto prepared = first.accepted(SetReadyRequest{envelope(snapshot, 1), true});
            require(prepared.ready && prepared.last_sequence == 1 && prepared.active,
                    "SetReady did not update only the owned session preference");
            const auto heartbeat = first.accepted(HeartbeatRequest{envelope(snapshot, 2)});
            require(heartbeat.ready && heartbeat.last_sequence == 2 && heartbeat.active,
                    "Heartbeat returned incorrect session sequence/state");
            const auto closed = first.accepted(LogoutRequest{envelope(snapshot, 3)});
            require(!closed.active && closed.last_sequence == 3,
                    "Logout retained session authority");
        } else if (scenario == "unregistered" || scenario == "revoked") {
            first.rejected(LoginRequest{account(false), character(false)});
        } else if (scenario == "wrong-owner") {
            first.rejected(LoginRequest{account(true), character(true)});
            first.rejected(LoginRequest{account(false), character(true)});
        } else if (scenario == "replay") {
            const auto snapshot = first.login(false);
            first.accepted(HeartbeatRequest{envelope(snapshot, 1)});
            first.rejected(HeartbeatRequest{envelope(snapshot, 1)});
            const auto next = first.accepted(HeartbeatRequest{envelope(snapshot, 2)});
            require(next.last_sequence == 2, "Rejected replay advanced the session sequence");
        } else if (scenario == "expiry" || scenario == "live-revocation") {
            const auto snapshot = first.login(false);
            if (scenario == "expiry") {
                // Deliberately cross the configured 300ms real session lease.
                const auto after_lease = clock.now() + 450ms;
                while (clock.now() < after_lease) {
                    cadence(clock);
                }
            }
            first.rejected(HeartbeatRequest{envelope(snapshot, 1)});
        } else if (scenario == "rate-limit") {
            const auto snapshot = first.login(false);
            first.accepted(HeartbeatRequest{envelope(snapshot, 1)});
            first.accepted(HeartbeatRequest{envelope(snapshot, 2)});
            first.rejected(HeartbeatRequest{envelope(snapshot, 3)});
        } else if (scenario == "reconnect") {
            const auto old = first.login(false);
            Client replacement{fixtures, static_cast<std::uint16_t>(port), "a", clock};
            replacement.await_ready();
            const auto current = replacement.login(false);
            require(current.session != old.session && current.ownership_epoch > old.ownership_epoch,
                    "Reconnect reused the old session identity or ownership epoch");
            first.rejected(HeartbeatRequest{envelope(old, 1)});
            replacement.rejected(HeartbeatRequest{envelope(old, 1)});
            first.close();
            replacement.accepted(HeartbeatRequest{envelope(current, 1)});
            replacement.accepted(LogoutRequest{envelope(current, 2)});
            fenced = true;
            additional_replies = replacement.replies;
            additional_rejections = replacement.rejections;
            replacement.close();
        } else if (scenario == "cross-connection") {
            const auto a = first.login(false);
            Client other{fixtures, static_cast<std::uint16_t>(port), "b", clock};
            other.await_ready();
            const auto b = other.login(true);
            other.rejected(HeartbeatRequest{envelope(a, 1)});
            first.accepted(HeartbeatRequest{envelope(a, 1)});
            other.accepted(HeartbeatRequest{envelope(b, 1)});
            first.accepted(LogoutRequest{envelope(a, 2)});
            other.accepted(LogoutRequest{envelope(b, 2)});
            fenced = true;
            additional_replies = other.replies;
            additional_rejections = other.rejections;
            other.close();
        } else if (scenario == "malformed" || scenario == "version") {
            auto bytes =
                take(encode_session_request(LoginRequest{account(false), character(false)}));
            if (scenario == "malformed") {
                bytes.pop_back();
            } else {
                bytes[1] = std::byte{2}; // Independent exact version field, not an encoder option.
            }
            try {
                first.exchange_bytes(std::move(bytes));
                throw std::runtime_error("Malformed session input produced a reply");
            } catch (const TransportFailure&) {
                first.close();
                std::cout << "{\"status\":\"PASS\",\"role\":\"client\",\"ready\":true,"
                             "\"owned_results\":false,\"codec_rejected\":true}\n";
                return 0;
            }
        } else {
            throw std::runtime_error("Unknown session client scenario");
        }
        const auto replies = first.replies + additional_replies;
        const auto rejections = first.rejections + additional_rejections;
        first.close();
        std::cout << "{\"status\":\"PASS\",\"role\":\"client\",\"ready\":true,"
                     "\"owned_results\":true,\"replies\":"
                  << replies << ",\"rejections\":" << rejections
                  << ",\"fenced\":" << (fenced ? "true" : "false") << "}\n";
        return 0;
    } catch (const TransportFailure& error) {
        const auto& operation = error.error.context.operation;
        const bool expected = credential_failure && !ready &&
                              (operation == "net.tls.handshake" || operation == "net.tls.read" ||
                               operation == "net.tls.write" || operation == "net.tls.verify");
        std::cout << "{\"status\":\"" << (expected ? "PASS" : "FAIL")
                  << "\",\"role\":\"client\",\"ready\":" << (ready ? "true" : "false")
                  << ",\"expected_tls_rejection\":" << (expected ? "true" : "false")
                  << ",\"error_code\":\"" << shark::error_code_name(error.error.code)
                  << "\",\"error_operation\":\"" << error.error.context.operation << "\"}\n";
        return expected ? 0 : 1;
    }
}

int server(const Options& options) {
    const auto expected_connections = number(options, "--connections");
    require(expected_connections > 0 && expected_connections <= 4,
            "Invalid fixture connection count");
    const std::filesystem::path fixtures{options.at("--fixtures")};
    const auto& scenario = options.at("--scenario");
    SessionLimits limits;
    if (scenario == "expiry") {
        limits.session_lifetime = 300ms;
        limits.session_idle_timeout = 300ms;
    } else if (scenario == "rate-limit") {
        limits.commands_per_window = 2;
        limits.command_window = 5s;
    }
    auto authority = take(SessionAuthority::create(limits));
    for (const bool second : {false, true}) {
        take(authority->provision_account(account(second)));
        take(authority->assign_character(account(second), character(second)));
    }
    const auto fingerprint_a =
        take(certificate_fingerprint(file(fixtures / "session/client-a.pem")));
    const auto fingerprint_b =
        take(certificate_fingerprint(file(fixtures / "session/client-b.pem")));
    if (scenario != "unregistered") {
        take(authority->enroll_certificate(account(false), fingerprint_a));
    }
    take(authority->enroll_certificate(account(true), fingerprint_b));
    if (scenario == "revoked") {
        take(authority->revoke_certificate(fingerprint_a));
    }
    ServerIdentity identity{file(fixtures / "server.pem"), file(fixtures / "server-key.pem")};
    auto listener =
        take(TlsListener::listen_loopback_authenticated(0, identity, file(fixtures / "ca.pem")));
    {
        std::ofstream ready{options.at("--ready"), std::ios::trunc};
        ready << "{\"port\":" << take(listener->port()) << "}\n";
        ready.close();
        require(static_cast<bool>(ready), "Cannot publish session fixture readiness");
    }
    struct Peer {
        std::unique_ptr<TlsTransport> transport;
        bool ready{false};
        unsigned replies{0};
        std::optional<SessionId> session;
    };
    std::vector<Peer> peers;
    std::set<CertificateFingerprint> credentials;
    std::set<std::uint64_t> connection_ids;
    unsigned accepted = 0;
    unsigned completed = 0;
    unsigned authenticated = 0;
    unsigned tls_rejections = 0;
    std::vector<std::string> tls_rejection_operations;
    unsigned codec_rejections = 0;
    unsigned requests = 0;
    unsigned successful = 0;
    unsigned rejected = 0;
    shark::SteadyMonotonicClock clock;
    while (clock.now() < 10s) {
        const auto now = clock.now();
        if (accepted < expected_connections) {
            auto next = take(listener->accept(now));
            if (next) {
                peers.push_back({std::move(*next), false, 0, std::nullopt});
                ++accepted;
            }
        }
        for (auto& peer : peers) {
            if (!peer.transport) {
                continue;
            }
            auto polled = peer.transport->poll(now, 16);
            if (!polled || polled.value().state == TransportState::closed) {
                if (!peer.ready) {
                    require(!polled, "Unauthenticated peer closed without a TLS rejection");
                    const auto& operation = polled.error().context.operation;
                    if (operation != "net.tls.handshake" && operation != "net.tls.client-profile") {
                        throw std::runtime_error("Preready peer failed outside TLS validation: " +
                                                 operation);
                    }
                    tls_rejection_operations.push_back(operation);
                    ++tls_rejections;
                } else {
                    require(peer.replies > 0, "Authenticated peer closed before a typed request");
                }
                if (peer.session) {
                    // An old connection's teardown must not revoke a replacement session.
                    take(authority->revoke_session(*peer.session));
                }
                peer.transport.reset();
                ++completed;
                continue;
            }
            require(polled.value().operations <= 16, "Server exceeded session poll budget");
            if (polled.value().state != TransportState::ready) {
                continue;
            }
            if (!peer.ready) {
                const auto proven = take(peer.transport->authenticated_peer());
                require(proven.connection_id != 0 &&
                            connection_ids.insert(proven.connection_id).second,
                        "Transport reused an authenticated connection identity");
                credentials.insert(proven.certificate);
                peer.ready = true;
                ++authenticated;
            }
            auto received = take(peer.transport->receive());
            if (!received) {
                continue;
            }
            require(received->kind == MessageKind::session_request,
                    "Unexpected session request kind");
            auto decoded = decode_session_request(received->payload);
            if (!decoded) {
                ++codec_rejections;
                take(peer.transport->close());
                peer.transport.reset();
                ++completed;
                continue;
            }
            ++requests;
            auto applied = authority->apply(*peer.transport, decoded.value(), now);
            if (applied) {
                ++successful;
                peer.session = applied.value().session;
            } else {
                ++rejected;
            }
            const auto reply = public_session_reply(applied);
            const auto bytes = take(encode_session_reply(reply));
            take(peer.transport->send(MessageKind::session_reply, bytes));
            ++peer.replies;
            if (scenario == "live-revocation" && applied &&
                std::holds_alternative<LoginRequest>(decoded.value())) {
                take(authority->revoke_certificate(fingerprint_a));
            }
        }
        if (accepted == expected_connections && completed == expected_connections) {
            const auto stats = take(authority->stats());
            require(stats.live_sessions == 0, "Disconnected fixture retained live authority");
            std::cout << "{\"status\":\"PASS\",\"role\":\"server\",\"accepted_connections\":"
                      << accepted << ",\"authenticated_connections\":" << authenticated
                      << ",\"distinct_credentials\":" << credentials.size()
                      << ",\"tls_rejections\":" << tls_rejections
                      << ",\"tls_rejection_operations\":[";
            for (std::size_t index = 0; index < tls_rejection_operations.size(); ++index) {
                std::cout << (index == 0 ? "" : ",") << '"' << tls_rejection_operations[index]
                          << '"';
            }
            std::cout << "],\"codec_rejections\":" << codec_rejections
                      << ",\"requests\":" << requests << ",\"successful_requests\":" << successful
                      << ",\"rejected_requests\":" << rejected << ",\"live_sessions\":0}\n";
            return 0;
        }
        cadence(clock);
    }
    throw std::runtime_error("Session server fixture watchdog expired");
}

} // namespace

int main(int argc, char** argv) {
    try {
        Options options;
        for (int index = 1; index < argc; index += 2) {
            require(index + 1 < argc && options.emplace(argv[index], argv[index + 1]).second,
                    "Invalid session fixture options");
        }
        const auto& mode = options.at("--mode");
        if (mode == "client") {
            return client(options);
        }
        if (mode == "server") {
            return server(options);
        }
        throw std::runtime_error("Unknown session fixture mode");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::cout << "{\"status\":\"FAIL\",\"error\":\"session-fixture\"}\n";
        return 1;
    }
}
