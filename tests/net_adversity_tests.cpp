#include <shark/net/interest.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <thread>

namespace {
using namespace shark;
using namespace shark::net;
using namespace std::chrono_literals;

template <typename Id> Id id(std::uint32_t value) {
    std::string text = "aabbccdd-1122-4334-8556-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 0xfU];
        value >>= 4U;
    }
    auto parsed = StableId::parse(text);
    REQUIRE(parsed);
    return Id{parsed.value()};
}

std::string fixture(const std::string& name) {
    std::ifstream input{std::filesystem::path{SHARK_NETWORK_FIXTURE_DIRECTORY} / name,
                        std::ios::binary};
    REQUIRE(input);
    return {std::istreambuf_iterator<char>{input}, {}};
}

struct Pair {
    std::unique_ptr<TlsListener> listener;
    std::unique_ptr<TlsTransport> client;
    std::unique_ptr<TlsTransport> server;

    explicit Pair(MonotonicTime now = 0ns) {
        auto listening = TlsListener::listen_loopback_authenticated(
            0, {fixture("server.pem"), fixture("server-key.pem")}, fixture("ca.pem"));
        REQUIRE(listening);
        listener = std::move(listening).value();
        auto connecting = TlsTransport::connect_loopback_authenticated(
            listener->port().value(), {fixture("ca.pem"), "shark.test"},
            {fixture("session/client-a.pem"), fixture("session/client-a-key.pem")}, now);
        REQUIRE(connecting);
        client = std::move(connecting).value();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            if (!server) {
                auto accepted = listener->accept(now);
                REQUIRE(accepted);
                if (accepted.value()) {
                    server = std::move(*accepted.value());
                }
            }
            REQUIRE(client->poll(now));
            if (server) {
                REQUIRE(server->poll(now));
                if (client->stats().value().state == TransportState::ready &&
                    server->stats().value().state == TransportState::ready) {
                    return;
                }
            }
            std::this_thread::yield();
        }
    }

    ReplicationUpdate update(MonotonicTime now) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            REQUIRE(server->poll(now));
            REQUIRE(client->poll(now));
            auto frame = client->receive();
            REQUIRE(frame);
            if (frame.value()) {
                REQUIRE(frame.value()->kind == MessageKind::replication_state);
                auto decoded = decode_replication_update(frame.value()->payload);
                REQUIRE(decoded);
                return std::move(decoded).value();
            }
            std::this_thread::yield();
        }
    }
};

SessionEnvelope envelope(const SessionSnapshot& session, std::uint64_t sequence) {
    return {session.session, session.ownership_epoch, sequence, session.character};
}

struct Harness {
    Pair pair;
    std::unique_ptr<SessionAuthority> sessions;
    std::optional<SessionSnapshot> session;
    std::unique_ptr<Scene> source;
    std::unique_ptr<InterestServer> server;
    std::vector<InterestEntry> catalog;

    explicit Harness(InterestLimits limits = {}) {
        limits.replication.streams = 1;
        limits.replication.resync_window = 100ms;
        SessionLimits session_limits;
        session_limits.auth_window = 1s;
        auto authority = SessionAuthority::create(session_limits);
        REQUIRE(authority);
        sessions = std::move(authority).value();
        REQUIRE(sessions->provision_account(id<AccountId>(1)));
        REQUIRE(sessions->assign_character(id<AccountId>(1), id<CharacterId>(1)));
        auto fingerprint = certificate_fingerprint(fixture("session/client-a.pem"));
        REQUIRE(fingerprint);
        REQUIRE(sessions->enroll_certificate(id<AccountId>(1), fingerprint.value()));
        login(0ns);
        auto created = Scene::create(
            RegionSceneScope{id<WorldId>(1), {id<RegionId>(2), id<RegionInstanceId>(3), 1}});
        REQUIRE(created);
        source = std::move(created).value();
        auto publisher = InterestServer::create(limits);
        REQUIRE(publisher);
        server = std::move(publisher).value();
        catalog.push_back(
            {entity(1), InterestVisibility::hidden, session->character, 0, false, true});
    }

    void login(MonotonicTime now) {
        auto logged_in =
            sessions->apply(*pair.server, LoginRequest{id<AccountId>(1), id<CharacterId>(1)}, now);
        REQUIRE(logged_in);
        auto ready = sessions->apply(*pair.server,
                                     SetReadyRequest{envelope(logged_in.value(), 1), true}, now);
        REQUIRE(ready);
        session = ready.value();
    }

