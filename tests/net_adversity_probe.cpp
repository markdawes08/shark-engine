#include <shark/net/fault_link.hpp>
#include <shark/net/interest.hpp>
#include <shark/time.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
using namespace shark;
using namespace shark::net;
using namespace std::chrono_literals;
using Options = std::map<std::string, std::string>;
constexpr std::size_t queue_limit = 16384;
constexpr std::size_t client_storage_limit = 32768;
constexpr std::size_t interest_storage_limit = 262144;
constexpr auto canary = "PRIVATE-ADVERSITY-CANARY-491f";

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <typename T> T take(Result<T> result) {
    if (!result)
        throw std::runtime_error("Adversity fixture: " + result.error().context.operation);
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result)
        throw std::runtime_error("Adversity fixture: " + result.error().context.operation);
}
std::string file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    require(static_cast<bool>(input), "Cannot read public credential fixture");
    std::string result{std::istreambuf_iterator<char>{input}, {}};
    require(!result.empty() && result.size() <= 65536, "Invalid credential fixture size");
    return result;
}
void mark(const std::filesystem::path& path) {
    std::ofstream output{path};
    output << "ready\n";
    output.close();
    require(static_cast<bool>(output), "Cannot write fixture barrier");
}
void finish_monitor(const Options& options) {
    std::filesystem::path marker{options.at("--pid-file")};
    marker += ".finish";
    mark(marker);
    std::filesystem::path acknowledged{options.at("--pid-file")};
    acknowledged += ".finished";
    SteadyMonotonicClock clock;
    while (!std::filesystem::exists(acknowledged) && clock.now() < 2s)
        take(clock.wait_for(1ms));
    require(std::filesystem::exists(acknowledged), "Final process resource sample missing");
}
unsigned number(const Options& options, const char* name) {
    unsigned result = 0;
    const auto& text = options.at(name);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(),
            "Invalid fixture number");
    return result;
}
StableId id(unsigned value) {
    std::string text = "4bd2395b-9372-4bb8-9f46-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t n = 0; n < 8; ++n) {
        text[35 - n] = digits[value & 15U];
        value >>= 4U;
    }
    return take(StableId::parse(text));
}
AccountId account(bool second) {
    return AccountId{id(second ? 102 : 101)};
}
CharacterId character(bool second) {
    return CharacterId{id(second ? 202 : 201)};
}
SessionEnvelope envelope(const SessionSnapshot& session, std::uint64_t sequence) {
    return {session.session, session.ownership_epoch, sequence, session.character};
}
bool shaped(const std::string& scenario) {
    return scenario == "rtt100-loss1" || scenario == "rtt200-loss5";
}
unsigned final_tick(const std::string& scenario) {
    return shaped(scenario) ? 320U : 120U;
}
TransportLimits transport_limits() {
    TransportLimits result;
    result.queue_frames = 4;
    result.queue_bytes = queue_limit;
    result.idle_timeout = 30s;
    return result;
}
void cadence(SteadyMonotonicClock& clock) {
    take(clock.wait_for(1ms));
}
Transform pose(unsigned ordinal, unsigned tick) {
    Transform result;
    result.translation = {static_cast<double>((ordinal / 33) * 1000 + ordinal % 33),
                          static_cast<double>(tick) / 1000, static_cast<double>(ordinal) / 1024};
    return result;
}
void privacy(std::span<const std::byte> bytes) {
    const std::string text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    require(text.find(canary) == std::string::npos, "Private Name disclosed");
    for (unsigned n = 0; n < 66; ++n) {
        const auto value = id(1000 + n);
        require(std::search(bytes.begin(), bytes.end(), value.bytes().begin(),
                            value.bytes().end()) == bytes.end(),
                "Durable source identity disclosed");
    }
}
unsigned verify(const ReplicaSnapshot& state, bool second, unsigned maximum) {
    require(state.has_baseline && state.entities.size() == 32, "Missing complete visible set");
    std::set<unsigned> observed;
    std::optional<unsigned> tick;
    for (const auto& entity : state.entities) {
        require(entity.pose.coefficients[11] >= 0 && entity.pose.coefficients[11] < 66.0 / 1024 &&
                    entity.pose.coefficients[7] >= 0 &&
                    entity.pose.coefficients[7] <= static_cast<double>(maximum) / 1000,
                "Replica position outside bounded fixture coordinates");
        const auto ordinal =
            static_cast<unsigned>(std::llround(entity.pose.coefficients[11] * 1024));
        const auto current =
            static_cast<unsigned>(std::llround(entity.pose.coefficients[7] * 1000));
        require(ordinal < 66 && ordinal / 33 == static_cast<unsigned>(second) && ordinal % 33 < 32,
                "Hidden or foreign actor disclosed");
        require(current <= maximum && (!tick || *tick == current), "Mixed source ticks");
        tick = current;
        ReplicatedPose expected;
        expected.coefficients[3] = static_cast<double>((ordinal / 33) * 1000 + ordinal % 33);
        expected.coefficients[7] = static_cast<double>(current) / 1000;
        expected.coefficients[11] = static_cast<double>(ordinal) / 1024;
        require(entity.pose == expected && observed.insert(ordinal % 33).second,
                "Replica differs from independent pose schedule");
    }
    return *tick;
}
struct QueuePeak {
    std::size_t bytes{0}, frames{0};
    void record(const TransportStats& stats) {
        bytes = std::max({bytes, stats.send_queue_bytes, stats.receive_queue_bytes});
        frames = std::max({frames, stats.send_queue_frames, stats.receive_queue_frames});
        require(bytes <= queue_limit && frames <= 4 && stats.operations <= 16,
                "Transport exceeded bounded queue or poll work");
    }
};

