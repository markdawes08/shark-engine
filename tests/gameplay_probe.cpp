#include <shark/net/combat_client.hpp>
#include <shark/net/fault_link.hpp>
#include <shark/net/movement_prediction.hpp>
#include <shark/net/replication.hpp>
#include <shark/scene.hpp>
#include <shark/server/combat.hpp>
#include <shark/time.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
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
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace shark;
using namespace shark::net;
using namespace shark::server;
using namespace shark::storage;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using Options = std::map<std::string, std::string>;
constexpr std::uint64_t final_tick = 320;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error{message};
}
template <typename T> T take(Result<T> result) {
    if (!result)
        throw std::runtime_error{result.error().context.operation + ": " + result.error().message};
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result)
        throw std::runtime_error{result.error().context.operation + ": " + result.error().message};
}
template <typename T> T id(unsigned value) {
    std::string text = "a04e0000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 15U];
        value >>= 4U;
    }
    return T{take(StableId::parse(text))};
}
AccountId account(bool second) {
    return id<AccountId>(second ? 102U : 101U);
}
fs::path path(const std::string& text) {
    return fs::path{std::u8string{text.begin(), text.end()}};
}
std::string file(const fs::path& source) {
    std::ifstream input{source, std::ios::binary};
    require(static_cast<bool>(input), "Cannot read gameplay fixture input");
    std::string result{std::istreambuf_iterator<char>{input}, {}};
    require(!result.empty() && result.size() <= 65536, "Invalid gameplay fixture input size");
    return result;
}
void publish(const fs::path& destination, const std::string& text = "ready\n") {
    require(!fs::exists(destination), "Gameplay rendezvous file already exists");
    auto temporary = destination;
    temporary += ".partial";
    std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
    output << text;
    output.close();
    require(static_cast<bool>(output), "Cannot publish gameplay rendezvous");
    fs::rename(temporary, destination);
}
void cadence(SteadyMonotonicClock& clock) {
    // Process/socket cadence only. Native tick and fault schedules use explicit clocks.
    take(clock.wait_for(1ms));
}
SessionEnvelope envelope(const SessionSnapshot& session, std::uint64_t sequence) {
    return {session.session, session.ownership_epoch, sequence, session.character};
}
bool shaped(const std::string& scenario) {
    return scenario != "baseline";
}
MonotonicTime one_way(const std::string& scenario) {
    return scenario == "baseline" ? 0ms : scenario == "rtt100-loss1" ? 50ms : 100ms;
}
FaultLinkConfig faults(const std::string& scenario) {
    FaultLinkConfig result;
    result.seed = scenario == "rtt100-loss1" ? 1 : 1337;
    result.maximum_packet_bytes = combat_maximum_reply_bytes;
    result.maximum_pending_packets = 64;
    result.maximum_pending_bytes = 65536;
    result.maximum_drain_count = 32;
    result.delay = one_way(scenario);
    if (shaped(scenario)) {
        result.jitter = 30ms;
        result.reorder_delay = 1500ms;
        result.loss_per_10000 = scenario == "rtt100-loss1" ? 100U : 500U;
        result.duplication_per_10000 = 5000;
        result.reorder_per_10000 = 3000;
    }
    return result;
}
struct Distribution {
    std::vector<double> values;
    void add(double value) {
        require(std::isfinite(value) && value >= 0 && values.size() < 512,
                "Gameplay measurement exceeded its bound");
        values.push_back(value);
    }
    double percentile(double fraction) const {
        if (values.empty())
            return 0;
        auto ordered = values;
        std::sort(ordered.begin(), ordered.end());
        return ordered[static_cast<std::size_t>(fraction *
                                                static_cast<double>(ordered.size() - 1))];
    }
    double maximum() const {
        return values.empty() ? 0 : *std::max_element(values.begin(), values.end());
    }
};

class Connection final {
  public:
    Connection(const Options& options, bool second, SteadyMonotonicClock& clock) : clock_(clock) {
        unsigned port = 0;
        const auto& text = options.at("--port");
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
        require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && port > 0 &&
                    port <= 65535,
                "Invalid gameplay fixture port");
        const auto fixtures = path(options.at("--fixtures"));
        transport = take(TlsTransport::connect_loopback_authenticated(
            static_cast<std::uint16_t>(port), {file(fixtures / "ca.pem"), "shark.test"},
            {file(fixtures / (second ? "session/client-b.pem" : "session/client-a.pem")),
             file(fixtures / (second ? "session/client-b-key.pem" : "session/client-a-key.pem"))},
            clock.now()));
        const auto deadline = clock.now() + 8s;
        while (clock.now() < deadline) {
            if (take(transport->poll(clock.now(), 16)).state == TransportState::ready)
                return;
            cadence(clock);
        }
        throw std::runtime_error{"Gameplay TLS readiness watchdog"};
    }
    SessionSnapshot exchange(const SessionRequest& request) {
        take(transport->send(MessageKind::session_request, take(encode_session_request(request))));
        const auto deadline = clock_.now() + 8s;
        while (clock_.now() < deadline) {
            take(transport->poll(clock_.now(), 16));
            auto frame = take(transport->receive());
            if (frame) {
                require(frame->kind == MessageKind::session_reply,
                        "Gameplay setup received an unexpected frame");
                const auto reply = take(decode_session_reply(frame->payload));
                require(std::holds_alternative<SessionSnapshot>(reply),
                        "Gameplay setup session request was rejected");
                ++session_replies;
                return std::get<SessionSnapshot>(reply);
            }
            cadence(clock_);
        }
        throw std::runtime_error{"Gameplay session exchange watchdog"};
    }
    std::unique_ptr<TlsTransport> transport;
    unsigned session_replies{0};

  private:
    SteadyMonotonicClock& clock_;
};