    EntityHandle entity(std::uint32_t number) {
        Transform transform;
        transform.translation.x = static_cast<double>(number % 32);
        auto created =
            source->create_entity({id<WorldEntityId>(number), {}, Name{"PRIVATE"}, transform, {}});
        REQUIRE(created);
        return created.value();
    }

    void replace_population(std::uint32_t first, std::uint32_t count = 32) {
        for (std::size_t index = 1; index < catalog.size(); ++index) {
            REQUIRE(source->destroy_entity(catalog[index].entity));
        }
        catalog.erase(catalog.begin() + 1, catalog.end());
        for (std::uint32_t offset = 0; offset < count; ++offset) {
            catalog.push_back({entity(first + offset), InterestVisibility::public_pose});
        }
    }

    InterestPublishResult publish(MonotonicTime now) {
        auto result = server->publish(*source, catalog, *sessions, *pair.server, now);
        REQUIRE(result);
        return result.value();
    }

    ReplicationControlResult control(const ReplicationControl& request, MonotonicTime now) {
        auto result = server->control(*source, catalog, *sessions, *pair.server, request, now);
        REQUIRE(result);
        return result.value();
    }

    std::unique_ptr<ReplicaClient> cache(std::size_t maximum = 32) {
        auto result = ReplicaClient::create(*session, maximum);
        REQUIRE(result);
        return std::move(result).value();
    }
};

void same(const ReplicaSnapshot& a, const ReplicaSnapshot& b) {
    CHECK(a.stream == b.stream);
    CHECK(a.revision == b.revision);
    CHECK(a.has_baseline == b.has_baseline);
    CHECK(a.entities == b.entities);
}

void same(const ReplicaClientStats& a, const ReplicaClientStats& b) {
    CHECK(a.entities == b.entities);
    CHECK(a.known_live_ids == b.known_live_ids);
    CHECK(a.latest_change_records == b.latest_change_records);
    CHECK(a.entity_capacity_bytes == b.entity_capacity_bytes);
    CHECK(a.known_live_capacity_bytes == b.known_live_capacity_bytes);
    CHECK(a.latest_change_capacity_bytes == b.latest_change_capacity_bytes);
}

void bounded(const InterestStats& stats, const InterestStats& initial) {
    CHECK(stats.streams <= 1);
    CHECK(stats.selected_entities <= 32);
    CHECK(stats.last_sent_entities <= 32);
    CHECK(stats.stream_capacity_bytes == initial.stream_capacity_bytes);
    CHECK(stats.selected_capacity_bytes <= 32 * sizeof(EntityHandle));
    CHECK(stats.last_sent_capacity_bytes <= 32 * sizeof(EntityHandle));
    CHECK(stats.selected_capacity_bytes >= stats.selected_entities * sizeof(EntityHandle));
    CHECK(stats.last_sent_capacity_bytes >= stats.last_sent_entities * sizeof(EntityHandle));
    const auto& replication = stats.replication;
    CHECK(replication.streams <= 1);
    CHECK(replication.mapped_entities <= 32);
    CHECK(replication.acknowledged_entities <= 32);
    CHECK(replication.pending_updates <= 1);
    CHECK(replication.pending_target_entities <= 32);
    CHECK(replication.pending_change_records <= 64);
    CHECK(replication.stream_capacity_bytes == initial.replication.stream_capacity_bytes);
    CHECK(replication.mapped_capacity_bytes <= initial.replication.mapped_capacity_bytes);
    CHECK(replication.acknowledged_capacity_bytes <= 32 * sizeof(ReplicaEntity));
    CHECK(replication.pending_target_capacity_bytes <= 32 * sizeof(ReplicaEntity));
    CHECK(replication.pending_change_capacity_bytes <= 64 * sizeof(ReplicationRecord));
    CHECK(replication.mapped_capacity_bytes >=
          replication.mapped_entities * (sizeof(EntityHandle) + sizeof(ReplicaId)));
    CHECK(replication.acknowledged_capacity_bytes >=
          replication.acknowledged_entities * sizeof(ReplicaEntity));
    CHECK(replication.pending_target_capacity_bytes >=
          replication.pending_target_entities * sizeof(ReplicaEntity));
    CHECK(replication.pending_change_capacity_bytes >=
          replication.pending_change_records * sizeof(ReplicationRecord));
}

