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

namespace {
using namespace shark;
using namespace shark::net;
using namespace std::chrono_literals;
using Options = std::map<std::string, std::string>;
constexpr auto private_name = "PRIVATE-INTEREST-CANARY-c35f90b0";
constexpr unsigned final_tick = 440;
constexpr std::size_t queue_limit = 16 * 1024;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <typename T> T take(Result<T> result) {
    if (!result)
        throw std::runtime_error("Interest fixture failed: " + result.error().context.operation);
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result)
        throw std::runtime_error("Interest fixture failed: " + result.error().context.operation);
}
std::string file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    require(static_cast<bool>(input), "Cannot read explicit public fixture credential");
    std::string result{std::istreambuf_iterator<char>{input}, {}};
    require(!result.empty() && result.size() <= 65536, "Invalid fixture credential length");
    return result;
}
unsigned number(const Options& options, const char* name) {
    unsigned value = 0;
    const auto& text = options.at(name);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    require(result.ec == std::errc{} && result.ptr == text.data() + text.size(),
            "Invalid fixture number");
    return value;
}
StableId id(unsigned value) {
    std::string text = "4bd2395b-9372-4bb8-9f46-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 15U];
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
TransportLimits transport_limits() {
    TransportLimits limits;
    limits.queue_frames = 4;
    limits.queue_bytes = queue_limit;
    return limits;
}
void cadence(SteadyMonotonicClock& clock) {
    take(clock.wait_for(1ms));
}
void mark(const std::filesystem::path& path) {
    std::ofstream output{path};
    output << "ready\n";
    output.close();
    require(static_cast<bool>(output), "Cannot publish fixture barrier");
}
unsigned phase(unsigned tick) {
    return tick < 40 ? 0 : std::min(3U, (tick - 40) / 100);
}
std::set<unsigned> expected(unsigned phase_number, bool revoked = false) {
    std::set<unsigned> result;
    for (unsigned n = 0; n < 63; ++n) {
        const bool selected = phase_number == 0   ? n < 32
                              : phase_number == 1 ? n >= 31
                              : phase_number == 2 ? n % 2 == 0
                                                  : n == 0 || n % 2 == 1;
        if (selected && !(revoked && n == 1))
            result.insert(n);
    }
    if (revoked)
        result.insert(32);
    return result;
}
Transform pose(unsigned ordinal, unsigned tick) {
    Transform result;
    result.translation = {static_cast<double>((ordinal / 64) * 1000 + ordinal % 64),
                          static_cast<double>(tick) / 1000.0,
                          static_cast<double>(ordinal) / 1024.0};
    return result;
}
void privacy(std::span<const std::byte> bytes) {
    const std::string text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    require(text.find(private_name) == std::string::npos,
            "Private Name entered interest wire data");
    for (unsigned n = 0; n < 128; ++n) {
        const auto value = id(1000 + n);
        require(std::search(bytes.begin(), bytes.end(), value.bytes().begin(),
                            value.bytes().end()) == bytes.end(),
                "Durable entity identity entered interest wire data");
    }
}
unsigned verify(const ReplicaSnapshot& state, bool second, bool budget, bool revoked) {
    require(state.has_baseline && state.entities.size() == 32,
            "Interest did not retain32 visible actors");
    std::set<unsigned> observed;
    std::optional<unsigned> tick;
    for (const auto& entity : state.entities) {
        const auto ordinal =
            static_cast<unsigned>(std::llround(entity.pose.coefficients[11] * 1024));
        require(ordinal < 128 && ordinal / 64 == static_cast<unsigned>(second),
                "Foreign recipient pose disclosed");
        const auto current_tick =
            static_cast<unsigned>(std::llround(entity.pose.coefficients[7] * 1000));
        require(current_tick <= final_tick && (!tick || *tick == current_tick),
                "Mixed or invalid source ticks");
        tick = current_tick;
        auto wanted = ReplicatedPose{};
        wanted.coefficients[3] = static_cast<double>((ordinal / 64) * 1000 + ordinal % 64);
        wanted.coefficients[7] = static_cast<double>(current_tick) / 1000.0;
        wanted.coefficients[11] = static_cast<double>(ordinal) / 1024.0;
        require(entity.pose == wanted, "Interest pose differs from independent source schedule");
        require(observed.insert(ordinal % 64).second, "Duplicated visible actor");
    }
    require(tick && observed == expected(budget ? phase(*tick) : 0, revoked && !second),
            "Priority, visibility or churn selection differs from independent expected set");
    return *tick;
}

class Client final {
  public:
    Client(const std::filesystem::path& fixtures, std::uint16_t port, bool second,
           SteadyMonotonicClock& clock)
        : clock_(clock) {
        const std::string leaf = second ? "client-b" : "client-a";
        transport = take(TlsTransport::connect_loopback_authenticated(
            port, {file(fixtures / "ca.pem"), "shark.test"},
            {file(fixtures / "session" / (leaf + ".pem")),
             file(fixtures / "session" / (leaf + "-key.pem"))},
            clock.now(), transport_limits()));
        while (clock.now() < 40s) {
            if (take(transport->poll(clock.now())).state == TransportState::ready)
                break;
            cadence(clock);
        }
        require(take(transport->stats()).state == TransportState::ready, "Interest TLS watchdog");
        session = request(LoginRequest{account(second), character(second)});
        const auto ready_started = clock.now();
        session = request(SetReadyRequest{envelope(*session, 1), true});
        ready_reply_ms =
            std::chrono::duration<double, std::milli>{clock.now() - ready_started}.count();
        require(session->ready && session->active, "Interest session not ready");
        cache = take(ReplicaClient::create(*session));
    }
    void send(MessageKind kind, std::vector<std::byte> bytes) {
        take(transport->send(kind, bytes));
        std::fill(bytes.begin(), bytes.end(), std::byte{0});
    }
    Frame frame() {
        while (clock_.now() < 40s) {
            const auto polled = take(transport->poll(clock_.now(), 16));
            require(polled.operations <= 16 && polled.state != TransportState::closed,
                    "Interest transport closed unexpectedly");
            auto received = take(transport->receive());
            if (received)
                return std::move(*received);
            cadence(clock_);
        }
        throw std::runtime_error("Interest client reply watchdog");
    }
    SessionSnapshot request(const SessionRequest& request) {
        send(MessageKind::session_request, take(encode_session_request(request)));
        auto response = frame();
        require(response.kind == MessageKind::session_reply, "Unexpected fixture session response");
        auto decoded = take(decode_session_reply(response.payload));
        require(std::holds_alternative<SessionSnapshot>(decoded),
                "Interest fixture session rejected");
        return std::get<SessionSnapshot>(decoded);
    }
    ReplicationUpdate update() {
        auto value = frame();
        require(value.kind == MessageKind::replication_state, "Expected interest state frame");
        privacy(value.payload);
        bytes_received += value.payload.size();
        return take(decode_replication_update(value.payload));
    }
    void ack(const ReplicationUpdate& update) {
        send(MessageKind::replication_control,
             take(encode_replication_control(
                 ReplicationAck{envelope(*session, sequence++), update.stream, update.revision})));
    }
    void finish() {
        const auto before = take(cache->snapshot());
        require(!request(LogoutRequest{envelope(*session, sequence++)}).active,
                "Interest logout retained session");
        take(transport->close());
        transport.reset();
        require(take(cache->snapshot()).entities == before.entities,
                "Interest snapshot borrowed transport storage");
    }
    std::unique_ptr<TlsTransport> transport;
    std::optional<SessionSnapshot> session;
    std::unique_ptr<ReplicaClient> cache;
    std::uint64_t sequence{2};
    std::size_t bytes_received{0};
    double ready_reply_ms{0};

  private:
    SteadyMonotonicClock& clock_;
};

int client(const Options& options) {
    const auto port = number(options, "--port");
    require(port > 0 && port <= 65535, "Invalid interest port");
    const std::filesystem::path fixtures{options.at("--fixtures")},
        control{options.at("--control")};
    const bool second = options.at("--identity") == "b";
    const auto& scenario = options.at("--scenario");
    const bool budget = scenario == "crowd-churn-budget";
    const bool revocation = scenario == "policy-revocation";
    const bool hol = scenario == "head-of-line";
    SteadyMonotonicClock clock;
    auto connection =
        std::make_unique<Client>(fixtures, static_cast<std::uint16_t>(port), second, clock);
    unsigned applied = 0, measured = 0, latest_tick = 0, reconnects = 0;
    std::array<bool, 4> seen{};
    bool first = true, control_after_state = false;
    while (clock.now() < 40s) {
        auto update = connection->update();
        require(take(connection->cache->apply(update)) == ReplicaApplyResult::applied,
                "Unexpected repeated or missing interest baseline");
        const auto tick =
            verify(take(connection->cache->snapshot()), second, budget, reconnects != 0);
        require(first || tick > latest_tick, "Interest cache did not advance active source state");
        latest_tick = tick;
        ++applied;
        if (budget && tick >= 40 && tick < final_tick)
            ++measured;
        seen[budget ? phase(tick) : 0] = true;
        if (hol && !second && !first) {
            const auto reply = connection->frame();
            require(reply.kind == MessageKind::session_reply,
                    "Control response overtook or replaced interest state");
            const auto decoded = take(decode_session_reply(reply.payload));
            require(std::holds_alternative<SessionSnapshot>(decoded) &&
                        std::get<SessionSnapshot>(decoded).last_sequence == 3,
                    "HOL marker response was not the submitted heartbeat");
            control_after_state = true;
        }
        if (budget && !second && applied == 5) {
            // Explicit slow-ACK injection during warmup; source scheduling continues.
            const auto release = clock.now() + 175ms;
            while (clock.now() < release) {
                take(connection->transport->poll(clock.now()));
                require(!take(connection->transport->receive()),
                        "Pending state was resent before ACK");
                cadence(clock);
            }
        }
        connection->ack(update);
        if (first && revocation && !second) {
            bool closed = false;
            while (clock.now() < 40s) {
                auto polled = connection->transport->poll(clock.now());
                if (!polled) {
                    require(polled.error().context.operation == "net.tls.read" ||
                                polled.error().context.operation == "net.tls.write",
                            "Privacy fence failed by deadline");
                    closed = true;
                    break;
                }
                auto received = take(connection->transport->receive());
                require(!received, "Revoked queued state reached recipient");
                cadence(clock);
            }
            require(closed, "Privacy close watchdog");
            const auto old_session = connection->session->session;
            take(connection->transport->close());
            connection =
                std::make_unique<Client>(fixtures, static_cast<std::uint16_t>(port), false, clock);
            require(connection->session->session != old_session,
                    "Privacy reconnect reused authority");
            ++reconnects;
        }
        if (first && hol && !second) {
            while (!std::filesystem::exists(control / "armed") && clock.now() < 40s) {
                take(connection->transport->poll(clock.now()));
                cadence(clock);
            }
            require(std::filesystem::exists(control / "armed"), "HOL arm watchdog");
            connection->send(MessageKind::session_request,
                             take(encode_session_request(HeartbeatRequest{
                                 envelope(*connection->session, connection->sequence++)})));
        }
        first = false;
        if ((budget && tick == final_tick) || (!budget && tick == (revocation ? 2U : 1U)))
            break;
    }
    require(!budget || (latest_tick == final_tick && measured >= 190 &&
                        std::ranges::all_of(seen, [](bool value) { return value; })),
            "Active32-actor measured publication cadence or phase coverage failed");
    require(budget || applied == 2, "Interest functional stage count mismatch");
    connection->finish();
    std::cout << "{\"status\":\"PASS\",\"role\":\"client\",\"visible\":32,\"applied\":" << applied
              << ",\"measured_updates\":" << measured << ",\"final_tick\":" << latest_tick
              << ",\"warmup_ack_hold_ms\":" << (budget && !second ? 175 : 0)
              << ",\"ready_reply_ms\":" << connection->ready_reply_ms
              << ",\"reconnects\":" << reconnects
              << ",\"control_after_state\":" << (control_after_state ? "true" : "false")
              << ",\"privacy_checked\":true,\"owned_results\":true}\n";
    return 0;
}

template <typename Values> void json_values(const Values& values) {
    std::cout << '[';
    bool first = true;
    for (const auto& value : values) {
        if (!first)
            std::cout << ',';
        first = false;
        std::cout << value;
    }
    std::cout << ']';
}
double percentile(std::vector<double> values, double quantile) {
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    const auto index =
        static_cast<std::size_t>(std::ceil(quantile * static_cast<double>(values.size()))) - 1;
    return values[index];
}

int server(const Options& options) {
    const std::filesystem::path fixtures{options.at("--fixtures")},
        control{options.at("--control")};
    const auto& scenario = options.at("--scenario");
    const bool budget = scenario == "crowd-churn-budget",
               revocation = scenario == "policy-revocation", hol = scenario == "head-of-line";
    require(budget || revocation || hol, "Unknown interest scenario");
    auto sessions = take(SessionAuthority::create());
    for (bool second : {false, true}) {
        take(sessions->provision_account(account(second)));
        take(sessions->assign_character(account(second), character(second)));
        take(sessions->enroll_certificate(
            account(second),
            take(certificate_fingerprint(
                file(fixtures / "session" / (second ? "client-b.pem" : "client-a.pem"))))));
    }
    auto interest = take(InterestServer::create());
    auto scene = take(Scene::create(
        RegionSceneScope{WorldId{id(1)}, {RegionId{id(2)}, RegionInstanceId{id(3)}, 1}}));
    std::vector<EntityHandle> entities;
    std::vector<InterestEntry> catalog;
    for (unsigned n = 0; n < 128; ++n) {
        const auto entity = take(scene->create_entity(
            {WorldEntityId{id(1000 + n)}, {}, Name{private_name}, pose(n, 0)}));
        entities.push_back(entity);
        catalog.push_back({entity,
                           n % 64 == 63   ? InterestVisibility::hidden
                           : n % 64 == 62 ? InterestVisibility::public_pose
                                          : InterestVisibility::owner_pose,
                           character(n >= 64),
                           static_cast<std::uint8_t>(n % 64 < 32    ? 200
                                                     : n % 64 == 63 ? 255
                                                                    : 1),
                           false, n % 64 == 0});
    }
    auto listener = take(TlsListener::listen_loopback_authenticated(
        0, {file(fixtures / "server.pem"), file(fixtures / "server-key.pem")},
        file(fixtures / "ca.pem"), transport_limits()));
    {
        std::ofstream ready{options.at("--ready")};
        ready << "{\"port\":" << take(listener->port()) << "}\n";
        ready.close();
        require(static_cast<bool>(ready), "Cannot publish interest readiness");
    }
    struct Peer {
        explicit Peer(std::unique_ptr<TlsTransport> value) : transport(std::move(value)) {}
        std::unique_ptr<TlsTransport> transport;
        std::optional<SessionSnapshot> session;
        unsigned index{0};
        unsigned acknowledgements{0};
        unsigned sent_tick{0};
        bool final_ack{false};
        std::optional<SessionSnapshot> held_reply;
        std::uint64_t previous_down{0}, previous_up{0};
        std::vector<double> down_rates, up_rates;
        std::vector<std::uint64_t> down_bytes, up_bytes;
        std::uint64_t join_down{0}, join_up{0};
        std::optional<MonotonicTime> initial_admitted;
        double unshaped_state_ack_ms{0};
    };
    std::vector<Peer> peers;
    unsigned accepted = 0, ended = 0, tick = 0, functional_stage = 0, sample_count = 0;
    const unsigned expected_connections = revocation ? 3U : 2U;
    std::array<bool, 2> first_acked{}, final_acked{};
    std::optional<MonotonicTime> start, previous_sample, hol_admitted;
    MonotonicTime next_attempt{0};
    std::size_t max_queue_bytes = 0, max_queue_frames = 0, max_work = 0, max_catalog = 0;
    std::size_t max_policy_rows = 0, max_policy_reads = 0, max_policy_comparisons = 0;
    std::size_t max_privacy_checks = 0, max_eligible = 0, max_omitted = 0;
    unsigned privacy_closures = 0, revoked_queue_frames = 0, state_messages = 0;
    unsigned pending_newer_source = 0;
    bool privacy_bytes_unchanged = false, b_progress_before_a = false;
    double hol_ms = 0;
    std::vector<double> work_us, selection_work_us, sample_seconds;
    auto check_queues = [&](const TransportStats& stats) {
        max_queue_bytes =
            std::max({max_queue_bytes, stats.send_queue_bytes, stats.receive_queue_bytes});
        max_queue_frames =
            std::max({max_queue_frames, stats.send_queue_frames, stats.receive_queue_frames});
        require(max_queue_bytes <= queue_limit && max_queue_frames <= 4,
                "Interest transport queue bound exceeded");
    };
    auto mutate = [&](unsigned current) {
        tick = current;
        const auto wanted = expected(budget ? phase(tick) : 0);
        for (unsigned n = 0; n < 128; ++n) {
            take(scene->set_transform(entities[n], pose(n, tick)));
            catalog[n].priority = static_cast<std::uint8_t>(n % 64 == 63              ? 255
                                                            : wanted.contains(n % 64) ? 200
                                                                                      : 1);
        }
    };
    auto record = [&](const InterestPublishResult& result) {
        max_work = std::max(max_work, result.work.selection_units());
        max_catalog = std::max(max_catalog, result.work.catalog_rows);
        max_policy_rows = std::max(max_policy_rows, result.work.policy_rows);
        max_policy_reads = std::max(max_policy_reads, result.work.policy_scene_reads);
        max_policy_comparisons = std::max(max_policy_comparisons, result.work.policy_comparisons);
        max_privacy_checks = std::max(max_privacy_checks, result.work.privacy_checks);
        max_eligible = std::max(max_eligible, result.eligible_entities);
        max_omitted = std::max(max_omitted, result.omitted_entities);
        require(max_policy_rows <= 128 && max_policy_reads <= 128 &&
                    max_policy_comparisons <= 4096 && max_privacy_checks <= 32,
                "Unthrottled interest policy work exceeded catalog-derived bounds");
        require(result.work.selection_units() <= 8192 && result.work.catalog_rows <= 128 &&
                    result.selected_entities <= 32 && result.records <= 64,
                "Interest selection or replication work bound exceeded");
        if (result.status == InterestPublishStatus::sent) {
            require(result.selected_entities == 32, "Crowded fixture lost visible capacity");
            ++state_messages;
        }
    };
    SteadyMonotonicClock clock;
    while (clock.now() < 40s) {
        const auto now = clock.now();
        if (accepted < expected_connections) {
            auto next = take(listener->accept(now));
            if (next) {
                peers.push_back(Peer{std::move(*next)});
                ++accepted;
            }
        }
        for (auto& peer : peers) {
            if (!peer.transport)
                continue;
            auto polled = peer.transport->poll(now, 16);
            if (!polled || polled.value().state == TransportState::closed) {
                require(peer.session && (!budget || peer.final_ack),
                        "Interest peer ended before authorized completion");
                take(interest->forget(peer.session->session));
                take(sessions->revoke_session(peer.session->session));
                peer.transport.reset();
                ++ended;
                continue;
            }
            check_queues(polled.value());
            if (polled.value().state != TransportState::ready)
                continue;
            auto frame = take(peer.transport->receive());
            if (!frame)
                continue;
            if (frame->kind == MessageKind::session_request) {
                const auto request = take(decode_session_request(frame->payload));
                auto result = take(sessions->apply(*peer.transport, request, now));
                peer.session = result;
                peer.index = result.account == account(true) ? 1U : 0U;
                if (hol && peer.index == 0 && std::holds_alternative<HeartbeatRequest>(request)) {
                    peer.held_reply = result;
                } else {
                    take(peer.transport->send(MessageKind::session_reply,
                                              take(encode_session_reply(result))));
                }
            } else {
                require(frame->kind == MessageKind::replication_control,
                        "Unexpected interest client frame");
                const auto command = take(decode_replication_control(frame->payload));
                require(take(interest->control(*scene, catalog, *sessions, *peer.transport, command,
                                               now)) == ReplicationControlResult::acknowledged,
                        "Interest ACK rejected or duplicated");
                ++peer.acknowledgements;
                if (!first_acked[peer.index]) {
                    first_acked[peer.index] = true;
                    const auto metrics = take(peer.transport->stats());
                    peer.join_down = metrics.socket_bytes_sent;
                    peer.join_up = metrics.socket_bytes_received;
                    require(peer.initial_admitted.has_value(),
                            "Missing initial publication timestamp");
                    peer.unshaped_state_ack_ms =
                        std::chrono::duration<double, std::milli>{now - *peer.initial_admitted}
                            .count();
                    require(peer.join_down <= 32768 && peer.join_up <= 32768,
                            "Interest join burst exceeded32KiB direction cap");
                }
                if ((budget && peer.sent_tick == final_tick) ||
                    (!budget && functional_stage == 1 &&
                     peer.sent_tick == (revocation ? 2U : 1U))) {
                    peer.final_ack = true;
                    final_acked[peer.index] = true;
                    if (hol && peer.index == 0) {
                        require(hol_admitted.has_value(), "Missing HOL admission timestamp");
                        hol_ms =
                            std::chrono::duration<double, std::milli>{now - *hol_admitted}.count();
                        b_progress_before_a = final_acked[1];
                    }
                }
            }
        }
        if (!start && peers.size() >= 2 && std::ranges::all_of(peers, [](const Peer& peer) {
                return peer.session && peer.session->ready;
            }))
            start = now;
        if (start && budget) {
            const auto elapsed = now - *start;
            const auto due =
                static_cast<unsigned>(std::min<MonotonicTime::rep>(final_tick, elapsed / 50ms));
            require(due <= tick + 8, "Interest source schedule accumulated sustained backlog");
            while (tick < due)
                mutate(tick + 1);
            if (elapsed >= 2s && !previous_sample) {
                previous_sample = now;
                for (auto& peer : peers) {
                    const auto metrics = take(peer.transport->stats());
                    peer.previous_down = metrics.socket_bytes_sent;
                    peer.previous_up = metrics.socket_bytes_received;
                }
            } else if (previous_sample && sample_count < 20 &&
                       elapsed >= 2s + std::chrono::seconds{sample_count + 1}) {
                const auto seconds = std::chrono::duration<double>{now - *previous_sample}.count();
                require(sample_seconds.size() < 20, "Socket sample storage bound exceeded");
                sample_seconds.push_back(seconds);
                for (auto& peer : peers) {
                    const auto metrics = take(peer.transport->stats());
                    const auto down = metrics.socket_bytes_sent - peer.previous_down,
                               up = metrics.socket_bytes_received - peer.previous_up;
                    peer.down_bytes.push_back(down);
                    peer.up_bytes.push_back(up);
                    peer.down_rates.push_back(static_cast<double>(down) / seconds);
                    peer.up_rates.push_back(static_cast<double>(up) / seconds);
                    peer.previous_down = metrics.socket_bytes_sent;
                    peer.previous_up = metrics.socket_bytes_received;
                }
                previous_sample = now;
                ++sample_count;
            }
        }
        if (start && !budget && functional_stage == 0 && first_acked[0] && first_acked[1]) {
            if (revocation) {
                auto& a =
                    *std::ranges::find_if(peers, [](const Peer& peer) { return peer.index == 0; });
                mutate(1);
                const auto sent =
                    take(interest->publish(*scene, catalog, *sessions, *a.transport, now));
                record(sent);
                if (sent.status == InterestPublishStatus::sent) {
                    const auto queued = take(a.transport->stats());
                    revoked_queue_frames = static_cast<unsigned>(queued.send_queue_frames);
                    require(revoked_queue_frames == 1,
                            "Privacy fixture did not create an unpolled pending frame");
                    catalog[1].visibility = InterestVisibility::hidden;
                    mutate(2);
                    require(take(interest->publish(*scene, catalog, *sessions, *a.transport, now))
                                    .status == InterestPublishStatus::privacy_closed,
                            "Interest did not fence revoked publication");
                    const auto closed = take(a.transport->stats());
                    require(closed.state == TransportState::closed &&
                                closed.send_queue_frames == 0 &&
                                !sessions->current(*a.transport, now),
                            "Privacy closure retained queued state or session authority");
                    privacy_bytes_unchanged = closed.socket_bytes_sent == queued.socket_bytes_sent;
                    require(privacy_bytes_unchanged,
                            "Privacy fence wrote the revoked queued update");
                    ++privacy_closures;
                    functional_stage = 1;
                }
            } else {
                if (!std::filesystem::exists(control / "arm"))
                    mark(control / "arm");
                if (std::filesystem::exists(control / "armed")) {
                    mutate(1);
                    functional_stage = 1;
                }
            }
        }
        if (start && now >= next_attempt) {
            next_attempt = now + 1ms;
            for (auto& peer : peers) {
                if (!peer.transport || !peer.session || !peer.session->ready ||
                    !peer.session->active || peer.final_ack ||
                    take(peer.transport->stats()).state != TransportState::ready)
                    continue;
                // Privacy's staged unpolled publication is handled above, before any poll.
                if (revocation && first_acked[0] && first_acked[1] && functional_stage == 0)
                    continue;
                const auto begun = std::chrono::steady_clock::now();
                const auto result =
                    take(interest->publish(*scene, catalog, *sessions, *peer.transport, now));
                const auto wall_us =
                    std::chrono::duration<double, std::micro>{std::chrono::steady_clock::now() -
                                                              begun}
                        .count();
                require(work_us.size() < 100000 && selection_work_us.size() < 100000,
                        "Wall-work sample storage bound exceeded");
                work_us.push_back(wall_us);
                if (result.work.selection_units() > 0)
                    selection_work_us.push_back(wall_us);
                record(result);
                if (result.status == InterestPublishStatus::pending_ack && tick > peer.sent_tick)
                    ++pending_newer_source;
                if (result.status == InterestPublishStatus::sent) {
                    peer.sent_tick = tick;
                    if (!peer.initial_admitted)
                        peer.initial_admitted = now;
                }
                check_queues(take(peer.transport->stats()));
                if (hol && functional_stage == 1 && peer.index == 0 &&
                    result.status == InterestPublishStatus::sent)
                    hol_admitted = now;
                if (hol && hol_admitted && peer.index == 0 && peer.held_reply) {
                    take(peer.transport->send(MessageKind::session_reply,
                                              take(encode_session_reply(*peer.held_reply))));
                    peer.held_reply.reset();
                    check_queues(take(peer.transport->stats()));
                }
            }
        }
        if (accepted == expected_connections && ended == expected_connections) {
            require(final_acked[0] && final_acked[1],
                    "Interest fixture ended before final convergence");
            const auto metrics = take(interest->stats());
            require(metrics.streams == 0 && take(sessions->stats()).live_sessions == 0,
                    "Interest fixture leaked authority");
            require(!hol || (hol_ms >= 300 && hol_ms <= 3000 && b_progress_before_a),
                    "Measured HOL delay or independent-peer progress outside declared bounds");
            require(!budget || sample_count == 20, "Incomplete steady bandwidth sampling");
            std::cout << "{\"status\":\"PASS\",\"role\":\"server\",\"connections\":" << accepted
                      << ",\"candidates\":128,\"visible_per_recipient\":32,\"source_tick\":" << tick
                      << ",\"sample_count\":" << sample_count
                      << ",\"state_messages\":" << state_messages
                      << ",\"max_queue_bytes\":" << max_queue_bytes
                      << ",\"max_queue_frames\":" << max_queue_frames
                      << ",\"max_selection_units\":" << max_work
                      << ",\"max_catalog_rows\":" << max_catalog
                      << ",\"max_policy_rows\":" << max_policy_rows
                      << ",\"max_policy_scene_reads\":" << max_policy_reads
                      << ",\"max_policy_comparisons\":" << max_policy_comparisons
                      << ",\"max_privacy_checks\":" << max_privacy_checks
                      << ",\"max_eligible_entities\":" << max_eligible
                      << ",\"max_omitted_entities\":" << max_omitted
                      << ",\"selection_units\":" << metrics.selection_units
                      << ",\"privacy_checks\":" << metrics.privacy_checks
                      << ",\"policy_units\":" << metrics.policy_units
                      << ",\"coalesced\":" << metrics.coalesced
                      << ",\"pending_newer_source\":" << pending_newer_source
                      << ",\"framed_bytes\":" << metrics.framed_bytes
                      << ",\"records\":" << metrics.records
                      << ",\"all_call_wall_p95_us\":" << percentile(work_us, .95)
                      << ",\"all_call_wall_p99_us\":" << percentile(work_us, .99)
                      << ",\"selection_call_wall_p95_us\":" << percentile(selection_work_us, .95)
                      << ",\"selection_call_wall_p99_us\":" << percentile(selection_work_us, .99)
                      << ",\"all_call_samples\":" << work_us.size()
                      << ",\"selection_call_samples\":" << selection_work_us.size()
                      << ",\"privacy_closures\":" << privacy_closures
                      << ",\"revoked_queue_frames\":" << revoked_queue_frames
                      << ",\"privacy_bytes_unchanged\":"
                      << (privacy_bytes_unchanged ? "true" : "false")
                      << ",\"hol_completion_ms\":" << hol_ms << ",\"independent_peer_progress\":"
                      << (b_progress_before_a ? "true" : "false")
                      << ",\"live_streams\":0,\"live_sessions\":0,\"sample_seconds\":";
            json_values(sample_seconds);
            std::cout << ",\"recipients\":[";
            bool first_peer = true;
            for (const auto& peer : peers) {
                if (!first_peer)
                    std::cout << ',';
                first_peer = false;
                const auto down95 = percentile(peer.down_rates, .95),
                           up95 = percentile(peer.up_rates, .95);
                require(down95 <= 65536 && up95 <= 16384,
                        "Measured per-recipient socket bandwidth exceeded LOCAL-MMO-1 directional "
                        "targets");
                std::cout << "{\"identity\":" << peer.index
                          << ",\"acknowledgements\":" << peer.acknowledgements
                          << ",\"join_down\":" << peer.join_down << ",\"join_up\":" << peer.join_up
                          << ",\"unshaped_state_ack_ms\":" << peer.unshaped_state_ack_ms
                          << ",\"down_p95\":" << down95 << ",\"up_p95\":" << up95
                          << ",\"down_bytes\":";
                json_values(peer.down_bytes);
                std::cout << ",\"up_bytes\":";
                json_values(peer.up_bytes);
                std::cout << ",\"down_bytes_per_second\":";
                json_values(peer.down_rates);
                std::cout << ",\"up_bytes_per_second\":";
                json_values(peer.up_rates);
                std::cout << '}';
            }
            std::cout << "]}\n";
            return 0;
        }
        std::this_thread::yield();
    }
    throw std::runtime_error("Interest server watchdog");
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::cout << std::setprecision(17);
        Options options;
        for (int n = 1; n < argc; n += 2)
            require(n + 1 < argc && options.emplace(argv[n], argv[n + 1]).second,
                    "Invalid interest fixture options");
        if (options.at("--mode") == "server")
            return server(options);
        if (options.at("--mode") == "client")
            return client(options);
        throw std::runtime_error("Unknown interest fixture mode");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::cout << "{\"status\":\"FAIL\",\"error\":\"interest-fixture\"}\n";
        return 1;
    }
}