class Connection final {
  public:
    Connection(const std::filesystem::path& fixtures, std::uint16_t port, bool second,
               SteadyMonotonicClock& clock)
        : clock_(clock) {
        const std::string leaf = second ? "client-b" : "client-a";
        transport = take(TlsTransport::connect_loopback_authenticated(
            port, {file(fixtures / "ca.pem"), "shark.test"},
            {file(fixtures / "session" / (leaf + ".pem")),
             file(fixtures / "session" / (leaf + "-key.pem"))},
            clock.now(), transport_limits()));
        while (clock.now() < 55s) {
            if (take(transport->poll(clock.now())).state == TransportState::ready)
                break;
            cadence(clock);
        }
        require(take(transport->stats()).state == TransportState::ready, "TLS readiness watchdog");
        session = request(LoginRequest{account(second), character(second)});
        session = request(SetReadyRequest{envelope(*session, 1), true});
        cache = take(ReplicaClient::create(*session));
    }
    SessionSnapshot request(const SessionRequest& request) {
        take(transport->send(MessageKind::session_request, take(encode_session_request(request))));
        while (clock_.now() < 55s) {
            take(transport->poll(clock_.now()));
            if (auto frame = take(transport->receive())) {
                require(frame->kind == MessageKind::session_reply, "Unexpected login reply kind");
                auto reply = take(decode_session_reply(frame->payload));
                require(std::holds_alternative<SessionSnapshot>(reply), "Login rejected");
                return std::get<SessionSnapshot>(reply);
            }
            cadence(clock_);
        }
        throw std::runtime_error("Login watchdog");
    }
    std::unique_ptr<TlsTransport> transport;
    std::optional<SessionSnapshot> session;
    std::unique_ptr<ReplicaClient> cache;
    std::uint64_t sequence{2};

  private:
    SteadyMonotonicClock& clock_;
};

enum class PendingKind { ack, resync, hostile, stale, heartbeat, logout };
struct PendingControl {
    PendingKind kind;
    std::uint64_t revision;
    MonotonicTime sent;
};

