#include <shark/net/replication.hpp>
#include <shark/time.hpp>

#include <algorithm>
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
#include <vector>

namespace {
using namespace std::chrono_literals;
using namespace shark;
using namespace shark::net;
using Options = std::map<std::string, std::string>;
constexpr auto secret = "SERVER-ONLY-NAME-7c5d8e340aa64b02";

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <typename T> T take(Result<T> result) {
    if (!result)
        throw std::runtime_error("Replication fixture operation failed: " +
                                 result.error().context.operation);
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result)
        throw std::runtime_error("Replication fixture operation failed: " +
                                 result.error().context.operation);
}
std::string file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    require(static_cast<bool>(input), "Cannot read explicit replication fixture credential");
    std::string value{std::istreambuf_iterator<char>{input}, {}};
    require(!value.empty() && value.size() <= 65536, "Invalid fixture credential size");
    return value;
}
unsigned number(const Options& options, const char* key) {
    unsigned value = 0;
    const auto& text = options.at(key);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(),
            "Invalid fixture number");
    return value;
}
void cadence(SteadyMonotonicClock& clock) {
    take(clock.wait_for(1ms));
}
StableId id(unsigned suffix) {
    require(suffix < 100, "Invalid fixture ID suffix");
    return take(StableId::parse(
        "2f939a1f-0747-491b-a038-f7856e3d19" +
        std::string{static_cast<char>('0' + suffix / 10), static_cast<char>('0' + suffix % 10)}));
}
AccountId account(bool second) {
    return AccountId{id(second ? 42 : 41)};
}
CharacterId character(bool second) {
    return CharacterId{id(second ? 52 : 51)};
}
Transform translated(double x) {
    Transform result;
    result.translation.x = x;
    return result;
}
SessionEnvelope envelope(const SessionSnapshot& session, std::uint64_t sequence) {
    return {session.session, session.ownership_epoch, sequence, session.character};
}
void privacy(std::span<const std::byte> bytes) {
    const std::string text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    require(text.find(secret) == std::string::npos, "Private Name leaked into replication payload");
    for (unsigned suffix : {1U, 2U, 3U, 11U, 12U, 13U, 14U, 15U}) {
        const auto value = id(suffix);
        require(std::search(bytes.begin(), bytes.end(), value.bytes().begin(),
                            value.bytes().end()) == bytes.end(),
                "Private source identity leaked into replication payload");
    }
}
std::vector<double> positions(const ReplicaSnapshot& snapshot) {
    std::vector<double> result;
    for (const auto& entity : snapshot.entities) {
        auto expected = ReplicatedPose{};
        expected.coefficients[3] = entity.pose.coefficients[3];
        require(entity.pose == expected,
                "Replica matrix differs from independent expected affine pose");
        result.push_back(entity.pose.coefficients[3]);
    }
    std::sort(result.begin(), result.end());
    return result;
}
void expected_state(const ReplicaSnapshot& snapshot, unsigned stage) {
    const std::vector<std::vector<double>> expected{{10, 20}, {11, 20, 30}, {22, 30}};
    require(stage < expected.size() && snapshot.has_baseline &&
                positions(snapshot) == expected[stage],
            "Client failed independent authoritative-state convergence check");
}