void bounded(const ReplicaClientStats& stats) {
    CHECK(stats.entities <= 32);
    CHECK(stats.known_live_ids <= 32);
    CHECK(stats.latest_change_records <= 64);
    CHECK(stats.entity_capacity_bytes <= 32 * sizeof(ReplicaEntity));
    CHECK(stats.known_live_capacity_bytes <= 32 * sizeof(ReplicaId));
    CHECK(stats.latest_change_capacity_bytes <= 64 * sizeof(ReplicationRecord));
    CHECK(stats.entity_capacity_bytes >= stats.entities * sizeof(ReplicaEntity));
    CHECK(stats.known_live_capacity_bytes >= stats.known_live_ids * sizeof(ReplicaId));
    CHECK(stats.latest_change_capacity_bytes >=
          stats.latest_change_records * sizeof(ReplicationRecord));
}

void released(const InterestStats& stats, const InterestStats& initial) {
    bounded(stats, initial);
    CHECK(stats.streams == 0);
    CHECK(stats.selected_entities == 0);
    CHECK(stats.last_sent_entities == 0);
    CHECK(stats.selected_capacity_bytes == 0);
    CHECK(stats.last_sent_capacity_bytes == 0);
    CHECK(stats.replication.streams == 0);
    CHECK(stats.replication.mapped_entities == 0);
    CHECK(stats.replication.acknowledged_entities == 0);
    CHECK(stats.replication.pending_updates == 0);
    CHECK(stats.replication.pending_target_entities == 0);
    CHECK(stats.replication.pending_change_records == 0);
    CHECK(stats.replication.mapped_capacity_bytes == 0);
    CHECK(stats.replication.acknowledged_capacity_bytes == 0);
    CHECK(stats.replication.pending_target_capacity_bytes == 0);
    CHECK(stats.replication.pending_change_capacity_bytes == 0);
    CHECK(stats.stream_capacity_bytes > 0);
    CHECK(stats.replication.stream_capacity_bytes > 0);
}

ReplicationUpdate decoded(const ReplicationUpdate& value) {
    auto encoded = encode_replication_update(value);
    REQUIRE(encoded);
    auto result = decode_replication_update(encoded.value());
    REQUIRE(result);
    return std::move(result).value();
}
} // namespace

TEST_CASE("Integrated churn and resync retain one bounded owned history", "[net-adversity]") {
    Harness h;
    h.replace_population(100);
    auto cache = h.cache();
    REQUIRE(h.publish(0ns).status == InterestPublishStatus::sent);
    auto update = h.pair.update(0ns);
    REQUIRE(cache->apply(update));
    const auto initial = h.server->stats().value();
    CHECK(initial.replication.mapped_entities == 32);
    CHECK(initial.replication.pending_target_entities == 32);
    CHECK(initial.replication.pending_change_records == 32);
    std::uint64_t sequence = 2;
    REQUIRE(
        h.control(ReplicationAck{envelope(*h.session, sequence++), update.stream, update.revision},
                  0ns) == ReplicationControlResult::acknowledged);
    MonotonicTime now{0};
    std::uint64_t highest_id = update.changes.back().id.value;
    for (std::uint32_t round = 1; round <= 32; ++round) {
        CAPTURE(round);
        now += 100ms;
        h.replace_population(100 + round * 32);
        REQUIRE(h.publish(now).status == InterestPublishStatus::sent);
        const auto delta = h.pair.update(now);
        REQUIRE_FALSE(delta.baseline);
        REQUIRE(delta.changes.size() == 64);
        CHECK(delta.changes.back().id.value > highest_id);
        highest_id = delta.changes.back().id.value;
        // Alternate semantic loss of state and of its ACK after authenticated
        // transport delivery. Neither behavior is called TCP packet loss.
        if (round % 2 == 0) {
            REQUIRE(cache->apply(delta));
        }
        auto retained = h.server->stats().value();
        bounded(retained, initial);
        CHECK(retained.replication.acknowledged_entities == 32);
        CHECK(retained.replication.pending_change_records == 64);
        REQUIRE(cache->discard_baseline());
        const auto discarded = cache->stats().value();
        bounded(discarded);
        CHECK(discarded.entities == 0);
        CHECK(discarded.known_live_ids == 32);
        CHECK(discarded.entity_capacity_bytes == 32 * sizeof(ReplicaEntity));
        REQUIRE(h.control(ReplicationResync{envelope(*h.session, sequence++), delta.stream,
                                            cache->snapshot().value().revision},
                          now) == ReplicationControlResult::resync_requested);
        retained = h.server->stats().value();
        CHECK(retained.replication.pending_updates == 0);
        CHECK(retained.replication.pending_target_capacity_bytes == 0);
        CHECK(retained.replication.pending_change_capacity_bytes == 0);
        now += 100ms;
        REQUIRE(h.publish(now).status == InterestPublishStatus::sent);
        update = h.pair.update(now);
        REQUIRE(update.baseline);
        CHECK(update.revision > delta.revision);
        CHECK(update.changes.back().id.value == highest_id);
        REQUIRE(cache->apply(update).value() == ReplicaApplyResult::applied);
        REQUIRE_FALSE(cache->apply(delta));
        REQUIRE(h.control(ReplicationAck{envelope(*h.session, sequence++), update.stream,
                                         update.revision},
                          now) == ReplicationControlResult::acknowledged);
        bounded(h.server->stats().value(), initial);
        bounded(cache->stats().value());
        CHECK(cache->stats().value().entities == 32);
        CHECK(cache->stats().value().latest_change_records == 32);
        CHECK(h.pair.server->stats().value().send_queue_frames == 0);
        CHECK(h.source->stats().value().live_entities == 33);
    }
    REQUIRE(h.pair.server->close());
    REQUIRE(h.sessions->revoke_session(h.session->session).value());
    REQUIRE(h.server->forget(h.session->session).value());
    released(h.server->stats().value(), initial);
    REQUIRE_FALSE(h.server->forget(h.session->session).value());
}

