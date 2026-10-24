#include <shark/storage/characters.hpp>
#include <shark/time.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace shark;
using namespace shark::net;
using namespace shark::storage;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using Options = std::map<std::string, std::string>;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error{message};
}
template <typename T> T take(Result<T> result) {
    if (!result)
        throw std::runtime_error{result.error().context.operation + ":" +
                                 std::string{error_code_name(result.error().code)}};
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result)
        throw std::runtime_error{result.error().context.operation + ":" +
                                 std::string{error_code_name(result.error().code)}};
}
template <typename T> void reject(const Result<T>& result) {
    require(!result, "Expected character rejection was admitted");
    require(!result.error().message.empty() && !result.error().context.operation.empty(),
            "Rejection omitted diagnostics");
}
fs::path path(const std::string& text) {
    return fs::path{std::u8string{text.begin(), text.end()}};
}
std::string text(const fs::path& value) {
    const auto bytes = value.u8string();
    return {bytes.begin(), bytes.end()};
}
std::string file(const fs::path& value) {
    std::ifstream input{value, std::ios::binary};
    require(static_cast<bool>(input), "Cannot read fixture input");
    std::string result{std::istreambuf_iterator<char>{input}, {}};
    require(!result.empty() && result.size() <= 65536, "Invalid fixture input size");
    return result;
}
void write(const fs::path& destination, const std::string& content) {
    if (fs::exists(destination)) {
        require(file(destination) == content, "Fixture attempted to replace published discovery");
        return;
    }
    auto temporary = destination;
    temporary += ".partial";
    std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
    output << content;
    output.close();
    require(static_cast<bool>(output), "Cannot write fixture result");
    fs::rename(temporary, destination);
}
WorldId world() {
    return WorldId{take(StableId::parse("00000000-0000-4000-8000-000000000001"))};
}
AccountId account(bool second = false) {
    return AccountId{take(StableId::parse(second ? "00000000-0000-4000-8000-000000000102"
                                                 : "00000000-0000-4000-8000-000000000101"))};
}
CharacterState state(unsigned version) {
    return {Name{version == 1   ? "Initial character"
                 : version == 2 ? "Saved character"
                                : "Replacement character"},
            {world(),
             {static_cast<std::int64_t>(version) * 123456, -17, 4},
             {static_cast<double>(version) + 0.125, 42.5, 1023.75}}};
}
SessionEnvelope envelope(const SessionSnapshot& session, std::uint64_t sequence) {
    return {session.session, session.ownership_epoch, sequence, session.character};
}
fs::path discovery(const Options& options, bool second) {
    return path(options.at("--discovery") + (second ? "-b.txt" : "-a.txt"));
}
void cadence(SteadyMonotonicClock& clock) {
    take(clock.wait_for(1ms));
}

class Client final {
  public:
    Client(const Options& options, bool second, SteadyMonotonicClock& clock) : clock_(clock) {
        unsigned port = 0;
        const auto& value = options.at("--port");
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
        require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && port > 0 &&
                    port <= 65535,
                "Invalid port");
        const auto fixtures = path(options.at("--fixtures"));
        transport_ = take(TlsTransport::connect_loopback_authenticated(
            static_cast<std::uint16_t>(port), {file(fixtures / "ca.pem"), "shark.test"},
            {file(fixtures / (second ? "session/client-b.pem" : "session/client-a.pem")),
             file(fixtures / (second ? "session/client-b-key.pem" : "session/client-a-key.pem"))},
            clock_.now()));
        const auto deadline = clock_.now() + 5s;
        while (clock_.now() < deadline) {
            if (take(transport_->poll(clock_.now(), 16)).state == TransportState::ready)
                return;
            cadence(clock_);
        }
        throw std::runtime_error{"Client TLS readiness timed out"};
    }
    SessionReply exchange(const SessionRequest& request) {
        take(transport_->send(MessageKind::session_request, take(encode_session_request(request))));
        const auto deadline = clock_.now() + 5s;
        while (clock_.now() < deadline) {
            const auto polled = take(transport_->poll(clock_.now(), 16));
            require(polled.operations <= 16, "Client poll exceeded budget");
            auto received = take(transport_->receive());
            if (received) {
                require(received->kind == MessageKind::session_reply, "Unexpected reply kind");
                auto reply = take(decode_session_reply(received->payload));
                if (std::holds_alternative<SessionRejected>(reply)) {
                    require(received->payload == take(encode_session_reply(SessionRejected{})),
                            "Rejection leaked private durable data");
                    ++rejections;
                } else
                    ++accepted;
                return reply;
            }
            require(polled.state != TransportState::closed, "Server closed before reply");
            cadence(clock_);
        }
        throw std::runtime_error{"Character reply timed out"};
    }
    SessionSnapshot accept(const SessionRequest& request) {
        auto reply = exchange(request);
        require(std::holds_alternative<SessionSnapshot>(reply), "Expected accepted session reply");
        return std::get<SessionSnapshot>(reply);
    }
    void deny(const SessionRequest& request) {
        require(std::holds_alternative<SessionRejected>(exchange(request)),
                "Expected generic rejection");
    }
    void close() {
        if (transport_) {
            take(transport_->close());
            transport_.reset();
        }
    }
    unsigned accepted{0};
    unsigned rejections{0};

  private:
    SteadyMonotonicClock& clock_;
    std::unique_ptr<TlsTransport> transport_;
};