class Client final {
  public:
    Client(const std::filesystem::path& fixtures, std::uint16_t port, bool second,
           SteadyMonotonicClock& clock)
        : clock_(clock) {
        const auto name = second ? "client-b" : "client-a";
        connection = take(TlsTransport::connect_loopback_authenticated(
            port, {file(fixtures / "ca.pem"), "shark.test"},
            {file(fixtures / "session" / (std::string{name} + ".pem")),
             file(fixtures / "session" / (std::string{name} + "-key.pem"))},
            clock.now()));
        while (clock.now() < 12s) {
            if (take(connection->poll(clock.now())).state == TransportState::ready)
                break;
            cadence(clock);
        }
        require(take(connection->stats()).state == TransportState::ready,
                "Replication TLS readiness watchdog");
        session = request(LoginRequest{account(second), character(second)});
        require(session->account == account(second) && session->character == character(second),
                "Replication client logged into another account");
        session = request(SetReadyRequest{envelope(*session, 1), true});
        require(session->ready && session->active && session->last_sequence == 1,
                "Session was not ready");
        cache = take(ReplicaClient::create(*session));
    }
    void send(MessageKind kind, std::vector<std::byte> payload) {
        take(connection->send(kind, payload));
        std::fill(payload.begin(), payload.end(), std::byte{0});
    }
    Frame frame() {
        while (clock_.now() < 12s) {
            const auto polled = take(connection->poll(clock_.now(), 16));
            require(polled.operations <= 16 && polled.state != TransportState::closed,
                    "Replication peer closed or exceeded poll budget");
            auto result = take(connection->receive());
            if (result)
                return std::move(*result);
            cadence(clock_);
        }
        throw std::runtime_error("Replication reply watchdog");
    }
    SessionSnapshot request(const SessionRequest& value) {
        send(MessageKind::session_request, take(encode_session_request(value)));
        auto reply = frame();
        require(reply.kind == MessageKind::session_reply,
                "Unexpected session fixture response kind");
        auto decoded = take(decode_session_reply(reply.payload));
        require(std::holds_alternative<SessionSnapshot>(decoded),
                "Replication fixture session rejected");
        return std::get<SessionSnapshot>(decoded);
    }
    ReplicationUpdate update() {
        auto received = frame();
        require(received.kind == MessageKind::replication_state,
                "Unexpected replication state kind");
        privacy(received.payload);
        payload_bytes += received.payload.size();
        ++received_updates;
        return take(decode_replication_update(received.payload));
    }
    void ack(const ReplicationUpdate& update, bool consume = true,
             std::optional<std::uint64_t> revision = std::nullopt) {
        send(MessageKind::replication_control, take(encode_replication_control(ReplicationAck{
                                                   envelope(*session, sequence), update.stream,
                                                   revision.value_or(update.revision)})));
        if (consume)
            ++sequence;
    }
    void resync(const ReplicationUpdate& update, std::uint64_t known) {
        send(MessageKind::replication_control,
             take(encode_replication_control(
                 ReplicationResync{envelope(*session, sequence), update.stream, known})));
        ++sequence;
    }
    void reject_ack(const ReplicationUpdate& update,
                    std::optional<std::uint64_t> revision = std::nullopt) {
        ack(update, false, revision);
        const auto rejected = frame();
        require(rejected.kind == MessageKind::session_reply &&
                    std::holds_alternative<SessionRejected>(
                        take(decode_session_reply(rejected.payload))),
                "Invalid ACK was not explicitly rejected by the authenticated server");
    }
    void finish() {
        const auto before = take(cache->snapshot());
        const auto ended = request(LogoutRequest{envelope(*session, sequence)});
        require(!ended.active, "Replication logout retained ownership");
        close();
        const auto owned = take(cache->snapshot());
        require(owned.entities == before.entities && owned.revision == before.revision,
                "Replica values did not survive transport destruction");
    }
    void close() {
        if (connection) {
            take(connection->close());
            connection.reset();
        }
    }
    std::unique_ptr<TlsTransport> connection;
    std::optional<SessionSnapshot> session;
    std::unique_ptr<ReplicaClient> cache;
    std::uint64_t sequence{2};
    std::size_t payload_bytes{0};
    unsigned received_updates{0};

  private:
    SteadyMonotonicClock& clock_;
};