TEST_CASE("Repeated authenticated reconnect releases history and fences stale cleanup",
          "[net-adversity]") {
    Harness h;
    h.replace_population(100, 1);
    REQUIRE(h.publish(0ns).status == InterestPublishStatus::sent);
    auto previous = h.pair.update(0ns);
    const auto initial = h.server->stats().value();
    MonotonicTime now{0};
    for (unsigned round = 0; round < 32; ++round) {
        CAPTURE(round);
        now += 1s;
        auto previous_pair = std::move(h.pair);
        const auto previous_session = *h.session;
        h.pair = Pair{now};
        h.login(now);
        CHECK(h.session->session != previous_session.session);
        CHECK(h.session->ownership_epoch == previous_session.ownership_epoch + 1);
        REQUIRE(h.publish(now).status == InterestPublishStatus::sent);
        const auto update = h.pair.update(now);
        CHECK(update.baseline);
        CHECK(update.revision == 1);
        CHECK(update.stream != previous.stream);
        REQUIRE_FALSE(h.server->forget(previous_session.session).value());
        REQUIRE_FALSE(h.sessions->revoke_session(previous_session.session).value());
        REQUIRE_FALSE(h.server->control(
            *h.source, h.catalog, *h.sessions, *h.pair.server,
            ReplicationAck{envelope(previous_session, 2), previous.stream, previous.revision},
            now));
        REQUIRE_FALSE(h.sessions->current(*previous_pair.server, now));
        CHECK(h.sessions->current(*h.pair.server, now).value().last_sequence == 1);
        auto cache = h.cache();
        REQUIRE_FALSE(cache->apply(previous));
        REQUIRE(cache->apply(update));
        bounded(h.server->stats().value(), initial);
        CHECK(h.server->stats().value().streams == 1);
        CHECK(h.server->stats().value().replication.pending_updates == 1);
        CHECK(h.sessions->stats().value().live_sessions == 1);
        previous = update;
    }
    REQUIRE(h.pair.server->close());
    REQUIRE(h.sessions->revoke_session(h.session->session).value());
    REQUIRE(h.server->forget(h.session->session).value());
    released(h.server->stats().value(), initial);
    CHECK(h.sessions->stats().value().live_sessions == 0);
}

