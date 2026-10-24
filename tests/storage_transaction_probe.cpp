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
int client(const Options& options) {
    const bool second = options.at("--identity") == "b";
    const bool initial = options.at("--scenario") == "race-expire";
    SteadyMonotonicClock clock;
    Client connection{options, second, clock};
    const auto session = login(connection, account(second), discovered(options, second, clock));
    const unsigned stages = initial ? 17U : 5U;
    for (unsigned stage = 0; stage < stages; ++stage) {
        const SessionRequest request = HeartbeatRequest{envelope(session, stage + 2U)};
        const bool denied = initial ? stage == 2 || stage == 13 : stage == 3 || stage == 4;
        if (denied)
            connection.deny(request);
        else {
            const auto result = connection.accept(request);
            require(result.account == account(second) && result.character == session.character &&
                        result.last_sequence == stage + 2U && result.ready,
                    "Transaction stage acknowledgement differs");
        }
    }
    connection.close();
    std::cout << "{\"status\":\"PASS\",\"role\":\"client\",\"scenario\":\""
              << options.at("--scenario") << "\",\"identity\":\"" << options.at("--identity")
              << "\",\"authenticated\":true,\"accepted\":" << connection.accepted
              << ",\"rejected\":" << connection.rejections << "}\n";
    return 0;
}