int client(const Options& options) {
    const auto port = number(options, "--port");
    require(port > 0 && port <= 65535, "Invalid client port");
    const std::filesystem::path fixtures{options.at("--fixtures")},
        control{options.at("--control")};
    const auto& scenario = options.at("--scenario");
    const bool second = options.at("--identity") == "b";
    const bool faults = shaped(scenario) && !second;
    const bool partition = scenario == "partition-reconnect" && !second;
    const bool slow = scenario == "slow-reader" && !second;
    const bool hostile = scenario == "hostile" && !second;
    const unsigned target_tick = final_tick(scenario);
    SteadyMonotonicClock clock;
    auto connection =
        std::make_unique<Connection>(fixtures, static_cast<std::uint16_t>(port), second, clock);
    FaultLinkConfig config;
    config.seed = scenario == "rtt100-loss1" ? 1U : 1337U;
    config.maximum_packet_bytes = 4096;
    config.maximum_pending_packets = 16;
    config.maximum_pending_bytes = 65536;
    config.maximum_drain_count = 8;
    config.jitter = 30ms;
    config.reorder_delay = 1500ms;
    config.loss_per_10000 = scenario == "rtt100-loss1" ? 100U : 500U;
    config.duplication_per_10000 = 5000;
    config.reorder_per_10000 = 3000;
    auto link = take(FaultLink::create(config));
    auto replay = take(FaultLink::create(config));
    QueuePeak queue;
    std::optional<PendingControl> pending;
    std::optional<ReplicationUpdate> needed_ack;
    std::optional<ReplicationUpdate> saved_old;
    std::optional<ReplicationUpdate> disconnected_update;
    std::optional<SessionSnapshot> saved_session;
    bool need_resync = false, primed = false, reconnect_done = false, paused = false;
    bool stale_sent = false, final_ack = false, logout_done = false;
    unsigned latest_tick = 0, applied = 0, duplicates = 0, stale_rejections = 0;
    unsigned resyncs = 0, reconnects = 0, hostile_step = 0, expected_rejections = 0;
    std::size_t client_storage = 0, history_records = 0;
    std::uint64_t submitted = 0, last_tag = 0, inversions = 0;
    MonotonicTime last_progress = clock.now(), pause_until{0}, next_hostile_at{0};
    std::vector<double> ack_latencies;
    ack_latencies.reserve(256);
    auto send_control = [&](const ReplicationControl& value, PendingKind kind,
                            std::uint64_t revision) {
        require(!pending, "Control overlap could consume ambiguous shared sequence");
        take(connection->transport->send(MessageKind::replication_control,
                                         take(encode_replication_control(value))));
        pending = PendingControl{kind, revision, clock.now()};
    };
    auto apply = [&](std::span<const std::byte> bytes) {
        privacy(bytes);
        auto update = take(decode_replication_update(bytes));
        const auto before = take(connection->cache->snapshot());
        auto result = connection->cache->apply(update);
        if (!result) {
            require(update.revision < before.revision &&
                        take(connection->cache->snapshot()).entities == before.entities,
                    "Unexpected semantic state rejection or partial mutation");
            ++stale_rejections;
            return;
        }
        if (result.value() == ReplicaApplyResult::duplicate) {
            ++duplicates;
            return;
        }
        if (result.value() == ReplicaApplyResult::resync_required) {
            require(take(connection->cache->snapshot()).entities == before.entities,
                    "Missing base partially changed client cache");
            need_resync = true;
            return;
        }
        latest_tick = verify(take(connection->cache->snapshot()), second, target_tick);
        ++applied;
        last_progress = clock.now();
        needed_ack = update;
        saved_old = update;
        const auto stats = take(connection->cache->stats());
        require(stats.entities <= 32 && stats.known_live_ids <= 32 &&
                    stats.latest_change_records <= 64,
                "Client history exceeded count bounds");
        client_storage =
            std::max(client_storage, stats.entity_capacity_bytes + stats.known_live_capacity_bytes +
                                         stats.latest_change_capacity_bytes);
        history_records = std::max(history_records, stats.latest_change_records);
        require(client_storage <= client_storage_limit,
                "Client owned history storage exceeded cap");
    };
    while (clock.now() < 55s) {
        const auto now = clock.now();
        if (slow && paused && !reconnect_done && now < pause_until) {
            cadence(clock);
            continue;
        }
        if (!second && !reconnect_done &&
            ((partition && std::filesystem::exists(control / "partition-dropped")) ||
             (slow && paused && std::filesystem::exists(control / "slow-expired")))) {
            saved_session = connection->session;
            disconnected_update = saved_old;
            take(connection->transport->close());
            connection = std::make_unique<Connection>(fixtures, static_cast<std::uint16_t>(port),
                                                      false, clock);
            require(connection->session->session != saved_session->session &&
                        connection->session->ownership_epoch > saved_session->ownership_epoch,
                    "Reconnect revived previous session authority");
            require(disconnected_update && !connection->cache->apply(*disconnected_update) &&
                        take(connection->cache->snapshot()).entities.empty(),
                    "Fresh authenticated cache accepted previous session state");
            pending.reset();
            needed_ack.reset();
            need_resync = false;
            reconnect_done = true;
            ++reconnects;
            last_progress = clock.now();
        }
        auto polled = connection->transport->poll(clock.now(), 16);
        if (!polled || polled.value().state == TransportState::closed) {
            require(!reconnect_done && (partition || slow), "Unexpected client transport failure");
            cadence(clock);
            continue;
        }
        queue.record(polled.value());
        if (partition && !reconnect_done && pending &&
            std::filesystem::exists(control / "partition-active"))
            mark(control / "uncertain-control-sent");
        for (unsigned drain = 0; drain < 4; ++drain) {
            auto frame = take(connection->transport->receive());
            if (!frame)
                break;
            if (frame->kind == MessageKind::replication_state) {
                privacy(frame->payload);
                if (faults && primed && !std::filesystem::exists(control / "faults-ended")) {
                    const auto submitted_at = clock.now();
                    const auto report = take(link->send(submitted_at, frame->payload, ++submitted));
                    const auto repeated =
                        take(replay->send(submitted_at, frame->payload, submitted));
                    require(report.outcome == repeated.outcome &&
                                report.queued_copies == repeated.queued_copies &&
                                report.scheduled_at == repeated.scheduled_at,
                            "Semantic seeded admission replay diverged");
                    require(report.outcome != FaultSendOutcome::backpressure,
                            "Semantic fault queue unexpectedly saturated");
                } else {
                    apply(frame->payload);
                }
            } else {
                require(frame->kind == MessageKind::session_reply && pending.has_value(),
                        "Unsolicited control reply");
                auto reply = take(decode_session_reply(frame->payload));
                const auto previous = *pending;
                pending.reset();
                if (std::holds_alternative<SessionRejected>(reply)) {
                    if (previous.kind == PendingKind::hostile ||
                        previous.kind == PendingKind::stale) {
                        ++expected_rejections;
                        if (previous.kind == PendingKind::hostile) {
                            ++hostile_step;
                            if (hostile_step == 3)
                                mark(control / "hostile-ended");
                        }
                    } else {
                        require(previous.kind == PendingKind::ack, "Unexpected rejected control");
                        need_resync = true;
                    }
                } else {
                    require(previous.kind != PendingKind::hostile &&
                                previous.kind != PendingKind::stale,
                            "Forged or replayed authority accepted");
                    const auto accepted = std::get<SessionSnapshot>(reply);
                    require(accepted.session == connection->session->session &&
                                accepted.last_sequence == connection->sequence,
                            "Control reply advanced an unexpected session sequence");
                    ++connection->sequence;
                    if (previous.kind == PendingKind::ack) {
                        require(ack_latencies.size() < 256, "ACK sample bound exceeded");
                        ack_latencies.push_back(
                            std::chrono::duration<double, std::milli>{clock.now() - previous.sent}
                                .count());
                        if (latest_tick == target_tick && saved_old &&
                            previous.revision == saved_old->revision)
                            final_ack = true;
                        if (!primed) {
                            primed = true;
                            mark(control / (second ? "b-primed" : "a-primed"));
                        }
                    }
                    if (previous.kind == PendingKind::logout) {
                        require(!accepted.active, "Logout retained authority");
                        logout_done = true;
                    }
                }
            }
        }
        const auto drained_at = clock.now();
        auto delivered_batch = take(link->drain(drained_at, 8));
        require(delivered_batch == take(replay->drain(drained_at, 8)) &&
                    take(link->stats()) == take(replay->stats()),
                "Semantic seeded delivery replay diverged");
        for (auto& delivered : delivered_batch) {
            if (delivered.message_tag < last_tag)
                ++inversions;
            last_tag = delivered.message_tag;
            apply(delivered.payload);
        }
        // The newly received update is still unacknowledged at this point.
        // Pausing only at loop entry would miss it: ordinary admission below
        // consumes needed_ack in the same iteration that receives the update.
        if (slow && !paused && primed && latest_tick > 0 && needed_ack && !pending) {
            paused = true;
            pause_until = clock.now() + 700ms;
            mark(control / "a-slow-start");
            continue;
        }
        if (logout_done) {
            take(connection->transport->close());
            connection->transport.reset();
            connection->cache.reset();
            break;
        }
        if (!pending && reconnect_done && !stale_sent) {
            require(saved_session && disconnected_update, "Missing stale authority fixture");
            send_control(ReplicationAck{envelope(*saved_session, 2), disconnected_update->stream,
                                        disconnected_update->revision},
                         PendingKind::stale, disconnected_update->revision);
            stale_sent = true;
        } else if (!pending && hostile && primed && latest_tick >= 20 && hostile_step < 3 &&
                   clock.now() >= next_hostile_at) {
            require(saved_old.has_value(), "Hostile baseline not primed");
            next_hostile_at = clock.now() + 200ms;
            if (hostile_step == 0)
                mark(control / "hostile-start");
            if (hostile_step == 0) {
                const std::array malformed{std::byte{0}, std::byte{1}};
                take(connection->transport->send(MessageKind::replication_control, malformed));
                pending = PendingControl{PendingKind::hostile, 0, clock.now()};
            } else {
                auto forged =
                    envelope(*connection->session, hostile_step == 1 ? connection->sequence : 2);
                if (hostile_step == 1)
                    forged.character = character(true);
                send_control(ReplicationAck{forged, saved_old->stream, saved_old->revision},
                             PendingKind::hostile, saved_old->revision);
            }
        } else if (!pending && partition && !reconnect_done &&
                   std::filesystem::exists(control / "partition-active")) {
            take(connection->transport->send(MessageKind::session_request,
                                             take(encode_session_request(HeartbeatRequest{envelope(
                                                 *connection->session, connection->sequence)}))));
            pending = PendingControl{PendingKind::heartbeat, 0, clock.now()};
            mark(control / "uncertain-control-sent");
        } else if (!pending && needed_ack) {
            send_control(ReplicationAck{envelope(*connection->session, connection->sequence),
                                        needed_ack->stream, needed_ack->revision},
                         PendingKind::ack, needed_ack->revision);
            needed_ack.reset();
        } else if (!pending && final_ack && take(link->stats()).pending_packets == 0) {
            take(connection->transport->send(MessageKind::session_request,
                                             take(encode_session_request(LogoutRequest{envelope(
                                                 *connection->session, connection->sequence)}))));
            pending = PendingControl{PendingKind::logout, 0, clock.now()};
        } else if (!pending && primed && !final_ack &&
                   (need_resync || (clock.now() - last_progress >= 700ms &&
                                    std::filesystem::exists(control / "source-start")))) {
            const auto state = take(connection->cache->snapshot());
            require(state.stream.has_value(), "Resync lacks prior authenticated stream");
            send_control(ReplicationResync{envelope(*connection->session, connection->sequence),
                                           *state.stream, state.revision},
                         PendingKind::resync, state.revision);
            need_resync = false;
            last_progress = clock.now();
            ++resyncs;
        }
        queue.record(take(connection->transport->stats()));
        cadence(clock);
    }
    const auto fault = take(link->stats());
    require(logout_done && latest_tick == target_tick && applied >= 3,
            "Client convergence watchdog");
    require(!second || applied >= (shaped(scenario) ? 80U : 30U),
            "Healthy recipient did not receive useful active updates");
    require(scenario != "baseline" || applied >= 30,
            "Baseline recipient did not receive useful active updates");
    require(fault.pending_packets == 0 && fault.pending_bytes == 0,
            "Semantic queue retained history");
    require(!faults || (fault.loss_drops > 0 && fault.duplicated_messages > 0 &&
                        fault.reordered_messages > 0 && inversions > 0 && resyncs > 0),
            "Fault profile did not exercise actual loss duplication reorder and recovery");
    if ((slow || partition) && (reconnects != 1 || expected_rejections != 1)) {
        std::cerr << "Reconnect observations: reconnects=" << reconnects
                  << " expected_rejections=" << expected_rejections << " paused=" << paused << '\n';
        throw std::runtime_error("Reconnect did not fence stale controls");
    }
    require(!hostile || expected_rejections == 3, "Hostile matrix incomplete");
    link.reset();
    replay.reset();
    connection.reset();
    needed_ack.reset();
    saved_old.reset();
    disconnected_update.reset();
    finish_monitor(options);
    std::cout << "{\"status\":\"PASS\",\"role\":\"client\",\"scenario\":\"" << scenario
              << "\",\"identity\":\"" << (second ? "b" : "a")
              << "\",\"source_tick\":" << latest_tick << ",\"applied\":" << applied
              << ",\"duplicates\":" << duplicates << ",\"stale_rejections\":" << stale_rejections
              << ",\"resyncs\":" << resyncs << ",\"reconnects\":" << reconnects
              << ",\"expected_rejections\":" << expected_rejections
              << ",\"privacy_checked\":true,\"final_entities\":32,\"max_queue_bytes\":"
              << queue.bytes << ",\"max_queue_frames\":" << queue.frames
              << ",\"max_client_storage_bytes\":" << client_storage
              << ",\"client_storage_limit_bytes\":" << client_storage_limit
              << ",\"max_client_history_records\":" << history_records
              << ",\"queued_fault_packets_peak\":" << fault.peak_packets
              << ",\"queued_fault_bytes_peak\":" << fault.peak_bytes
              << ",\"loss_drops\":" << fault.loss_drops
              << ",\"duplicate_messages\":" << fault.duplicated_messages
              << ",\"reordered_messages\":" << fault.reordered_messages
              << ",\"delivered_reorder_inversions\":" << inversions
              << ",\"delivered_copies\":" << fault.delivered_copies
              << ",\"semantic_submissions\":" << submitted
              << ",\"semantic_faults_enabled\":" << (faults ? "true" : "false")
              << ",\"final_cleanup\":true,\"ack_latency_ms\":[";
    for (std::size_t n = 0; n < ack_latencies.size(); ++n) {
        if (n != 0)
            std::cout << ',';
        std::cout << ack_latencies[n];
    }
    std::cout << "],\"semantic_seed\":" << config.seed
              << ",\"semantic_loss_per_10000\":" << config.loss_per_10000
              << ",\"semantic_duplication_per_10000\":" << config.duplication_per_10000
              << ",\"semantic_reorder_per_10000\":" << config.reorder_per_10000
              << ",\"semantic_jitter_ms\":30,\"semantic_reorder_delay_ms\":1500"
                 ",\"semantic_packet_limit\":16,\"semantic_byte_limit\":65536"
                 ",\"semantic_max_packet_bytes\":4096,\"semantic_max_drain_count\":8"
                 ",\"semantic_replay_verified\":true}\n";
    return 0;
}