TEST_CASE("Malformed and adversarial decoded state preserves client values and owned storage",
          "[net-adversity]") {
    Harness h;
    h.replace_population(100, 2);
    auto cache = h.cache(2);
    REQUIRE(h.publish(0ns).status == InterestPublishStatus::sent);
    const auto baseline = h.pair.update(0ns);
    REQUIRE(cache->apply(baseline));
    const auto before = cache->snapshot().value();
    const auto storage = cache->stats().value();
    const auto wire = encode_replication_update(baseline).value();
    std::vector<std::vector<std::byte>> malformed;
    malformed.push_back(wire);
    malformed.back()[1] = std::byte{2}; // unsupported version
    malformed.push_back(wire);
    malformed.back()[3] = std::byte{1}; // nonzero reserved field
    malformed.push_back(wire);
    malformed.back()[61] = std::byte{65}; // excessive record count
    malformed.push_back(wire);
    malformed.back().pop_back();
    malformed.push_back(wire);
    malformed.back().push_back(std::byte{0});
    malformed.emplace_back(3743, std::byte{0});
    for (const auto& bytes : malformed) {
        REQUIRE_FALSE(decode_replication_update(bytes));
        same(before, cache->snapshot().value());
        same(storage, cache->stats().value());
    }
    auto next = baseline;
    next.baseline = false;
    next.base_revision = baseline.revision;
    next.revision = baseline.revision + 1;
    next.changes = {{ReplicationChange::update, baseline.changes[0].id, ReplicatedPose{}},
                    {ReplicationChange::update, ReplicaId{999}, ReplicatedPose{}}};
    next.changes[0].pose->coefficients[3] = 90;
    std::vector<ReplicationUpdate> invalid{next};
    auto forged = next;
    forged.session = id<SessionId>(999);
    invalid.push_back(forged);
    forged = next;
    ++forged.ownership_epoch;
    invalid.push_back(forged);
    forged = next;
    forged.stream = id<ReplicaSessionId>(999);
    invalid.push_back(forged);
    forged = next;
    forged.changes[1].kind = ReplicationChange::spawn; // cache would exceed its two-entity cap
    invalid.push_back(forged);
    forged = next;
    forged.changes[1] = {ReplicationChange::despawn, ReplicaId{999}, {}};
    invalid.push_back(forged);
    for (const auto& value : invalid) {
        REQUIRE_FALSE(cache->apply(decoded(value)));
        same(before, cache->snapshot().value());
        same(storage, cache->stats().value());
    }
    next.changes[1].id = baseline.changes[1].id;
    REQUIRE(cache->apply(decoded(next)).value() == ReplicaApplyResult::applied);
    CHECK(cache->snapshot().value().entities[0].pose.coefficients[3] == 90);
}

TEST_CASE("Sequence epoch and replica exhaustion reject wrapping and preserve fences",
          "[net-adversity]") {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    CHECK(next_sequence(maximum - 1).value() == maximum);
    CHECK(check_sequence(maximum - 1, maximum).value() == maximum);
    REQUIRE_FALSE(next_sequence(maximum));
    REQUIRE_FALSE(check_sequence(maximum, 0));
    REQUIRE_FALSE(check_sequence(maximum, 1));
    REQUIRE_FALSE(check_sequence(maximum, maximum));
    CHECK(next_ownership_epoch(maximum - 1).value() == maximum);
    REQUIRE_FALSE(next_ownership_epoch(maximum));

    Harness h;
    // No shipping setter jumps an authoritative session to UINT64_MAX. Test
    // its real admission fence, plus the same checked helper at exhaustion.
    for (const auto sequence : {maximum, maximum - 1, std::uint64_t{0}}) {
        REQUIRE_FALSE(h.sessions->apply(*h.pair.server,
                                        HeartbeatRequest{envelope(*h.session, sequence)}, 0ns));
        CHECK(h.sessions->current(*h.pair.server, 0ns).value().last_sequence == 1);
    }
    REQUIRE(h.sessions->apply(*h.pair.server, HeartbeatRequest{envelope(*h.session, 2)}, 0ns));
    auto cache = h.cache();
    ReplicationUpdate baseline{
        h.session->session,
        h.session->ownership_epoch,
        id<ReplicaSessionId>(10),
        maximum - 1,
        0,
        true,
        {{ReplicationChange::spawn, ReplicaId{maximum - 1}, ReplicatedPose{}}}};
    REQUIRE(cache->apply(decoded(baseline)));
    auto last = baseline;
    last.revision = maximum;
    last.base_revision = maximum - 1;
    last.baseline = false;
    last.changes = {{ReplicationChange::spawn, ReplicaId{maximum}, ReplicatedPose{}}};
    REQUIRE(cache->apply(decoded(last)));
    const auto before = cache->snapshot().value();
    const auto storage = cache->stats().value();
    CHECK(cache->apply(decoded(last)).value() == ReplicaApplyResult::duplicate);
    REQUIRE_FALSE(cache->apply(decoded(baseline)));
    auto wrapped = baseline;
    wrapped.revision = 1;
    wrapped.changes.front().id = ReplicaId{1};
    REQUIRE_FALSE(cache->apply(decoded(wrapped)));
    wrapped.revision = 0;
    REQUIRE_FALSE(cache->apply(wrapped));
    same(before, cache->snapshot().value());
    same(storage, cache->stats().value());
    REQUIRE(cache->discard_baseline());
    CHECK(cache->apply(decoded(last)).value() == ReplicaApplyResult::resync_required);
    REQUIRE_FALSE(cache->apply(decoded(baseline)));
    CHECK(cache->stats().value().entities == 0);
    CHECK(cache->stats().value().known_live_ids == 2);

    auto identity_cache = h.cache();
    baseline.revision = 1;
    baseline.changes[0].id = ReplicaId{maximum};
    REQUIRE(identity_cache->apply(decoded(baseline)));
    auto removal = baseline;
    removal.revision = 2;
    removal.base_revision = 1;
    removal.baseline = false;
    removal.changes = {{ReplicationChange::despawn, ReplicaId{maximum}, {}}};
    REQUIRE(identity_cache->apply(decoded(removal)));
    baseline.revision = 3;
    REQUIRE_FALSE(identity_cache->apply(decoded(baseline)));
    baseline.changes[0].id = ReplicaId{1};
    REQUIRE_FALSE(identity_cache->apply(decoded(baseline)));
    CHECK(identity_cache->stats().value().entities == 0);
}