int client(const Options& options) {
    const auto port = number(options, "--port");
    require(port > 0 && port <= 65535, "Invalid replication fixture port");
    const std::filesystem::path fixtures{options.at("--fixtures")};
    const auto& scenario = options.at("--scenario");
    const bool second = options.at("--identity") == "b";
    SteadyMonotonicClock clock;
    auto current =
        std::make_unique<Client>(fixtures, static_cast<std::uint16_t>(port), second, clock);
    unsigned stage = 0, applied = 0, duplicates = 0, resyncs = 0, baselines = 0, deltas = 0;
    unsigned reconnects = 0, invalid_controls = 0, acknowledgements = 0;
    std::size_t previous_bytes = 0;
    std::optional<std::uint64_t> original_revision;
    std::optional<ReplicaId> continuing_id, destroyed_id;
    while (clock.now() < 12s) {
        auto update = current->update();
        if (scenario == "reconnect" && !second && reconnects == 0 && stage == 1) {
            require(!update.baseline, "Reconnect fixture did not receive an outstanding delta");
            auto replacement =
                std::make_unique<Client>(fixtures, static_cast<std::uint16_t>(port), false, clock);
            require(replacement->session->session != current->session->session &&
                        replacement->session->ownership_epoch > current->session->ownership_epoch,
                    "Reconnect reused session authority");
            require(!replacement->cache->apply(update),
                    "Fresh session cache accepted stale prior-session update");
            current->reject_ack(update);
            ++invalid_controls;
            previous_bytes += current->payload_bytes;
            current->close();
            current = std::move(replacement);
            continuing_id.reset();
            destroyed_id.reset();
            ++reconnects;
            continue;
        }
        const auto before = take(current->cache->snapshot());
        const auto outcome = take(current->cache->apply(update));
        if (outcome == ReplicaApplyResult::resync_required) {
            require(scenario == "missing-baseline" && !second && stage == 1 && !update.baseline,
                    "Unexpected missing replication baseline");
            const auto after = take(current->cache->snapshot());
            require(after.entities == before.entities && after.revision == before.revision &&
                        after.has_baseline == before.has_baseline,
                    "Failed delta partially changed client cache");
            current->resync(update, before.revision);
            ++resyncs;
            continue;
        }
        if (outcome == ReplicaApplyResult::duplicate) {
            require(scenario == "duplicate-state", "Unexpected repeated state payload");
            const auto after = take(current->cache->snapshot());
            require(after.entities == before.entities && after.revision == before.revision,
                    "Duplicate replication payload mutated client state");
            ++duplicates;
            if (stage == 3)
                break;
            continue;
        }
        const auto snapshot = take(current->cache->snapshot());
        expected_state(snapshot, stage);
        if (stage == 0)
            original_revision = update.revision;
        for (const auto& entity : snapshot.entities) {
            const auto x = entity.pose.coefficients[3];
            if (x == 20 || x == 22) {
                if (continuing_id)
                    require(entity.id == *continuing_id,
                            "Surviving entity changed replica identity");
                continuing_id = entity.id;
            }
            if (x == 10 || x == 11)
                destroyed_id = entity.id;
            if (x == 30 && destroyed_id)
                require(entity.id != *destroyed_id, "Spawn reused another live replica ID");
            if (stage == 2 && destroyed_id)
                require(entity.id != *destroyed_id, "Despawn left an old replica entity");
        }
        ++applied;
        update.baseline ? ++baselines : ++deltas;
        if (scenario == "forged-state") {
            const auto unchanged = snapshot.entities;
            current->send(MessageKind::replication_control,
                          take(encode_replication_update(update)));
            bool rejected = false;
            while (clock.now() < 12s) {
                auto polled = current->connection->poll(clock.now());
                if (!polled) {
                    require(polled.error().context.operation == "net.tls.read" ||
                                polled.error().context.operation == "net.tls.write",
                            "Forged state failed by deadline instead of peer rejection");
                    rejected = true;
                    break;
                }
                require(polled.value().state != TransportState::closed,
                        "Unexpected clean forged-state EOF");
                cadence(clock);
            }
            require(rejected && take(current->cache->snapshot()).entities == unchanged,
                    "Forged-state rejection watchdog or cache mutation");
            current->close();
            std::cout << "{\"status\":\"PASS\",\"role\":\"client\",\"forgery_rejected\":true,"
                         "\"privacy_checked\":true,\"owned_results\":true,\"applied\":1}\n";
            return 0;
        }
        if (scenario == "ack-rejection" && !second && stage == 0) {
            current->reject_ack(update, update.revision + 100);
            ++invalid_controls;
        }
        if (scenario == "ack-rejection" && !second && stage == 2) {
            current->reject_ack(update, original_revision);
            ++invalid_controls;
        }
        current->ack(update);
        ++acknowledgements;
        if (scenario == "ack-rejection" && !second && stage == 0)
            current->ack(update);
        if (scenario == "missing-baseline" && !second && stage == 0) {
            take(current->cache->discard_baseline());
            const auto discarded = take(current->cache->snapshot());
            require(!discarded.has_baseline && discarded.revision == snapshot.revision &&
                        discarded.stream == snapshot.stream,
                    "Cache loss reset replication fences");
        }
        ++stage;
        if (stage == 3 && scenario != "duplicate-state")
            break;
    }
    require(stage == 3 && applied == 3, "Replication stage watchdog");
    const auto final = take(current->cache->snapshot());
    expected_state(final, 2);
    const auto bytes = previous_bytes + current->payload_bytes;
    current->finish();
    std::cout << "{\"status\":\"PASS\",\"role\":\"client\",\"privacy_checked\":true,"
                 "\"owned_results\":true,\"applied\":"
              << applied << ",\"duplicates\":" << duplicates << ",\"resyncs\":" << resyncs
              << ",\"reconnects\":" << reconnects << ",\"baselines\":" << baselines
              << ",\"deltas\":" << deltas << ",\"invalid_controls\":" << invalid_controls
              << ",\"acknowledgements\":" << acknowledgements
              << ",\"payload_bytes_received\":" << bytes << ",\"final_revision\":" << final.revision
              << ",\"final_positions\":[22,30],\"final_entities\":2}\n";
    return 0;
}