CharacterId discovered(const Options& options, bool second, SteadyMonotonicClock& clock) {
    const auto destination = discovery(options, second);
    const auto deadline = clock.now() + 5s;
    while (clock.now() < deadline) {
        std::ifstream input{destination};
        std::string value;
        if (input >> value) {
            const auto parsed = StableId::parse(value);
            if (parsed)
                return CharacterId{parsed.value()};
        }
        cadence(clock);
    }
    throw std::runtime_error{"Fixture character discovery timed out"};
}
SessionSnapshot login(Client& client, AccountId owner, CharacterId character) {
    const auto session = client.accept(LoginRequest{owner, character});
    require(session.account == owner && session.character == character && session.active &&
                !session.ready && session.last_sequence == 0,
            "Login returned incorrect identity");
    return client.accept(SetReadyRequest{envelope(session, 1), true});
}
SessionRequest old_request(const Options& options) {
    const auto bytes = file(path(options.at("--old-envelope")));
    return take(
        decode_session_request({reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()}));
}
int client(const Options& options) {
    const auto& scenario = options.at("--scenario");
    const bool second = options.at("--identity") == "b";
    SteadyMonotonicClock clock;
    Client first{options, second, clock};
    const auto character = discovered(options, second, clock);
    unsigned extra_accepted = 0;
    unsigned extra_rejected = 0;
    if (scenario == "restart-denied") {
        first.deny(LoginRequest{account(second), character});
        first.deny(old_request(options));
    } else {
        if (scenario == "restart-read") {
            if (second) {
                const auto foreign = discovered(options, false, clock);
                first.deny(LoginRequest{account(), foreign});
                first.deny(LoginRequest{account(true), foreign});
            } else
                first.deny(old_request(options));
        }
        const auto initial = login(first, account(second), character);
        first.accept(HeartbeatRequest{envelope(initial, 2)});
        if (scenario == "create-save") {
            first.accept(HeartbeatRequest{envelope(initial, 3)});
            first.deny(HeartbeatRequest{envelope(initial, 4)});
            const auto after_conflict = first.accept(HeartbeatRequest{envelope(initial, 5)});
            require(after_conflict.last_sequence == 5,
                    "Admitted revision conflict did not consume the sequence");
            if (!second) {
                const auto bytes =
                    take(encode_session_request(HeartbeatRequest{envelope(initial, 6)}));
                write(path(options.at("--old-envelope")),
                      {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
            }
            first.accept(LogoutRequest{envelope(initial, 6)});
            first.deny(HeartbeatRequest{envelope(initial, 7)});
        } else if (scenario == "restart-read") {
            first.accept(LogoutRequest{envelope(initial, 3)});
        } else if (scenario == "replacement") {
            Client replacement{options, false, clock};
            const auto current = login(replacement, account(), character);
            require(current.session != initial.session &&
                        current.ownership_epoch > initial.ownership_epoch,
                    "Replacement reused session ownership");
            first.deny(HeartbeatRequest{envelope(initial, 3)});
            replacement.deny(HeartbeatRequest{envelope(initial, 3)});
            first.close();
            replacement.accept(HeartbeatRequest{envelope(current, 2)});
            replacement.accept(HeartbeatRequest{envelope(current, 3)});
            replacement.accept(LogoutRequest{envelope(current, 4)});
            extra_accepted = replacement.accepted;
            extra_rejected = replacement.rejections;
            replacement.close();
        } else if (scenario == "logout-expiry") {
            first.accept(LogoutRequest{envelope(initial, 3)});
            first.deny(HeartbeatRequest{envelope(initial, 4)});
            Client replacement{options, false, clock};
            const auto current = login(replacement, account(), character);
            replacement.accept(HeartbeatRequest{envelope(current, 2)});
            replacement.deny(HeartbeatRequest{envelope(current, 3)});
            extra_accepted = replacement.accepted;
            extra_rejected = replacement.rejections;
            replacement.close();
        } else if (scenario == "revoke-certificate" || scenario == "revoke-account") {
            first.deny(HeartbeatRequest{envelope(initial, 3)});
            Client replacement{options, second, clock};
            replacement.deny(LoginRequest{account(second), character});
            extra_accepted = replacement.accepted;
            extra_rejected = replacement.rejections;
            replacement.close();
        } else
            throw std::runtime_error{"Unknown character scenario"};
    }
    first.close();
    std::cout << "{\"status\":\"PASS\",\"role\":\"client\",\"scenario\":\"" << scenario
              << "\",\"identity\":\"" << (second ? "b" : "a")
              << "\",\"authenticated\":true,\"accepted\":" << first.accepted + extra_accepted
              << ",\"rejected\":" << first.rejections + extra_rejected << "}\n";
    return 0;
}

struct Peer {
    std::unique_ptr<TlsTransport> transport;
    bool ready{false};
    bool second{false};
    unsigned ordinal{0};
    unsigned heartbeats{0};
    std::optional<SessionSnapshot> session{};
};

void print_record(const CharacterRecord& record) {
    const auto& position = record.state.position;
    std::cout << "{\"account_id\":\"" << record.account.value().to_string()
              << "\",\"character_id\":\"" << record.character.value().to_string()
              << "\",\"entity_id\":\"" << record.entity.value().to_string() << "\",\"world_id\":\""
              << position.world.value().to_string() << "\",\"revision\":" << record.revision
              << ",\"name\":\"" << record.state.name.value << "\",\"cell\":[" << position.cell.x
              << ',' << position.cell.y << ',' << position.cell.z << "],\"offset\":["
              << std::setprecision(17) << position.offset[0] << ',' << position.offset[1] << ','
              << position.offset[2] << "]}";
}
int server(const Options& options) {
    const auto& scenario = options.at("--scenario");
    const auto database = path(options.at("--database"));
    const auto fixtures = path(options.at("--fixtures"));
    auto store = take(scenario == "create-save" ? Store::create(database, world())
                                                : Store::open(database, world()));
    const auto identity = take(store->metadata());
    std::map<AccountId, CharacterRecord> records;
    for (const auto& record : take(store->registry()).characters)
        records.emplace(record.account, record);
    SessionLimits limits;
    limits.auth_attempts_per_window = 128;
    limits.commands_per_window = 128;
    if (scenario == "logout-expiry") {
        limits.session_lifetime = 10ns;
        limits.session_idle_timeout = 10ns;
    }
    auto service = take(CharacterAuthority::create(std::move(store), limits));
    const std::array fingerprints{
        take(certificate_fingerprint(file(fixtures / "session/client-a.pem"))),
        take(certificate_fingerprint(file(fixtures / "session/client-b.pem")))};
    if (scenario == "create-save") {
        for (const bool second : {false, true}) {
            take(service->provision_account(account(second)));
            take(service->enroll_certificate(account(second), fingerprints[second ? 1U : 0U]));
        }
    }
    auto listener = take(TlsListener::listen_loopback_authenticated(
        0, {file(fixtures / "server.pem"), file(fixtures / "server-key.pem")},
        file(fixtures / "ca.pem")));
    write(path(options.at("--ready")), std::to_string(take(listener->port())) + "\n");
    SteadyMonotonicClock clock;
    MonotonicTime service_now{0ns};
    std::vector<Peer> peers;
    std::array<unsigned, 2> account_connections{};
    unsigned authenticated = 0, completed = 0, successful = 0, rejected = 0;
    unsigned created = 0, loaded = 0, saved = 0, roster_conflicts = 0, admission_rejections = 0;
    unsigned stale_account_rejections = 0;
    while (clock.now() < 18s) {
        if (peers.size() < 2) {
            auto accepted = take(listener->accept(clock.now()));
            if (accepted)
                peers.push_back({std::move(*accepted)});
        }
        for (auto& peer : peers) {
            if (!peer.transport)
                continue;
            auto polled = peer.transport->poll(clock.now(), 16);
            if (!polled || polled.value().state == TransportState::closed) {
                require(peer.ready, "Connection failed before authenticated readiness");
                if (peer.session)
                    take(service->revoke_session(peer.session->session));
                peer.transport.reset();
                ++completed;
                continue;
            }
            require(polled.value().operations <= 16, "Server poll exceeded budget");
            if (polled.value().state != TransportState::ready)
                continue;
            if (!peer.ready) {
                const auto proven = take(peer.transport->authenticated_peer());
                require(proven.certificate == fingerprints[0] ||
                            proven.certificate == fingerprints[1],
                        "Unexpected fixture certificate");
                peer.second = proven.certificate == fingerprints[1];
                peer.ordinal = account_connections[peer.second ? 1U : 0U]++;
                auto roster = service->roster(*peer.transport, account(peer.second), service_now);
                const bool denied =
                    scenario == "restart-denied" ||
                    ((scenario == "revoke-certificate" || scenario == "revoke-account") &&
                     peer.ordinal == 1);
                if (denied) {
                    reject(roster);
                    ++admission_rejections;
                } else {
                    auto owned = take(std::move(roster));
                    if (scenario == "create-save") {
                        require(owned.characters.empty() && owned.revision == 1,
                                "Initial roster not empty");
                        const auto record =
                            take(service->create_character(*peer.transport, account(peer.second),
                                                           owned.revision, state(1), service_now));
                        records.emplace(record.account, record);
                        ++created;
                        reject(service->create_character(*peer.transport, account(peer.second),
                                                         owned.revision, state(1), service_now));
                        ++roster_conflicts;
                    } else {
                        require(owned.characters.size() == 1 &&
                                    owned.characters.front() == records.at(account(peer.second)),
                                "Restored roster differs from durable records");
                    }
                }
                // Test-only local discovery carries generated IDs, never player data over a forged
                // wire kind. The actual roster/create above is admitted by this server-side live
                // mTLS connection.
                write(discovery(options, peer.second),
                      records.at(account(peer.second)).character.value().to_string() + "\n");
                peer.ready = true;
                ++authenticated;
            }
            auto received = take(peer.transport->receive());
            if (!received)
                continue;
            require(received->kind == MessageKind::session_request,
                    "Unexpected character fixture wire kind");
            const auto request = take(decode_session_request(received->payload));
            Result<SessionSnapshot> outcome = Error{ErrorCode::execution_failed,
                                                    "Unprocessed fixture request",
                                                    {.operation = "fixture.character"}};
            bool revoke_after = false;
            if (const auto* heartbeat = std::get_if<HeartbeatRequest>(&request)) {
                auto& record = records.at(account(peer.second));
                const bool owned_envelope =
                    peer.session && heartbeat->authority.session == peer.session->session;
                if (scenario == "replacement" && peer.ordinal == 0 && peer.heartbeats == 1) {
                    reject(service->roster(*peer.transport, account(), service_now));
                    reject(service->create_character(*peer.transport, account(), 2, state(1),
                                                     service_now));
                    stale_account_rejections += 2;
                }
                if (scenario == "logout-expiry" && peer.ordinal == 1 && peer.heartbeats == 1)
                    service_now = 10ns;
                bool do_save = !owned_envelope;
                std::uint64_t revision = record.revision;
                unsigned version = 3;
                if (owned_envelope) {
                    if (scenario == "create-save") {
                        do_save =
                            peer.heartbeats == 1 || peer.heartbeats == 2 || peer.heartbeats >= 4;
                        version = 2;
                        if (peer.heartbeats == 2)
                            revision = 1;
                    } else if (scenario == "replacement")
                        do_save = peer.ordinal == 0 ? peer.heartbeats != 0 : peer.heartbeats == 0;
                    else if (scenario == "revoke-certificate" || scenario == "revoke-account") {
                        do_save = peer.heartbeats != 0;
                        revoke_after = peer.heartbeats == 0;
                    } else if (scenario == "logout-expiry")
                        do_save = peer.heartbeats != 0;
                }
                auto result =
                    do_save ? service->save(*peer.transport, heartbeat->authority, revision,
                                            state(version), service_now)
                            : service->load(*peer.transport, heartbeat->authority, service_now);
                if (result) {
                    require(peer.session.has_value(),
                            "Character operation admitted without a fixture login");
                    if (do_save) {
                        require(result.value().revision == record.revision + 1 &&
                                    result.value().state == state(version),
                                "Saved checkpoint differs");
                        record = result.value();
                        ++saved;
                    } else {
                        require(result.value() == record, "Loaded checkpoint differs");
                        ++loaded;
                    }
                    auto snapshot = *peer.session;
                    snapshot.last_sequence = heartbeat->authority.sequence;
                    outcome = snapshot;
                } else
                    outcome = result.error();
                if (owned_envelope)
                    ++peer.heartbeats;
            } else
                outcome = service->apply(*peer.transport, request, service_now);
            if (outcome) {
                peer.session = outcome.value();
                ++successful;
            } else {
                reject(outcome);
                ++rejected;
            }
            take(peer.transport->send(MessageKind::session_reply,
                                      take(encode_session_reply(public_session_reply(outcome)))));
            if (revoke_after && outcome) {
                if (scenario == "revoke-certificate")
                    take(service->revoke_certificate(fingerprints[0]));
                else
                    take(service->revoke_account(account(true)));
            }
        }
        if (peers.size() == 2 && completed == 2)
            break;
        cadence(clock);
    }
    require(completed == 2 && authenticated == 2,
            "Character fixture did not complete both authenticated connections");
    service.reset();
    store = take(Store::open(database, world()));
    require(take(store->metadata()) == identity,
            "Store identity changed during character operations");
    const auto registry = take(store->registry());
    require(registry.accounts.size() == 2 && registry.certificates.size() == 2 &&
                registry.characters.size() == 2,
            "Durable registry count changed");
    for (const auto& record : registry.characters)
        require(record == records.at(record.account),
                "Closed store differs from acknowledged state");
    std::cout << "{\"status\":\"PASS\",\"role\":\"server\",\"scenario\":\"" << scenario
              << "\",\"store_id\":\"" << identity.id.value().to_string() << "\",\"world_id\":\""
              << identity.world.value().to_string()
              << "\",\"schema_version\":" << identity.schema_version
              << ",\"authenticated\":" << authenticated << ",\"accepted\":" << successful
              << ",\"rejected\":" << rejected << ",\"created\":" << created
              << ",\"loaded\":" << loaded << ",\"saved\":" << saved
              << ",\"roster_conflicts\":" << roster_conflicts
              << ",\"admission_rejections\":" << admission_rejections
              << ",\"stale_account_rejections\":" << stale_account_rejections << ",\"records\":[";
    bool comma = false;
    for (const auto& [owner, record] : records) {
        static_cast<void>(owner);
        if (comma)
            std::cout << ',';
        print_record(record);
        comma = true;
    }
    std::cout << "],\"accounts\":[";
    comma = false;
    for (const auto& value : registry.accounts) {
        if (comma)
            std::cout << ',';
        std::cout << "{\"account_id\":\"" << value.account.value().to_string()
                  << "\",\"revoked\":" << (value.revoked ? "true" : "false")
                  << ",\"roster_revision\":" << value.roster_revision << '}';
        comma = true;
    }
    std::cout << "],\"certificates\":[";
    comma = false;
    for (const auto& value : registry.certificates) {
        if (comma)
            std::cout << ',';
        std::cout << "{\"account_id\":\"" << value.account.value().to_string()
                  << "\",\"revoked\":" << (value.revoked ? "true" : "false") << '}';
        comma = true;
    }
    std::cout << "]}\n";
    return 0;
}
} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        Options options;
        for (int index = 1; index < argc; index += 2) {
            require(
                index + 1 < argc &&
                    options.emplace(text(fs::path{argv[index]}), text(fs::path{argv[index + 1]}))
                        .second,
                "Invalid fixture options");
        }
        if (options.at("--mode") == "server")
            return server(options);
        if (options.at("--mode") == "client")
            return client(options);
        throw std::runtime_error{"Unknown character fixture role"};
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        std::cout << "{\"status\":\"FAIL\",\"error\":\"storage-character-fixture\"}\n";
        return 1;
    }
}