int server(const Options& options) {
    const std::filesystem::path fixtures{options.at("--fixtures")},
        control{options.at("--control")};
    const auto& scenario = options.at("--scenario");
    const bool partition = scenario == "partition-reconnect", slow = scenario == "slow-reader";
    const bool relay = shaped(scenario) || partition;
    const unsigned target_tick = final_tick(scenario),
                   expected_connections = (partition || slow) ? 3U : 2U;
    SessionLimits session_limits;
    session_limits.accounts = 2;
    session_limits.characters = 2;
    session_limits.certificates = 2;
    session_limits.live_sessions = 2;
    auto sessions = take(SessionAuthority::create(session_limits));
    for (bool second : {false, true}) {
        take(sessions->provision_account(account(second)));
        take(sessions->assign_character(account(second), character(second)));
        take(sessions->enroll_certificate(
            account(second),
            take(certificate_fingerprint(
                file(fixtures / "session" / (second ? "client-b.pem" : "client-a.pem"))))));
    }
    InterestLimits limits;
    limits.replication.streams = 2;
    limits.replication.resyncs_per_window = 32;
    limits.replication.acknowledgement_timeout = slow ? 500ms : 5s;
    auto interest = take(InterestServer::create(limits));
    auto scene = take(Scene::create(
        RegionSceneScope{WorldId{id(1)}, {RegionId{id(2)}, RegionInstanceId{id(3)}, 1}}));
    std::vector<EntityHandle> entities;
    std::vector<InterestEntry> catalog;
    for (unsigned n = 0; n < 66; ++n) {
        const auto entity =
            take(scene->create_entity({WorldEntityId{id(1000 + n)}, {}, Name{canary}, pose(n, 0)}));
        entities.push_back(entity);
        catalog.push_back(
            {entity, n % 33 == 32 ? InterestVisibility::hidden : InterestVisibility::owner_pose,
             character(n >= 33), 0, false, n % 33 == 32});
    }
    auto listener = take(TlsListener::listen_loopback_authenticated(
        0, {file(fixtures / "server.pem"), file(fixtures / "server-key.pem")},
        file(fixtures / "ca.pem"), transport_limits(), 3));
    {
        std::ofstream ready{options.at("--ready")};
        ready << "{\"port\":" << take(listener->port()) << "}\n";
        ready.close();
        require(static_cast<bool>(ready), "Cannot publish readiness");
    }
    struct Peer {
        explicit Peer(std::unique_ptr<TlsTransport> value) : transport(std::move(value)) {}
        std::unique_ptr<TlsTransport> transport;
        std::optional<SessionSnapshot> session;
        unsigned index{0}, sent_tick{0};
        std::uint64_t sent_revision{0};
        bool final_ack{false}, logout{false};
    };
    std::vector<Peer> peers;
    peers.reserve(3);
    unsigned accepted = 0, ended = 0, tick = 0, accepted_controls = 0, rejected_controls = 0;
    unsigned malformed_controls = 0, slow_timeouts = 0, uncertain_disconnects = 0;
    unsigned healthy_during_fault = 0;
    std::array<bool, 2> final_acked{};
    std::optional<MonotonicTime> start;
    std::optional<unsigned> fault_start_tick;
    QueuePeak queue;
    std::size_t storage_peak = 0, pending_peak = 0, records_peak = 0, policy_peak = 0,
                selection_peak = 0;
    SteadyMonotonicClock clock;
    auto release = [&](Peer& peer) {
        if (peer.session) {
            take(interest->forget(peer.session->session));
            take(sessions->revoke_session(peer.session->session));
        }
        take(peer.transport->close());
        peer.transport.reset();
        ++ended;
    };
    while (clock.now() < 55s) {
        const auto now = clock.now();
        if (accepted < expected_connections) {
            if (auto next = take(listener->accept(now))) {
                peers.emplace_back(std::move(*next));
                ++accepted;
            }
        }
        if (!start && std::filesystem::exists(control / "a-primed") &&
            std::filesystem::exists(control / "b-primed")) {
            if (relay && !std::filesystem::exists(control / "relay-arm"))
                mark(control / "relay-arm");
            if (!relay || std::filesystem::exists(control / "relay-armed")) {
                start = now;
                mark(control / "source-start");
            }
        }
        if (start) {
            const auto due = static_cast<unsigned>(
                std::min<MonotonicTime::rep>(target_tick, (now - *start) / 50ms));
            require(due <= tick + 16, "Source accumulated sustained tick backlog");
            while (tick < due) {
                ++tick;
                for (unsigned n = 0; n < 66; ++n)
                    take(scene->set_transform(entities[n], pose(n, tick)));
            }
            if (tick == target_tick && !std::filesystem::exists(control / "faults-ended"))
                mark(control / "faults-ended");
            if (partition && now - *start >= 1s &&
                !std::filesystem::exists(control / "partition-start"))
                mark(control / "partition-start");
        }
        const bool fault_active =
            (partition && std::filesystem::exists(control / "partition-active") &&
             !std::filesystem::exists(control / "partition-dropped")) ||
            (slow && std::filesystem::exists(control / "a-slow-start") &&
             !std::filesystem::exists(control / "slow-expired")) ||
            (scenario == "hostile" && std::filesystem::exists(control / "hostile-start") &&
             !std::filesystem::exists(control / "hostile-ended"));
        if (fault_active && !fault_start_tick)
            fault_start_tick = tick;
        for (auto& peer : peers) {
            if (!peer.transport)
                continue;
            auto polled = peer.transport->poll(now, 16);
            if (!polled || polled.value().state == TransportState::closed) {
                require(peer.logout || ((partition || slow) && peer.index == 0 && !peer.final_ack),
                        "Unexpected server-side transport termination");
                if (partition && !peer.logout)
                    ++uncertain_disconnects;
                release(peer);
                continue;
            }
            queue.record(polled.value());
            if (polled.value().state != TransportState::ready)
                continue;
            for (unsigned drain = 0; drain < 4; ++drain) {
                auto frame = take(peer.transport->receive());
                if (!frame)
                    break;
                if (frame->kind == MessageKind::session_request) {
                    const auto request = take(decode_session_request(frame->payload));
                    auto result = sessions->apply(*peer.transport, request, now);
                    require(static_cast<bool>(result), "Unexpected session request rejection");
                    peer.session = result.value();
                    peer.index = result.value().account == account(true) ? 1U : 0U;
                    peer.logout = !result.value().active;
                    take(peer.transport->send(MessageKind::session_reply,
                                              take(encode_session_reply(result.value()))));
                } else {
                    require(frame->kind == MessageKind::replication_control && peer.session,
                            "Unexpected client frame kind");
                    const auto before = take(sessions->current(*peer.transport, now));
                    auto decoded = decode_replication_control(frame->payload);
                    bool accepted_control = false;
                    if (!decoded) {
                        ++malformed_controls;
                    } else {
                        auto result = interest->control(*scene, catalog, *sessions, *peer.transport,
                                                        decoded.value(), now);
                        accepted_control = static_cast<bool>(result);
                        if (accepted_control) {
                            ++accepted_controls;
                            if (std::holds_alternative<ReplicationAck>(decoded.value())) {
                                if (peer.sent_tick == target_tick &&
                                    std::get<ReplicationAck>(decoded.value()).revision ==
                                        peer.sent_revision &&
                                    result.value() == ReplicationControlResult::acknowledged) {
                                    peer.final_ack = true;
                                    final_acked[peer.index] = true;
                                }
                                if (peer.index == 1 && fault_active && fault_start_tick &&
                                    peer.sent_tick > *fault_start_tick &&
                                    result.value() == ReplicationControlResult::acknowledged)
                                    ++healthy_during_fault;
                            }
                        } else {
                            ++rejected_controls;
                        }
                    }
                    const auto after = take(sessions->current(*peer.transport, now));
                    require(accepted_control || after.last_sequence == before.last_sequence,
                            "Rejected control consumed session sequence");
                    const SessionReply reply =
                        accepted_control ? SessionReply{after} : SessionReply{SessionRejected{}};
                    take(peer.transport->send(MessageKind::session_reply,
                                              take(encode_session_reply(reply))));
                }
            }
            if (!peer.session || !peer.session->ready || !peer.session->active || peer.final_ack)
                continue;
            auto result = interest->publish(*scene, catalog, *sessions, *peer.transport, now);
            if (!result) {
                require(slow && peer.index == 0 &&
                            result.error().context.operation == "net.replication.ack-timeout" &&
                            slow_timeouts == 0,
                        "Unexpected managed publication failure");
                ++slow_timeouts;
                release(peer);
                mark(control / "slow-expired");
                continue;
            }
            const auto& publication = result.value();
            selection_peak = std::max(selection_peak, publication.work.selection_units());
            policy_peak =
                std::max(policy_peak,
                         publication.work.policy_rows + publication.work.policy_scene_reads +
                             publication.work.policy_comparisons + publication.work.privacy_checks);
            require(publication.selected_entities <= 32 && publication.records <= 64 &&
                        publication.work.selection_units() <= 8192,
                    "Selection work or record cap exceeded");
            if (publication.status == InterestPublishStatus::sent) {
                require(publication.selected_entities == 32, "Publication lost authorized actors");
                peer.sent_tick = tick;
                ++peer.sent_revision;
            }
            queue.record(take(peer.transport->stats()));
        }
        const auto state = take(interest->stats());
        const auto& history = state.replication;
        require(state.streams <= 2 && state.selected_entities <= 64 &&
                    state.last_sent_entities <= 64 && history.streams <= 2 &&
                    history.mapped_entities <= 64 && history.acknowledged_entities <= 64 &&
                    history.pending_updates <= 2 && history.pending_target_entities <= 64 &&
                    history.pending_change_records <= 128,
                "Integrated retained history exceeded hard count bounds");
        const auto storage = state.stream_capacity_bytes + state.selected_capacity_bytes +
                             state.last_sent_capacity_bytes + history.stream_capacity_bytes +
                             history.mapped_capacity_bytes + history.acknowledged_capacity_bytes +
                             history.pending_target_capacity_bytes +
                             history.pending_change_capacity_bytes;
        storage_peak = std::max(storage_peak, storage);
        pending_peak = std::max(pending_peak, history.pending_updates);
        records_peak = std::max(records_peak, history.pending_change_records);
        require(storage_peak <= interest_storage_limit,
                "Integrated owned history storage exceeded cap");
        if (accepted == expected_connections && ended == expected_connections) {
            require(final_acked[0] && final_acked[1] && tick == target_tick && state.streams == 0 &&
                        state.selected_capacity_bytes == 0 && state.last_sent_capacity_bytes == 0 &&
                        history.mapped_capacity_bytes == 0 &&
                        history.acknowledged_capacity_bytes == 0 &&
                        history.pending_target_capacity_bytes == 0 &&
                        history.pending_change_capacity_bytes == 0 &&
                        take(sessions->stats()).live_sessions == 0,
                    "Final convergence or authority cleanup failed");
            require(!(partition || slow || scenario == "hostile") || healthy_during_fault > 0,
                    "Healthy peer stalled during affected recipient failure");
            require(!slow || slow_timeouts == 1, "Missing slow-reader deadline failure");
            require(!partition || (uncertain_disconnects == 1 &&
                                   std::filesystem::exists(control / "uncertain-control-sent")),
                    "Partition did not exercise uncertain control admission");
            require(scenario != "hostile" || (malformed_controls == 1 && rejected_controls == 2),
                    "Integrated hostile input matrix incomplete");
            take(listener->close());
            mark(control / "relay-finish");
            listener.reset();
            interest.reset();
            sessions.reset();
            scene.reset();
            finish_monitor(options);
            std::cout << "{\"status\":\"PASS\",\"role\":\"server\",\"scenario\":\"" << scenario
                      << "\",\"source_tick\":" << tick << ",\"connections\":" << accepted
                      << ",\"candidates\":66,\"visible_per_recipient\":32,\"accepted_controls\":"
                      << accepted_controls << ",\"rejected_controls\":" << rejected_controls
                      << ",\"malformed_controls\":" << malformed_controls
                      << ",\"live_streams\":0,\"live_sessions\":0,\"max_queue_bytes\":"
                      << queue.bytes << ",\"max_queue_frames\":" << queue.frames
                      << ",\"max_interest_storage_bytes\":" << storage_peak
                      << ",\"interest_storage_limit_bytes\":" << interest_storage_limit
                      << ",\"max_replication_pending_updates\":" << pending_peak
                      << ",\"max_replication_pending_records\":" << records_peak
                      << ",\"max_policy_units\":" << policy_peak
                      << ",\"max_selection_units\":" << selection_peak
                      << ",\"independent_peer_progress\":"
                      << (healthy_during_fault > 0 ? "true" : "false")
                      << ",\"healthy_updates_during_fault\":" << healthy_during_fault
                      << ",\"slow_timeouts\":" << slow_timeouts
                      << ",\"uncertain_disconnects\":" << uncertain_disconnects
                      << ",\"final_cleanup\":true}\n";
            return 0;
        }
        cadence(clock);
    }
    throw std::runtime_error("Adversity server watchdog");
}