int server(const Options& options) {
    const std::filesystem::path fixtures{options.at("--fixtures")};
    const auto& scenario = options.at("--scenario");
    const unsigned client_count = scenario == "forged-state" ? 1U : 2U;
    const unsigned connection_count = client_count + (scenario == "reconnect" ? 1U : 0U);
    auto sessions = take(SessionAuthority::create());
    for (bool second : {false, true}) {
        take(sessions->provision_account(account(second)));
        take(sessions->assign_character(account(second), character(second)));
        take(sessions->enroll_certificate(
            account(second),
            take(certificate_fingerprint(
                file(fixtures / "session" / (second ? "client-b.pem" : "client-a.pem"))))));
    }
    auto publisher = take(ReplicationServer::create());
    auto scene = take(Scene::create(
        RegionSceneScope{WorldId{id(1)}, {RegionId{id(2)}, RegionInstanceId{id(3)}, 1}}));
    const auto parent =
        take(scene->create_entity({WorldEntityId{id(11)}, {}, Name{secret}, translated(100)}));
    const auto first = take(
        scene->create_entity({WorldEntityId{id(12)}, {}, Name{secret}, translated(-90), parent}));
    const auto second = take(
        scene->create_entity({WorldEntityId{id(13)}, {}, Name{secret}, translated(-80), parent}));
    take(scene->create_entity({WorldEntityId{id(14)}, {}, Name{secret}, translated(987654)}));
    std::vector<EntityHandle> published{first, second};
    const auto initial_revision = take(scene->stats()).revision;
    auto listener = take(TlsListener::listen_loopback_authenticated(
        0, {file(fixtures / "server.pem"), file(fixtures / "server-key.pem")},
        file(fixtures / "ca.pem")));
    {
        std::ofstream ready{options.at("--ready")};
        ready << "{\"port\":" << take(listener->port()) << "}\n";
        ready.close();
        require(static_cast<bool>(ready), "Cannot publish replication readiness");
    }
    struct Peer {
        std::unique_ptr<TlsTransport> transport;
        std::optional<SessionSnapshot> session;
        std::optional<ReplicationUpdate> sent;
        unsigned sent_stage{0};
        bool suppressed{false};
    };
    std::vector<Peer> peers;
    std::set<CertificateFingerprint> credentials;
    std::array<bool, 2> acknowledged{};
    unsigned accepted = 0, completed = 0, stage = 0;
    unsigned controls = 0, control_rejections = 0, duplicate_controls = 0, resyncs = 0,
             codec_rejections = 0;
    unsigned logical_updates = 0, sent_updates = 0, spawns = 0, mutations = 0, despawns = 0;
    std::uint64_t socket_sent = 0, socket_received = 0;
    std::size_t payload_bytes = 0;
    auto retire = [&](Peer& peer) {
        const auto metrics = take(peer.transport->stats());
        socket_sent += metrics.socket_bytes_sent;
        socket_received += metrics.socket_bytes_received;
        if (peer.session) {
            take(publisher->forget(peer.session->session));
            take(sessions->revoke_session(peer.session->session));
        }
        peer.transport.reset();
        ++completed;
    };
    SteadyMonotonicClock clock;
    while (clock.now() < 14s) {
        const auto now = clock.now();
        if (accepted < connection_count) {
            auto next = take(listener->accept(now));
            if (next) {
                peers.push_back({std::move(*next), {}, {}, 0, false});
                ++accepted;
            }
        }
        for (auto& peer : peers) {
            if (!peer.transport)
                continue;
            auto polled = peer.transport->poll(now, 16);
            if (!polled || polled.value().state == TransportState::closed) {
                require(peer.session.has_value(),
                        "Replication peer failed before session authentication");
                retire(peer);
                continue;
            }
            require(polled.value().operations <= 16, "Replication server exceeded polling budget");
            if (polled.value().state != TransportState::ready)
                continue;
            auto frame = take(peer.transport->receive());
            if (frame) {
                if (frame->kind == MessageKind::session_request) {
                    const auto request = take(decode_session_request(frame->payload));
                    auto result = sessions->apply(*peer.transport, request, now);
                    require(static_cast<bool>(result),
                            "Fixture session request unexpectedly rejected");
                    peer.session = result.value();
                    if (std::holds_alternative<LoginRequest>(request)) {
                        credentials.insert(take(peer.transport->authenticated_peer()).certificate);
                        for (auto& previous : peers)
                            if (&previous != &peer && previous.session &&
                                previous.session->account == peer.session->account)
                                previous.suppressed = true;
                    }
                    take(peer.transport->send(
                        MessageKind::session_reply,
                        take(encode_session_reply(public_session_reply(result)))));
                } else {
                    require(frame->kind == MessageKind::replication_control,
                            "Client supplied server-only replication kind");
                    auto control = decode_replication_control(frame->payload);
                    if (!control) {
                        require(scenario == "forged-state",
                                "Unexpected malformed replication control");
                        ++codec_rejections;
                        require(take(scene->stats()).revision == initial_revision,
                                "Forged state mutated authoritative Scene");
                        take(peer.transport->close());
                        retire(peer);
                        continue;
                    }
                    ++controls;
                    auto result =
                        publisher->control(*sessions, *peer.transport, control.value(), now);
                    if (!result) {
                        ++control_rejections;
                        // Fixture coordination only: observe rejection before reusing the
                        // exact session sequence or closing a fenced connection.
                        take(peer.transport->send(MessageKind::session_reply,
                                                  take(encode_session_reply(SessionRejected{}))));
                    } else if (result.value() == ReplicationControlResult::duplicate) {
                        ++duplicate_controls;
                    } else if (result.value() == ReplicationControlResult::resync_requested) {
                        ++resyncs;
                        peer.sent.reset();
                    } else {
                        require(peer.session && peer.sent && peer.sent_stage == stage,
                                "ACK advanced another scene stage");
                        acknowledged[peer.session->account == account(true) ? 1U : 0U] = true;
                    }
                }
            }
            if (peer.session && peer.session->ready && peer.session->active && !peer.suppressed) {
                auto prepared =
                    take(publisher->prepare(*scene, published, *sessions, *peer.transport, now));
                if (prepared && (!peer.sent || *prepared != *peer.sent)) {
                    privacy(take(encode_replication_update(*prepared)));
                    const auto bytes = take(encode_replication_update(*prepared));
                    ++logical_updates;
                    for (const auto& change : prepared->changes) {
                        if (change.kind == ReplicationChange::spawn)
                            ++spawns;
                        else if (change.kind == ReplicationChange::update)
                            ++mutations;
                        else
                            ++despawns;
                    }
                    const unsigned copies = scenario == "duplicate-state" ? 2U : 1U;
                    for (unsigned copy = 0; copy < copies; ++copy) {
                        take(peer.transport->send(MessageKind::replication_state, bytes));
                        ++sent_updates;
                        payload_bytes += bytes.size();
                    }
                    peer.sent = std::move(*prepared);
                    peer.sent_stage = stage;
                }
            }
        }
        if (scenario != "forged-state" && stage < 2 && acknowledged[0] && acknowledged[1]) {
            acknowledged = {};
            if (stage == 0) {
                take(scene->set_transform(first, translated(-89)));
                published.push_back(take(scene->create_entity(
                    {WorldEntityId{id(15)}, {}, Name{secret}, translated(30)})));
                // A private-only change is deliberately concurrent with public changes.
                take(scene->set_name(second, Name{std::string{secret} + "-changed"}));
            } else {
                take(scene->destroy_entity(first));
                published.erase(published.begin());
                take(scene->set_transform(second, translated(-78)));
            }
            ++stage;
        }
        if (accepted == connection_count && completed == connection_count) {
            require(scenario == "forged-state" ||
                        (stage == 2 && acknowledged[0] && acknowledged[1]),
                    "Peers ended before both acknowledged final authoritative state");
            require(take(publisher->stats()).streams == 0 &&
                        take(sessions->stats()).live_sessions == 0,
                    "Replication fixture leaked streams or authority");
            std::cout << "{\"status\":\"PASS\",\"role\":\"server\",\"accepted_connections\":"
                      << accepted << ",\"distinct_credentials\":" << credentials.size()
                      << ",\"scene_stage\":" << stage
                      << ",\"initial_scene_revision\":" << initial_revision
                      << ",\"final_scene_revision\":" << take(scene->stats()).revision
                      << ",\"controls\":" << controls
                      << ",\"control_rejections\":" << control_rejections
                      << ",\"duplicate_controls\":" << duplicate_controls
                      << ",\"resyncs\":" << resyncs << ",\"codec_rejections\":" << codec_rejections
                      << ",\"logical_updates\":" << logical_updates
                      << ",\"sent_updates\":" << sent_updates << ",\"spawns\":" << spawns
                      << ",\"updates\":" << mutations << ",\"despawns\":" << despawns
                      << ",\"payload_bytes_sent\":" << payload_bytes
                      << ",\"socket_bytes_sent\":" << socket_sent
                      << ",\"socket_bytes_received\":" << socket_received
                      << ",\"privacy_checked\":true,\"live_streams\":0,\"live_sessions\":0}\n";
            return 0;
        }
        cadence(clock);
    }
    throw std::runtime_error("Replication server stage watchdog");
}
} // namespace

int main(int argc, char** argv) {
    try {
        Options options;
        for (int index = 1; index < argc; index += 2)
            require(index + 1 < argc && options.emplace(argv[index], argv[index + 1]).second,
                    "Invalid replication fixture options");
        if (options.at("--mode") == "client")
            return client(options);
        if (options.at("--mode") == "server")
            return server(options);
        throw std::runtime_error("Unknown replication fixture mode");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::cout << "{\"status\":\"FAIL\",\"error\":\"replication-fixture\"}\n";
        return 1;
    }
}