struct Peer {
    std::unique_ptr<TlsTransport> transport;
    bool ready{false};
    bool second{false};
    unsigned stage{0};
    std::optional<SessionSnapshot> session{};
    std::optional<SessionEnvelope> pending{};
};
void print_economy(const EconomyRecord& record) {
    std::cout << "{\"account_id\":\"" << record.account.value().to_string()
              << "\",\"character_id\":\"" << record.character.value().to_string()
              << "\",\"revision\":" << record.revision << ",\"currency\":" << record.state.currency
              << ",\"item_units\":" << record.state.item_units << '}';
}
void print_receipt(const TransferReceipt& receipt) {
    require(receipt.status == TransferStatus::committed ||
                receipt.status == TransferStatus::stale_revision,
            "Unexpected fixture receipt status");
    const auto& request = receipt.request;
    std::cout << "{\"store_id\":\"" << request.operation.store.value().to_string()
              << "\",\"actor\":\"" << request.operation.actor.value().to_string()
              << "\",\"sequence\":" << request.operation.sequence << ",\"source_id\":\""
              << request.source.value().to_string() << "\",\"target_id\":\""
              << request.target.value().to_string()
              << "\",\"expected_source_revision\":" << request.expected_source_revision
              << ",\"expected_target_revision\":" << request.expected_target_revision
              << ",\"currency\":" << request.quantity.currency
              << ",\"item_units\":" << request.quantity.item_units << ",\"status\":\""
              << (receipt.status == TransferStatus::committed ? "committed" : "stale_revision")
              << "\",\"source\":";
    require(receipt.source.has_value() && receipt.target.has_value(), "Receipt omitted records");
    print_economy(*receipt.source);
    std::cout << ",\"target\":";
    print_economy(*receipt.target);
    std::cout << '}';
}
int server(const Options& options) {
    const auto& scenario = options.at("--scenario");
    const bool initial = scenario == "race-expire";
    require(initial || scenario == "restart-read", "Unknown transaction scenario");
    const auto database = path(options.at("--database"));
    const auto fixtures = path(options.at("--fixtures"));
    auto store = take(initial ? Store::create(database, world()) : Store::open(database, world()));
    const auto identity = take(store->metadata());
    std::map<AccountId, CharacterRecord> records;
    std::map<AccountId, OperationStream> restored;
    for (const auto& record : take(store->registry()).characters) {
        records.emplace(record.account, record);
        restored.emplace(record.account, take(store->operation_stream(record.account)));
    }
    SessionLimits limits;
    limits.auth_attempts_per_window = 128;
    limits.commands_per_window = 128;
    auto service = take(CharacterAuthority::create(std::move(store), limits));
    const std::array fingerprints{
        take(certificate_fingerprint(file(fixtures / "session/client-a.pem"))),
        take(certificate_fingerprint(file(fixtures / "session/client-b.pem")))};
    if (initial) {
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
    const MonotonicTime service_now{0ns};
    std::vector<Peer> peers;
    std::map<AccountId, TransferReceipt> first_receipts;
    std::map<AccountId, TransferReceipt> last_receipts;
    unsigned authenticated = 0, completed = 0, accepted_count = 0, rejected_count = 0;
    unsigned committed = 0, stale = 0, replayed = 0, mismatch = 0, expired = 0;
    unsigned economy_queries = 0, stream_queries = 0, simultaneous_pending = 0;
    auto request_for = [&](bool second, std::uint64_t sequence) {
        return TransferRequest{{identity.id, account(second), sequence},
                               records.at(account(second)).character,
                               records.at(account(!second)).character,
                               1,
                               1,
                               {7, 2}};
    };
    auto expected_economy = [&](bool second) {
        return EconomyRecord{account(second), records.at(account(second)).character, 2,
                             second ? EconomyState{107, 22} : EconomyState{93, 18}};
    };
    auto respond = [&](Peer& peer, Result<SessionSnapshot> outcome) {
        if (outcome) {
            peer.session = outcome.value();
            ++accepted_count;
        } else {
            reject(outcome);
            ++rejected_count;
        }
        take(peer.transport->send(MessageKind::session_reply,
                                  take(encode_session_reply(public_session_reply(outcome)))));
    };
    auto heartbeat = [&](Peer& peer, const SessionEnvelope& authority) {
        require(peer.session.has_value(), "Transaction stage before login");
        const auto stage = peer.stage++;
        const bool query_economy = initial ? stage == 4 || stage == 16 : stage == 0;
        const bool query_stream = initial ? stage == 3 || stage == 15 : stage == 1;
        std::optional<Error> failure;
        if (query_economy) {
            const auto record = take(service->economy(*peer.transport, authority, service_now));
            require(record == expected_economy(peer.second), "Economy conservation differs");
            ++economy_queries;
        } else if (query_stream) {
            const auto stream =
                take(service->operation_stream(*peer.transport, authority, service_now));
            const auto high = initial && stage == 3 ? 1U : 9U;
            require(stream.store == identity.id && stream.actor == account(peer.second) &&
                        stream.high_watermark == high &&
                        stream.receipts.size() == (high == 1 ? 1U : 8U),
                    "Operation stream bounds differ");
            if (!initial)
                require(stream == restored.at(account(peer.second)),
                        "Restart changed retained outcomes");
            ++stream_queries;
        } else {
            const bool denied_mismatch = initial ? stage == 2 : stage == 4;
            const bool denied_expired = initial ? stage == 13 : stage == 3;
            std::uint64_t sequence = 1;
            if (initial && stage >= 5 && stage <= 12)
                sequence = stage - 3U;
            if ((initial && stage == 14) || (!initial && !denied_expired))
                sequence = 9;
            auto request = request_for(peer.second, sequence);
            if (denied_mismatch)
                ++request.quantity.currency;
            auto result = service->transfer(*peer.transport, authority, request, service_now);
            if (denied_mismatch || denied_expired) {
                reject(result);
                require(result.error().code == ErrorCode::conflict,
                        "Wrong operation rejection code");
                require(result.error().context.operation == (denied_mismatch
                                                                 ? "storage.transfer.mismatch"
                                                                 : "storage.transfer.expired"),
                        "Wrong operation rejection reason");
                failure = result.error();
                if (denied_mismatch)
                    ++mismatch;
                else
                    ++expired;
            } else {
                const auto value = take(std::move(result));
                require(value.receipt.request == request, "Receipt changed request identity");
                const bool retry = initial ? stage == 1 || stage == 14 : true;
                require(value.replayed == retry, "Retry admission differs");
                if (retry) {
                    const auto& expected =
                        initial
                            ? (stage == 1 ? first_receipts : last_receipts).at(account(peer.second))
                            : restored.at(account(peer.second)).receipts.back();
                    require(value.receipt == expected, "Retry changed retained outcome");
                    ++replayed;
                } else {
                    const auto status = stage == 0 && !peer.second ? TransferStatus::committed
                                                                   : TransferStatus::stale_revision;
                    require(value.receipt.status == status &&
                                value.receipt.source == expected_economy(peer.second) &&
                                value.receipt.target == expected_economy(!peer.second),
                            "Fresh transfer outcome differs");
                    if (status == TransferStatus::committed)
                        ++committed;
                    else
                        ++stale;
                    if (stage == 0)
                        first_receipts.emplace(account(peer.second), value.receipt);
                    if (stage == 12)
                        last_receipts.emplace(account(peer.second), value.receipt);
                }
            }
        }
        auto snapshot = *peer.session;
        snapshot.last_sequence = authority.sequence;
        respond(peer,
                failure ? Result<SessionSnapshot>{*failure} : Result<SessionSnapshot>{snapshot});
    };
    while (clock.now() < 18s) {
        if (peers.size() < 2) {
            auto connection = take(listener->accept(clock.now()));
            if (connection)
                peers.push_back({std::move(*connection)});
        }
        for (auto& peer : peers) {
            if (!peer.transport)
                continue;
            auto polled = peer.transport->poll(clock.now(), 16);
            if (!polled || polled.value().state == TransportState::closed) {
                require(peer.ready && peer.stage == (initial ? 17U : 5U),
                        "Client closed before all stages");
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
                        "Unexpected client certificate");
                peer.second = proven.certificate == fingerprints[1];
                require(std::none_of(peers.begin(), peers.end(),
                                     [&](const Peer& other) {
                                         return &other != &peer && other.ready &&
                                                other.second == peer.second;
                                     }),
                        "Duplicate client identity");
                const auto roster =
                    take(service->roster(*peer.transport, account(peer.second), service_now));
                if (initial) {
                    require(roster.characters.empty() && roster.revision == 1,
                            "Initial roster differs");
                    const auto record = take(service->create_character(
                        *peer.transport, account(peer.second), 1, state(1), service_now));
                    records.emplace(record.account, record);
                    const auto initialized = take(
                        service->initialize_economy(record.account, record.character, {100, 20}));
                    require(initialized.revision == 1 && initialized.state == EconomyState{100, 20},
                            "Initial economy differs");
                } else
                    require(roster.characters.size() == 1 &&
                                roster.characters.front() == records.at(account(peer.second)),
                            "Restart character ownership differs");
                write(discovery(options, peer.second),
                      records.at(account(peer.second)).character.value().to_string() + "\n");
                peer.ready = true;
                ++authenticated;
            }
            if (peer.pending)
                continue;
            auto received = take(peer.transport->receive());
            if (!received)
                continue;
            require(received->kind == MessageKind::session_request,
                    "Unexpected transaction fixture wire kind");
            const auto request = take(decode_session_request(received->payload));
            if (const auto* ping = std::get_if<HeartbeatRequest>(&request)) {
                if (initial && peer.stage == 0)
                    peer.pending = ping->authority;
                else
                    heartbeat(peer, ping->authority);
            } else
                respond(peer, service->apply(*peer.transport, request, service_now));
        }
        if (initial && simultaneous_pending == 0 && peers.size() == 2 && peers[0].pending &&
            peers[1].pending) {
            simultaneous_pending = 2;
            for (const bool second : {false, true}) {
                auto& peer = *std::find_if(peers.begin(), peers.end(), [&](const Peer& value) {
                    return value.second == second;
                });
                heartbeat(peer, *peer.pending);
                peer.pending.reset();
            }
        }
        if (completed == 2)
            break;
        cadence(clock);
    }
    require(completed == 2 && authenticated == 2, "Transaction clients did not complete");
    service.reset();
    store = take(Store::open(database, world()));
    require(take(store->metadata()) == identity, "Store identity changed");
    std::cout << "{\"status\":\"PASS\",\"role\":\"server\",\"scenario\":\"" << scenario
              << "\",\"store_id\":\"" << identity.id.value().to_string() << "\",\"world_id\":\""
              << identity.world.value().to_string()
              << "\",\"schema_version\":" << identity.schema_version
              << ",\"authenticated\":" << authenticated << ",\"accepted\":" << accepted_count
              << ",\"rejected\":" << rejected_count << ",\"committed\":" << committed
              << ",\"stale\":" << stale << ",\"replayed\":" << replayed
              << ",\"mismatch\":" << mismatch << ",\"expired\":" << expired
              << ",\"economy_queries\":" << economy_queries
              << ",\"stream_queries\":" << stream_queries
              << ",\"simultaneous_pending\":" << simultaneous_pending << ",\"records\":[";
    bool comma = false;
    for (const bool second : {false, true}) {
        const auto record =
            take(store->economy(account(second), records.at(account(second)).character));
        require(record == expected_economy(second), "Reopen changed economy");
        if (comma)
            std::cout << ',';
        print_economy(record);
        comma = true;
    }
    std::cout << "],\"streams\":[";
    comma = false;
    for (const bool second : {false, true}) {
        const auto stream = take(store->operation_stream(account(second)));
        require(stream.high_watermark == 9 && stream.receipts.size() == 8,
                "Reopen changed operation retention");
        if (comma)
            std::cout << ',';
        std::cout << "{\"actor\":\"" << stream.actor.value().to_string()
                  << "\",\"high_watermark\":" << stream.high_watermark << ",\"receipts\":[";
        bool separator = false;
        for (const auto& receipt : stream.receipts) {
            if (separator)
                std::cout << ',';
            print_receipt(receipt);
            separator = true;
        }
        std::cout << "]}";
        comma = true;
    }
    std::cout << "],\"initial_receipts\":[";
    comma = false;
    for (const auto& [owner, receipt] : first_receipts) {
        static_cast<void>(owner);
        if (comma)
            std::cout << ',';
        print_receipt(receipt);
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
        throw std::runtime_error{"Unknown transaction fixture role"};
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        std::cout << "{\"status\":\"FAIL\",\"error\":\"storage-transaction-fixture\"}\n";
        return 1;
    }
}