int run_client(const Options& options) {
    const auto control = path(options.at("--control"));
    const auto& scenario = options.at("--scenario");
    const bool second = options.at("--identity") == "b";
    const auto label = second ? "b" : "a";
    SteadyMonotonicClock clock;
    auto connection = std::make_unique<Connection>(options, second, clock);
    const auto discovery = control / (std::string{"character-"} + label + ".txt");
    while (!fs::exists(discovery) && clock.now() < 8s)
        cadence(clock);
    const auto character = CharacterId{take(StableId::parse(file(discovery)))};
    auto session = connection->exchange(LoginRequest{account(second), character});
    require(session.active && !session.ready && session.last_sequence == 0,
            "Gameplay login did not establish a fresh session");
    session = connection->exchange(SetReadyRequest{envelope(session, 1), true});
    require(session.ready && session.last_sequence == 1, "Gameplay readiness was not admitted");
    auto replica = take(ReplicaClient::create(session));
    std::unique_ptr<CombatClient> combat;
    std::unique_ptr<MovementPredictor> predictor;
    const auto fault_config = faults(scenario);
    auto link = take(FaultLink::create(fault_config));
    std::optional<CombatState> first_state;
    std::optional<ReplicationUpdate> last_replication;
    std::optional<CombatAction> original_action;
    std::set<std::pair<CombatStreamId, std::uint64_t>> effects;
    std::array<std::optional<MonotonicTime>, final_tick + 1> received_at{};
    Distribution corrections, latencies;
    std::uint64_t sequence = 2, next_action = 1, latest_tick = 0, last_delivery = 0;
    unsigned applied = 0, duplicates = 0, stale = 0, inversions = 0;
    unsigned replica_spawns = 0, replica_despawns = 0, replica_peak = 0;
    unsigned reconnects = 0, pauses = 0, extra_replies = 0, expected_rejections = 0;
    unsigned movement_inputs = 0, action_stage = 0, life_changes = 0, resyncs = 0;
    unsigned grant_rejections = 0, healthy_pause_ticks = 0, healthy_disconnected_ticks = 0;
    unsigned own_dead_states = 0, target_dead_states = 0;
    unsigned observed_health_mask = 0;
    bool observed_respawn = false;
    std::optional<CombatTargetId> observed_dead_life;
    double wall_correction = 0;
    std::size_t prediction_peak = 0;
    bool started = false, replayed = false, disconnected = false, final_seen = false;
    bool grant_sent = false, grant_pending = false;
    MonotonicTime next_input{0}, pause_until{0}, final_at{0}, first_action_at{0};

    const auto consume = [&](const std::vector<std::byte>& payload, MonotonicTime now) {
        const auto reply = take(decode_combat_reply(payload));
        const auto* state = std::get_if<CombatState>(&reply);
        require(state != nullptr, "Observation shaper received a generic rejection");
        if (state->completed_tick < last_delivery)
            ++inversions;
        last_delivery = state->completed_tick;
        if (state->session != session.session ||
            state->ownership_epoch != session.ownership_epoch ||
            state->completed_tick < latest_tick) {
            if (combat)
                require(!combat->apply(reply), "Stale gameplay observation was accepted");
            ++stale;
            return;
        }
        if (combat) {
            const auto before = take(combat->snapshot());
            if (state->stream != before.state.stream) {
                require(state->completed_tick > before.state.completed_tick && second,
                        "Unexpected gameplay life identity change");
                require(!combat->apply(reply), "Old combat owner rebound to a fresh life");
                combat = take(CombatClient::create(session, *state));
                predictor.reset();
                ++life_changes;
            } else {
                auto update = take(combat->apply(reply));
                if (update.status == CombatClientApplyResult::duplicate) {
                    require(update.effects.empty(), "Duplicate observation repeated an effect");
                    ++duplicates;
                    return;
                }
                if (update.status == CombatClientApplyResult::resync_required) {
                    require(update.effects.empty(), "Gapped state delivered uncertain effects");
                    take(combat->resynchronize(*state));
                    ++resyncs;
                }
                for (const auto& effect : update.effects)
                    require(effects.emplace(state->stream, effect.action_sequence).second,
                            "Presentation effect was delivered more than once");
            }
        } else {
            combat = take(CombatClient::create(session, *state));
            first_state = *state;
        }
        if (state->movement) {
            if (!predictor) {
                predictor =
                    take(MovementPredictor::create(session, *state->movement, {.gravity_y = 0}));
            } else {
                const auto before = take(predictor->snapshot());
                const auto correction = predictor->apply(MovementReply{*state->movement});
                require(static_cast<bool>(correction),
                        "Committed movement correction was rejected");
                if (correction.value() == PredictionCorrectionResult::applied) {
                    const auto distance =
                        glm::length(before.predicted.position - state->movement->position);
                    corrections.add(distance);
                    if (state->completed_tick < 40 && state->movement->position.x >= 1.25 &&
                        before.predicted.position.x > 1.41)
                        wall_correction = std::max(wall_correction, distance);
                }
            }
            const auto prediction = take(predictor->snapshot());
            prediction_peak = std::max(prediction_peak, prediction.pending_inputs);
            require(!prediction.reconciliation_required && prediction.pending_inputs <= 8,
                    "Gameplay prediction exceeded its bounded live history");
        } else if (predictor) {
            take(predictor->suspend());
            predictor.reset();
        }
        latest_tick = state->completed_tick;
        ++applied;
        own_dead_states += state->health == 0 ? 1U : 0U;
        target_dead_states +=
            !state->targets.empty() && state->targets.front().health == 0 ? 1U : 0U;
        if (second || !state->targets.empty()) {
            const auto health = second ? state->health : state->targets.front().health;
            const auto life = second ? state->self : state->targets.front().id;
            require(health <= 100 && health % 25 == 0,
                    "Observed health departed from literal melee profile");
            observed_health_mask |= 1U << ((100U - health) / 25U);
            if (health == 0)
                observed_dead_life = life;
            if (health == 100 && observed_dead_life && life != *observed_dead_life)
                observed_respawn = true;
        }
        if (!second && fs::exists(control / "pause-started") &&
            !fs::exists(control / "pause-ended"))
            ++healthy_pause_ticks;
        if (!second && fs::exists(control / "disconnect-started") &&
            !fs::exists(control / "reconnect-b"))
            ++healthy_disconnected_ticks;
        if (state->completed_tick != 0 && received_at[state->completed_tick])
            latencies.add(
                std::chrono::duration<double, std::milli>{now - *received_at[state->completed_tick]}
                    .count());
        if (!started) {
            started = true;
            next_input = now + 50ms;
            publish(control / (std::string{"started-"} + label));
        }
        if (latest_tick == final_tick && !final_seen) {
            final_seen = true;
            final_at = now;
        }
    };

    while (clock.now() < 50s) {
        auto now = clock.now();
        if (pause_until > now) {
            cadence(clock);
            continue;
        }
        if (pause_until != 0ns) {
            publish(control / "pause-ended");
            pause_until = 0ns;
        }
        if (disconnected && reconnects == 0) {
            if (!fs::exists(control / "reconnect-b")) {
                cadence(clock);
                continue;
            }
            const auto old_session = session;
            const auto old_state = take(combat->snapshot()).state;
            const auto old_replica = last_replication;
            extra_replies += connection->session_replies;
            connection = std::make_unique<Connection>(options, true, clock);
            auto logged = connection->exchange(LoginRequest{account(true), character});
            session = connection->exchange(SetReadyRequest{envelope(logged, 1), true});
            require(session.session != old_session.session &&
                        session.ownership_epoch > old_session.ownership_epoch,
                    "Reconnect reused old session authority");
            combat.reset();
            predictor.reset();
            replica = take(ReplicaClient::create(session));
            if (old_replica)
                require(!replica->apply(*old_replica), "Fresh replica accepted old session state");
            sequence = 2;
            // Old intent crosses the replacement's actual TLS connection and must
            // reject without consuming its fresh shared command sequence.
            const CombatAction old{envelope(old_session, 2), old_state.stream, 1,
                                   old_state.targets.front().id, CombatActionKind::strike};
            take(connection->transport->send(MessageKind::combat_action,
                                             take(encode_combat_action(old))));
            ++reconnects;
            now = clock.now();
        }
        const auto polled = take(connection->transport->poll(now, 16));
        require(polled.operations <= 16 && polled.state != TransportState::closed,
                "Gameplay client lost its live TLS transport");
        for (unsigned drain = 0; drain < 16; ++drain) {
            auto frame = take(connection->transport->receive());
            if (!frame)
                break;
            if (frame->kind == MessageKind::session_reply) {
                const auto reply = take(decode_session_reply(frame->payload));
                if (std::holds_alternative<SessionRejected>(reply)) {
                    require(grant_pending, "Shared sequence ACK was rejected");
                    grant_pending = false;
                    ++grant_rejections;
                }
                ++connection->session_replies;
            } else if (frame->kind == MessageKind::replication_state) {
                const auto update = take(decode_replication_update(frame->payload));
                const auto result = take(replica->apply(update, now));
                require(result != ReplicaApplyResult::resync_required,
                        "Reliable replica lost its baseline");
                if (result == ReplicaApplyResult::applied) {
                    for (const auto& change : update.changes) {
                        replica_spawns += change.kind == ReplicationChange::spawn ? 1U : 0U;
                        replica_despawns += change.kind == ReplicationChange::despawn ? 1U : 0U;
                    }
                }
                last_replication = update;
                const auto stats = take(replica->stats());
                require(stats.entities <= 2 && stats.presentation_frames <= 8,
                        "Gameplay replica history exceeded its bounds");
                replica_peak = std::max(replica_peak, static_cast<unsigned>(stats.entities));
                const auto sampled = take(replica->sample(now > 100ms ? now - 100ms : 0ns));
                require(sampled.entities.size() == stats.entities,
                        "Interpolation revived a despawned actor");
                take(connection->transport->send(
                    MessageKind::replication_control,
                    take(encode_replication_control(ReplicationAck{
                        envelope(session, sequence), update.stream, update.revision}))));
                ++sequence;
            } else {
                require(frame->kind == MessageKind::combat_state,
                        "Unexpected gameplay server frame");
                const auto decoded = take(decode_combat_reply(frame->payload));
                if (std::holds_alternative<CombatRejected>(decoded)) {
                    ++expected_rejections;
                    if (combat)
                        take(combat->apply(decoded));
                    continue;
                }
                const auto& state = std::get<CombatState>(decoded);
                require(state.completed_tick <= final_tick,
                        "Server exceeded declared tick workload");
                received_at[state.completed_tick] = now;
                // First binding and tick320 are explicit healthy reconciliation.
                // Ticks1..319 retain all seeded loss/duplicate/reorder counters.
                if (!combat || state.completed_tick == final_tick)
                    consume(frame->payload, now);
                else
                    require(take(link->send(now, frame->payload, state.completed_tick)).outcome !=
                                FaultSendOutcome::backpressure,
                            "Gameplay observation link exceeded bounded storage");
            }
        }
        for (const auto& delivery : take(link->drain(now, 32)))
            consume(delivery.payload, now);
        if (!combat) {
            cadence(clock);
            continue;
        }
        const auto state = take(combat->snapshot()).state;
        if (!second && movement_inputs < 20 && now >= next_input && predictor &&
            take(predictor->snapshot()).pending_inputs < 8) {
            const auto prepared = take(predictor->prepare(envelope(session, sequence), 1000, 0));
            require(take(predictor->send(*connection->transport, prepared)) ==
                        PredictionSendResult::admitted,
                    "New movement intent was not admitted once");
            ++sequence;
            ++movement_inputs;
            prediction_peak = std::max(prediction_peak, take(predictor->snapshot()).pending_inputs);
            next_input = now + 50ms;
        }
        constexpr std::array<std::uint64_t, 6> action_ticks{45, 46, 60, 70, 82, 94};
        if (!second && action_stage < action_ticks.size() &&
            latest_tick >= action_ticks[action_stage] &&
            (action_stage == 0 ||
             (action_stage == 1 ? now >= first_action_at + 150ms
                                : (!state.outcomes.empty() &&
                                   state.outcomes.back().action_sequence == action_stage)))) {
            require(!state.targets.empty(), "Combat client has no verified target selection");
            const CombatAction action{
                envelope(session, sequence), state.stream, next_action, state.targets.front().id,
                action_stage == 2 ? CombatActionKind::interact : CombatActionKind::strike};
            take(connection->transport->send(MessageKind::combat_action,
                                             take(encode_combat_action(action))));
            if (!original_action)
                original_action = action;
            if (action_stage == 0)
                first_action_at = now;
            ++sequence;
            ++next_action;
            ++action_stage;
        }
        if (!second && !replayed && latest_tick >= 120) {
            require(original_action.has_value(), "Missing original replay fixture intent");
            const auto bytes = take(encode_combat_action(*original_action));
            take(connection->transport->send(MessageKind::combat_action, bytes));
            take(connection->transport->send(MessageKind::combat_action, bytes));
            replayed = true;
        }
        if (!second && !grant_sent && latest_tick >= 130 && expected_rejections == 2) {
            auto invalid =
                take(encode_session_request(HeartbeatRequest{envelope(session, sequence)}));
            invalid[2] =
                std::byte{0x7f}; // Unsupported player-proposed grant kind; no endpoint exists.
            take(connection->transport->send(MessageKind::session_request, invalid));
            grant_sent = true;
            grant_pending = true;
        }
        if (second && pauses == 0 && latest_tick >= 170) {
            publish(control / "pause-started");
            pause_until = now + 750ms;
            ++pauses;
        }
        if (second && !disconnected && latest_tick >= 210) {
            require(first_state && state.movement && state.health == 100,
                    "Disconnect did not follow the completed respawn");
            publish(control / "disconnect-started");
            take(connection->transport->close());
            disconnected = true;
            continue;
        }
        if (final_seen && now - final_at >= 500ms && take(link->stats()).pending_packets == 0 &&
            take(replica->snapshot()).entities.size() == 2) {
            const auto final = take(combat->snapshot()).state;
            require(final.completed_tick == final_tick && final.health == 100 && final.movement &&
                        final.targets.size() == 1 && final.targets.front().health == 100 &&
                        std::abs(final.movement->position.x - (second ? -2.0 : 0.0)) < 0.0001 &&
                        std::abs(final.movement->position.y) < 0.0001 &&
                        std::abs(final.movement->position.z) < 0.0001,
                    "Final committed gameplay states did not converge");
            auto replicas = take(replica->snapshot()).entities;
            std::sort(replicas.begin(), replicas.end(), [](const auto& a, const auto& b) {
                return a.pose.coefficients[3] < b.pose.coefficients[3];
            });
            require(std::abs(replicas[0].pose.coefficients[3] + 2) < 0.0001 &&
                        std::abs(replicas[1].pose.coefficients[3]) < 0.0001,
                    "Final replicas disagree with literal committed world poses");
            require(replica_peak == 2 && replica_despawns >= (second ? 1U : 2U) &&
                        effects.size() <= 6 && observed_health_mask == 31 && observed_respawn &&
                        (!second || (reconnects == 1 && expected_rejections == 1)) &&
                        (second || (action_stage == 6 && movement_inputs == 20 &&
                                    expected_rejections == 2 && grant_rejections == 1 &&
                                    healthy_pause_ticks > 0 && healthy_disconnected_ticks > 0)),
                    "Gameplay client missed a connected lifecycle");
            if (!second && !shaped(scenario))
                require(effects.size() == 6,
                        "Healthy presentation did not deliver all six outcomes once");
            if (!second)
                require(wall_correction > 0.05,
                        "Static wall produced no actual prediction correction");
            const auto injected = take(link->stats());
            if (shaped(scenario))
                require(injected.loss_drops > 0 && injected.duplicated_messages > 0 &&
                            inversions > 0,
                        "Declared gameplay adversity was not observed");
            publish(control / (std::string{"client-"} + label + ".done"));
            std::cout
                << "{\"status\":\"PASS\",\"role\":\"client\",\"scenario\":\"" << scenario
                << "\",\"identity\":\"" << label << "\",\"authenticated\":" << (1 + reconnects)
                << ",\"latest_tick\":" << latest_tick
                << ",\"session_replies\":" << connection->session_replies + extra_replies
                << ",\"reconnects\":" << reconnects << ",\"pauses\":" << pauses
                << ",\"states_applied\":" << applied << ",\"duplicate_observations\":" << duplicates
                << ",\"stale_observations\":" << stale << ",\"loss_drops\":" << injected.loss_drops
                << ",\"duplicated_messages\":" << injected.duplicated_messages
                << ",\"inversions\":" << inversions
                << ",\"maximum_correction_m\":" << corrections.maximum()
                << ",\"wall_correction_m\":" << wall_correction
                << ",\"correction_p50_m\":" << corrections.percentile(0.5)
                << ",\"correction_p95_m\":" << corrections.percentile(0.95)
                << ",\"maximum_delivery_latency_ms\":" << latencies.maximum()
                << ",\"delivery_latency_p50_ms\":" << latencies.percentile(0.5)
                << ",\"delivery_latency_p95_ms\":" << latencies.percentile(0.95)
                << ",\"prediction_peak\":" << prediction_peak
                << ",\"replica_spawns\":" << replica_spawns
                << ",\"replica_despawns\":" << replica_despawns
                << ",\"replica_peak\":" << replica_peak << ",\"effects\":" << effects.size()
                << ",\"life_changes\":" << life_changes << ",\"resyncs\":" << resyncs
                << ",\"final_x\":" << final.movement->position.x
                << ",\"final_y\":" << final.movement->position.y
                << ",\"final_z\":" << final.movement->position.z
                << ",\"final_health\":" << final.health
                << ",\"final_target_health\":" << final.targets.front().health
                << ",\"final_replica_entities\":" << replicas.size() << ",\"final_replica_x\":["
                << replicas[0].pose.coefficients[3] << ',' << replicas[1].pose.coefficients[3]
                << ']' << ",\"grant_rejections\":" << grant_rejections
                << ",\"healthy_pause_ticks\":" << healthy_pause_ticks
                << ",\"healthy_disconnected_ticks\":" << healthy_disconnected_ticks
                << ",\"own_dead_states\":" << own_dead_states
                << ",\"target_dead_states\":" << target_dead_states
                << ",\"observed_health_mask\":" << observed_health_mask
                << ",\"observed_respawn\":" << (observed_respawn ? "true" : "false")
                << ",\"semantic_seed\":" << fault_config.seed
                << ",\"semantic_loss_per_10000\":" << fault_config.loss_per_10000
                << ",\"semantic_duplication_per_10000\":" << fault_config.duplication_per_10000
                << ",\"semantic_reorder_per_10000\":" << fault_config.reorder_per_10000
                << ",\"semantic_reorder_delay_ms\":"
                << std::chrono::duration_cast<std::chrono::milliseconds>(fault_config.reorder_delay)
                       .count()
                << ",\"semantic_delay_ms\":"
                << std::chrono::duration_cast<std::chrono::milliseconds>(fault_config.delay).count()
                << "}\n";
            take(connection->transport->close());
            return 0;
        }
        cadence(clock);
    }
    throw std::runtime_error{"Gameplay client watchdog"};
}