TEST_CASE("Owned storage observations enforce the same service owner", "[net-adversity]") {
    Harness h;
    h.replace_population(100, 1);
    auto cache = h.cache();
    auto replication = ReplicationServer::create();
    REQUIRE(replication);
    REQUIRE(h.publish(0ns).status == InterestPublishStatus::sent);
    REQUIRE(cache->apply(h.pair.update(0ns)));
    const auto before = cache->stats().value();
    std::array<std::optional<Error>, 3> errors;
    std::thread foreign{[&] {
        auto interest = h.server->stats();
        if (!interest) {
            errors[0] = interest.error();
        }
        auto replicas = replication.value()->stats();
        if (!replicas) {
            errors[1] = replicas.error();
        }
        auto client = cache->stats();
        if (!client) {
            errors[2] = client.error();
        }
    }};
    foreign.join();
    for (const auto& error : errors) {
        REQUIRE(error);
    }
    CHECK(errors[0]->context.operation == "net.interest.owner");
    CHECK(errors[1]->context.operation == "net.replication.owner");
    CHECK(errors[2]->context.operation == "net.replica.owner");
    same(before, cache->stats().value());
    CHECK(h.server->stats().value().replication.pending_updates == 1);
}

TEST_CASE("Repeated application ACK timeout preserves one pending update until explicit recovery",
          "[net-adversity]") {
    InterestLimits limits;
    limits.replication.acknowledgement_timeout = 100ms;
    Harness h{limits};
    h.replace_population(100);
    auto cache = h.cache();
    MonotonicTime now{0};
    REQUIRE(h.publish(now).status == InterestPublishStatus::sent);
    auto update = h.pair.update(now);
    const auto initial = h.server->stats().value();
    std::uint64_t sequence = 2;
    for (unsigned round = 0; round < 32; ++round) {
        CAPTURE(round);
        REQUIRE(cache->apply(update));
        now += 100ms;
        auto timed_out = h.server->publish(*h.source, h.catalog, *h.sessions, *h.pair.server, now);
        REQUIRE_FALSE(timed_out);
        CHECK(timed_out.error().context.operation == "net.replication.ack-timeout");
        const auto retained = h.server->stats().value();
        bounded(retained, initial);
        CHECK(retained.replication.pending_updates == 1);
        CHECK(retained.replication.pending_change_records == 32);
        CHECK(retained.publications == static_cast<std::uint64_t>(round) + 1);
        REQUIRE_FALSE(h.server->control(
            *h.source, h.catalog, *h.sessions, *h.pair.server,
            ReplicationAck{envelope(*h.session, sequence), update.stream, update.revision}, now));
        CHECK(h.sessions->current(*h.pair.server, now).value().last_sequence == sequence - 1);
        // Repair/revalidate explicitly before any further output polling.
        REQUIRE(h.control(ReplicationResync{envelope(*h.session, sequence++), update.stream,
                                            update.revision},
                          now) == ReplicationControlResult::resync_requested);
        CHECK(h.server->stats().value().replication.pending_updates == 0);
        REQUIRE(h.publish(now).status == InterestPublishStatus::sent);
        const auto replacement = h.pair.update(now);
        CHECK(replacement.baseline);
        CHECK(replacement.revision == update.revision + 1);
        update = replacement;
    }
    REQUIRE(h.pair.server->close());
    REQUIRE(h.sessions->revoke_session(h.session->session).value());
    REQUIRE(h.server->forget(h.session->session).value());
    released(h.server->stats().value(), initial);
}