void publish_pid(const Options& options) {
    const std::filesystem::path path{options.at("--pid-file")};
    std::ofstream output{path};
#ifdef _WIN32
    output << GetCurrentProcessId() << '\n';
#else
    output << getpid() << '\n';
#endif
    output.close();
    require(static_cast<bool>(output), "Cannot write owned process identity");
    SteadyMonotonicClock clock;
    auto observed = path;
    observed += ".observed";
    while (!std::filesystem::exists(observed) && clock.now() < 2s)
        cadence(clock);
    require(std::filesystem::exists(observed), "Owned process resource monitor did not attach");
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::cout << std::setprecision(17);
        Options options;
        for (int n = 1; n < argc; n += 2)
            require(n + 1 < argc && options.emplace(argv[n], argv[n + 1]).second,
                    "Invalid adversity options");
        const auto& scenario = options.at("--scenario");
        require(scenario == "baseline" || shaped(scenario) || scenario == "partition-reconnect" ||
                    scenario == "slow-reader" || scenario == "hostile",
                "Unknown adversity scenario");
        publish_pid(options);
        if (options.at("--mode") == "server")
            return server(options);
        if (options.at("--mode") == "client")
            return client(options);
        throw std::runtime_error("Unknown adversity role");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::cout << "{\"status\":\"FAIL\",\"error\":\"adversity-fixture\"}\n";
        return 1;
    }
}