struct DelayedFrame {
    MonotonicTime due;
    Frame frame;
};
struct Peer {
    std::unique_ptr<TlsTransport> transport;
    bool authenticated{false};
    bool second{false};
    bool bound{false};
    std::optional<SessionSnapshot> session{};
    std::optional<std::uint64_t> sent_tick{};
    std::deque<DelayedFrame> requests{};
};

int run_server(const Options& options) {
    const auto fixtures = path(options.at("--fixtures"));
    const auto control = path(options.at("--control"));
    const auto& scenario = options.at("--scenario");
    const auto world = id<WorldId>(1);
    const WorldPosition origin{world, {}, {0, 0, 0}};
    SessionLimits limits;
    limits.auth_attempts_per_window = 128;
    limits.commands_per_window = 128;
    auto authority = take(
        CharacterAuthority::create(take(Store::create(control / "Gameplay.db", world)), limits));
    const std::array fingerprints{
        take(certificate_fingerprint(file(fixtures / "session/client-a.pem"))),
        take(certificate_fingerprint(file(fixtures / "session/client-b.pem")))};
    for (const bool second : {false, true}) {
        take(authority->provision_account(account(second)));
        take(authority->enroll_certificate(account(second), fingerprints[second ? 1U : 0U]));
    }
    auto listener = take(TlsListener::listen_loopback_authenticated(
        0, {file(fixtures / "server.pem"), file(fixtures / "server-key.pem")},
        file(fixtures / "ca.pem")));
    publish(path(options.at("--ready")),
            "{\"port\":" + std::to_string(take(listener->port())) + "}\n");
    SteadyMonotonicClock clock;
    std::array<std::optional<CharacterRecord>, 2> records;
    std::array<std::optional<EntityHandle>, 2> entities;
    std::array<unsigned, 2> bindings{};
    std::vector<Peer> peers;
    peers.reserve(3);
    std::unique_ptr<Scene> scene;
    std::unique_ptr<PhysicsWorld> physics;
    std::unique_ptr<CharacterMovement> movement;
    std::unique_ptr<CharacterCombat> combat;
    auto publisher = take(ReplicationServer::create());
    std::unique_ptr<ServerScheduler> schedule;
    unsigned authenticated = 0, reconnects = 0, deaths = 0, respawns = 0;
    unsigned hits = 0, cooldowns = 0, interactions = 0, replay_rejections = 0;
    unsigned grant_rejections = 0;
    unsigned pause_world_ticks = 0, disconnected_world_ticks = 0, canonical_states = 0;
    std::optional<std::uint64_t> death_tick, respawn_tick, detached_tick;
    double wall_max_x = 0;
    std::uint64_t tick = 0;
    std::array<std::optional<CombatState>, 2> final_states;

    const auto state_for = [&](Peer& peer, MonotonicTime now) {
        std::vector<CombatTargetId> visible;
        for (auto& other : peers) {
            if (&other == &peer || !other.transport || !other.bound)
                continue;
            const auto own =
                take(combat->view(*authority, *other.transport, *scene, *physics, {}, now));
            visible.push_back(own.self);
        }
        return take(combat->view(*authority, *peer.transport, *scene, *physics, visible, now));
    };
    const auto send_state = [&](Peer& peer, MonotonicTime now) {
        require(!peer.sent_tick || *peer.sent_tick != tick,
                "Two canonical gameplay states for one recipient/tick");
        const auto state = state_for(peer, now);
        if (tick == final_tick)
            final_states[peer.second ? 1U : 0U] = state;
        take(peer.transport->send(MessageKind::combat_state, take(encode_combat_reply(state))));
        peer.sent_tick = tick;
        ++canonical_states;
    };

    while (clock.now() < 50s) {
        const auto now = clock.now();
        if (peers.size() < 3) {
            auto accepted = take(listener->accept(now));
            if (accepted)
                peers.push_back({std::move(*accepted)});
        }
        for (auto& peer : peers) {
            if (!peer.transport)
                continue;
            auto polled = peer.transport->poll(now, 16);
            if (!polled || polled.value().state == TransportState::closed) {
                require(peer.authenticated && (peer.second || tick == final_tick),
                        "Unexpected gameplay connection failure");
                if (peer.session) {
                    take(authority->revoke_session(peer.session->session));
                    take(publisher->forget(peer.session->session));
                }
                peer.transport.reset();
                peer.bound = false;
                peer.requests.clear();
                continue;
            }
            require(polled.value().operations <= 16, "Gameplay server poll exceeded budget");
            if (polled.value().state != TransportState::ready)
                continue;
            if (!peer.authenticated) {
                const auto proven = take(peer.transport->authenticated_peer());
                require(proven.certificate == fingerprints[0] ||
                            proven.certificate == fingerprints[1],
                        "Unexpected gameplay mTLS identity");
                peer.second = proven.certificate == fingerprints[1];
                const auto index = peer.second ? 1U : 0U;
                if (!records[index]) {
                    const auto roster =
                        take(authority->roster(*peer.transport, account(peer.second), now));
                    records[index] = take(authority->create_character(
                        *peer.transport, account(peer.second), roster.revision,
                        {Name{peer.second ? "B" : "A"}, origin}, now));
                    publish(control /
                                (std::string{"character-"} + (peer.second ? "b" : "a") + ".txt"),
                            records[index]->character.value().to_string());
                }
                peer.authenticated = true;
                ++authenticated;
            }
            for (unsigned drain = 0; drain < 16; ++drain) {
                auto frame = take(peer.transport->receive());
                if (!frame)
                    break;
                require(peer.requests.size() < 64,
                        "Reliable gameplay request delay exceeded its cap");
                peer.requests.push_back({now + one_way(scenario), std::move(*frame)});
            }
        }
        if (!scene && records[0] && records[1]) {
            scene = take(Scene::create(
                RegionSceneScope{world, {id<RegionId>(2), id<RegionInstanceId>(3), 1}}));
            PhysicsWorldConfig profile;
            profile.tick_hz = movement_tick_hz;
            profile.collision_substeps = movement_collision_substeps;
            profile.gravity = {};
            physics = take(PhysicsWorld::create(profile));
            for (std::size_t index = 0; index < entities.size(); ++index) {
                EntitySeed seed{records[index]->entity};
                seed.transform.translation.x = index == 0 ? 0 : -2;
                seed.rigid_body = RigidBody{};
                seed.rigid_body->motion = BodyMotion::dynamic_body;
                seed.rigid_body->shape = ColliderShape::sphere;
                seed.rigid_body->friction = 0;
                entities[index] = take(scene->create_entity(seed));
                take(physics->create_body(*scene, *entities[index]));
            }
            EntitySeed wall{id<WorldEntityId>(500)};
            wall.transform.translation.x = 2;
            wall.rigid_body = RigidBody{};
            wall.rigid_body->half_extents = {0.1, 10, 10};
            take(physics->create_body(*scene, take(scene->create_entity(wall))));
            take(physics->begin_simulation(*scene));
            movement = take(CharacterMovement::create(*scene, *physics, origin));
            combat = take(CharacterCombat::create(
                *scene, *physics, take(CombatRules::create({"scripts/combat.lua", R"lua(
                    function on_combat(kind, tick, actor, target, distance)
                        if kind==1 then return true,25,10 end
                        return true,0,5
                    end
                )lua"}))));
        }
        if (scene) {
            for (auto& peer : peers) {
                if (!peer.transport)
                    continue;
                while (!peer.requests.empty() && peer.requests.front().due <= now) {
                    auto frame = std::move(peer.requests.front().frame);
                    peer.requests.pop_front();
                    const auto index = peer.second ? 1U : 0U;
                    if (frame.kind == MessageKind::session_request) {
                        const auto decoded = decode_session_request(frame.payload);
                        if (!decoded) {
                            require(peer.bound && !peer.second && frame.payload.size() == 52 &&
                                        frame.payload[2] == std::byte{0x7f},
                                    "Unexpected malformed gameplay session request");
                            const auto before = take(authority->authorize_entity(
                                *peer.transport, *scene, *entities[index], now));
                            take(peer.transport->send(
                                MessageKind::session_reply,
                                take(encode_session_reply(SessionRejected{}))));
                            const auto after = take(authority->authorize_entity(
                                *peer.transport, *scene, *entities[index], now));
                            require(before.last_sequence == after.last_sequence,
                                    "Unsupported item grant consumed shared session admission");
                            ++grant_rejections;
                            continue;
                        }
                        const auto& request = decoded.value();
                        peer.session = take(authority->apply(*peer.transport, request, now));
                        take(peer.transport->send(MessageKind::session_reply,
                                                  take(encode_session_reply(*peer.session))));
                        if (peer.session->ready && !peer.bound) {
                            if (bindings[index] == 0) {
                                take(combat->bind(*authority, *peer.transport, *movement, *scene,
                                                  *physics, *entities[index], now));
                            } else {
                                const auto restored = take(
                                    combat->reconnect(*authority, *peer.transport, *movement,
                                                      *scene, *physics, *entities[index], now));
                                require(!restored.error && restored.state &&
                                            restored.state->health == 100,
                                        "Gameplay reconnect changed retained life state");
                                ++reconnects;
                            }
                            ++bindings[index];
                            peer.bound = true;
                            send_state(peer, now);
                        }
                    } else if (frame.kind == MessageKind::movement_input) {
                        require(peer.bound, "Movement preceded checked gameplay binding");
                        take(movement->submit(*authority, *peer.transport, *scene, *physics,
                                              take(decode_movement_input(frame.payload)), now));
                    } else if (frame.kind == MessageKind::combat_action) {
                        const auto before = take(authority->authorize_entity(
                            *peer.transport, *scene, *entities[index], now));
                        const auto admitted =
                            combat->submit(*authority, *peer.transport, *scene, *physics,
                                           take(decode_combat_action(frame.payload)), now);
                        if (!admitted) {
                            const auto after = take(authority->authorize_entity(
                                *peer.transport, *scene, *entities[index], now));
                            require(
                                after.last_sequence == before.last_sequence,
                                "Rejected old action consumed a fresh session command sequence");
                            ++replay_rejections;
                            take(peer.transport->send(MessageKind::combat_state,
                                                      take(encode_combat_reply(CombatRejected{}))));
                        }
                    } else {
                        require(frame.kind == MessageKind::replication_control && peer.session,
                                "Unexpected gameplay request kind");
                        take(authority->control_replication(
                            *publisher, *peer.transport,
                            take(decode_replication_control(frame.payload)), now));
                        const auto current = take(authority->authorize_entity(
                            *peer.transport, *scene, *entities[index], now));
                        take(peer.transport->send(MessageKind::session_reply,
                                                  take(encode_session_reply(current))));
                    }
                }
            }
        }
        if (!schedule && combat && bindings[0] == 1 && bindings[1] == 1 &&
            fs::exists(control / "started-a") && fs::exists(control / "started-b"))
            schedule = take(
                ServerScheduler::create({.tick_rate_hz = 20, .max_ticks_per_pass = 1}, now, 1));
        if (schedule && tick < final_tick) {
            const TickCallback update = [&](std::uint64_t scheduled) -> Result<void> {
                require(scheduled == tick + 1, "Gameplay scheduler skipped a native tick");
                if (scheduled == 40) {
                    // Trusted fixture staging only, through the checked Physics owner.
                    // The player protocol never carries a position/teleport intent.
                    for (std::size_t index = 0; index < entities.size(); ++index) {
                        const auto reference = take(scene->reference(*entities[index]));
                        const auto body = take(physics->find_body(*scene, reference));
                        auto pose = take(physics->body_state(*scene, body)).pose;
                        pose.translation = {index == 0 ? 0.0 : 1.2, 0, 0};
                        take(physics->teleport(*scene, body, pose));
                    }
                }
                std::vector<TlsTransport*> live;
                for (auto& peer : peers)
                    if (peer.transport && peer.bound)
                        live.push_back(peer.transport.get());
                const auto report =
                    take(combat->step(*authority, *movement, *scene, *physics, live, now));
                require(!report.error && report.movement_completed &&
                            report.movement.completed_tick == scheduled,
                        "Connected gameplay tick retained a partial failure");
                tick = scheduled;
                require(take(physics->stats()).completed_ticks == tick &&
                            take(physics->stats()).pending_events == 0,
                        "Native gameplay tick/contact lifetime diverged");
                if (tick < 40) {
                    const auto x =
                        take(scene->entity_state(*entities[0])).local_transform.translation.x;
                    wall_max_x = std::max(wall_max_x, x);
                    // Reuse M04a's qualified wall bound. Native penetration slop
                    // is 0.02 m, so a new ideal-surface + 0.01 oracle is invalid.
                    require(x < 1.6, "Authoritative movement crossed the static collider");
                }
                for (const auto& result : report.outcomes) {
                    if (result.outcome.outcome == CombatOutcome::hit) {
                        require(result.outcome.amount == 25,
                                "Server Lua changed literal melee damage");
                        ++hits;
                    } else if (result.outcome.outcome == CombatOutcome::cooldown)
                        ++cooldowns;
                    else if (result.outcome.outcome == CombatOutcome::interacted) {
                        require(result.outcome.amount == 0,
                                "Interaction produced a reward/damage value");
                        ++interactions;
                    } else
                        throw std::runtime_error{"Unexpected connected combat outcome"};
                }
                deaths += static_cast<unsigned>(report.deaths);
                respawns += static_cast<unsigned>(report.respawns);
                if (report.deaths)
                    death_tick = tick;
                if (report.respawns) {
                    respawn_tick = tick;
                    require(death_tick && tick == *death_tick + 60,
                            "Native respawn did not preserve the original 60-tick deadline");
                }
                if (fs::exists(control / "pause-started") && !fs::exists(control / "pause-ended"))
                    ++pause_world_ticks;
                const auto combat_stats = take(combat->stats());
                if (combat_stats.detached_actors != 0) {
                    ++disconnected_world_ticks;
                    if (!detached_tick) {
                        detached_tick = tick;
                        publish(control / "detached-b");
                    }
                    if (tick >= 230 && !fs::exists(control / "reconnect-b"))
                        publish(control / "reconnect-b");
                }
                for (auto& peer : peers)
                    if (peer.transport && peer.bound)
                        send_state(peer, now);
                return {};
            };
            take(schedule->poll(now, update));
        }
        if (combat) {
            std::vector<EntityHandle> selected;
            for (const auto& reference : take(combat->active_entities(*scene, *physics)))
                selected.push_back(reference.handle);
            for (auto& peer : peers)
                if (peer.transport && peer.bound)
                    take(authority->publish_replication(*publisher, *scene, selected,
                                                        *peer.transport, now, 4096));
        }
        if (tick == final_tick && fs::exists(control / "client-a.done") &&
            fs::exists(control / "client-b.done")) {
            require(authenticated == 3 && reconnects == 1 && deaths == 1 && respawns == 1 &&
                        hits == 4 && cooldowns == 1 && interactions == 1 &&
                        replay_rejections == 3 && grant_rejections == 1 && final_states[0] &&
                        final_states[1] && final_states[0]->movement && final_states[1]->movement &&
                        pause_world_ticks >= 10 && disconnected_world_ticks >= 10 &&
                        wall_max_x >= 1.25 && death_tick && respawn_tick &&
                        *respawn_tick == *death_tick + 60,
                    "Connected gameplay did not complete its declared acceptance timeline");
            require(take(scene->stats()).live_entities == 3 && take(physics->stats()).bodies == 3,
                    "Gameplay lifecycle leaked native or Scene actors");
            std::cout
                << "{\"status\":\"PASS\",\"role\":\"server\",\"scenario\":\"" << scenario
                << "\",\"ticks\":" << tick << ",\"authenticated\":" << authenticated
                << ",\"reconnects\":" << reconnects << ",\"deaths\":" << deaths
                << ",\"respawns\":" << respawns << ",\"hits\":" << hits
                << ",\"cooldowns\":" << cooldowns << ",\"interactions\":" << interactions
                << ",\"replay_rejections\":" << replay_rejections
                << ",\"pause_world_ticks\":" << pause_world_ticks
                << ",\"grant_rejections\":" << grant_rejections
                << ",\"disconnected_world_ticks\":" << disconnected_world_ticks
                << ",\"wall_max_x\":" << wall_max_x << ",\"canonical_states\":" << canonical_states
                << ",\"death_tick\":" << *death_tick << ",\"respawn_tick\":" << *respawn_tick
                << ",\"request_delay_ms\":"
                << std::chrono::duration_cast<std::chrono::milliseconds>(one_way(scenario)).count()
                << ",\"shaped_tick_limit\":319,\"healthy_final_tick\":320"
                << ",\"final_a_x\":" << final_states[0]->movement->position.x
                << ",\"final_b_x\":" << final_states[1]->movement->position.x
                << ",\"final_a_health\":" << final_states[0]->health
                << ",\"final_b_health\":" << final_states[1]->health
                << ",\"live_entities\":" << take(scene->stats()).live_entities
                << ",\"bodies\":" << take(physics->stats()).bodies << "}\n";
            return 0;
        }
        cadence(clock);
    }
    throw std::runtime_error{"Gameplay server watchdog"};
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::cout << std::setprecision(17);
        Options options;
        for (int index = 1; index < argc; index += 2)
            require(index + 1 < argc && options.emplace(argv[index], argv[index + 1]).second,
                    "Invalid gameplay probe options");
        const auto& scenario = options.at("--scenario");
        require(scenario == "baseline" || scenario == "rtt100-loss1" || scenario == "rtt200-loss5",
                "Unknown gameplay adversity profile");
        if (options.at("--mode") == "server")
            return run_server(options);
        require(options.at("--mode") == "client" &&
                    (options.at("--identity") == "a" || options.at("--identity") == "b"),
                "Unknown gameplay probe role");
        return run_client(options);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::cout << "{\"status\":\"FAIL\",\"error\":\"gameplay-fixture\"}\n";
        return 1;
    }
}
