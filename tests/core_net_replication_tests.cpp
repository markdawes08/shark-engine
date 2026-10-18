#include <shark/net/replication.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
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
    std::ifstream stream{std::filesystem::path{SHARK_NETWORK_FIXTURE_DIRECTORY} / name,
                         std::ios::binary};
    REQUIRE(stream);
    return {std::istreambuf_iterator<char>{stream}, {}};
}

struct Pair {
    std::unique_ptr<TlsListener> listener;
    std::unique_ptr<TlsTransport> client;
    std::unique_ptr<TlsTransport> server;

    explicit Pair(const std::string& identity = "client-a", bool authenticated = true) {
        const ServerIdentity server_identity{fixture("server.pem"), fixture("server-key.pem")};
        auto listening =
            authenticated
                ? TlsListener::listen_loopback_authenticated(0, server_identity, fixture("ca.pem"))
                : TlsListener::listen_loopback(0, server_identity);
        REQUIRE(listening);
        listener = std::move(listening).value();
        const ClientTrust trust{fixture("ca.pem"), "shark.test"};
        auto connecting =
            authenticated ? TlsTransport::connect_loopback_authenticated(
                                listener->port().value(), trust,
                                {fixture("session/" + identity + ".pem"),
                                 fixture("session/" + identity + "-key.pem")},
                                0ns)
                          : TlsTransport::connect_loopback(listener->port().value(), trust, 0ns);
        REQUIRE(connecting);
        client = std::move(connecting).value();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            if (!server) {
                auto accepted = listener->accept(0ns);
                REQUIRE(accepted);
                if (accepted.value()) {
                    server = std::move(*accepted.value());
                }
            }
            REQUIRE(client->poll(0ns));
            if (server) {
                REQUIRE(server->poll(0ns));
                if (client->stats().value().state == TransportState::ready &&
                    server->stats().value().state == TransportState::ready) {
                    break;
                }
            }
            std::this_thread::yield();
        }
    }
};

std::unique_ptr<SessionAuthority> authority(const SessionLimits& limits = {}) {
    auto created = SessionAuthority::create(limits);
    REQUIRE(created);
    auto service = std::move(created).value();
    for (const auto number : {1U, 2U}) {
        REQUIRE(service->provision_account(id<AccountId>(number)));
        REQUIRE(service->assign_character(id<AccountId>(number), id<CharacterId>(number)));
        auto fingerprint = certificate_fingerprint(
            fixture(number == 1 ? "session/client-a.pem" : "session/client-b.pem"));
        REQUIRE(fingerprint);
        REQUIRE(service->enroll_certificate(id<AccountId>(number), fingerprint.value()));
    }
    return service;
}

SessionEnvelope envelope(const SessionSnapshot& session, std::uint64_t sequence) {
    return {session.session, session.ownership_epoch, sequence, session.character};
}

SessionSnapshot ready(SessionAuthority& service, Pair& pair, std::uint32_t number = 1,
                      MonotonicTime now = 0ns) {
    auto login = service.apply(*pair.server,
                               LoginRequest{id<AccountId>(number), id<CharacterId>(number)}, now);
    REQUIRE(login);
    auto result =
        service.apply(*pair.server, SetReadyRequest{envelope(login.value(), 1), true}, now);
    REQUIRE(result);
    return result.value();
}

std::unique_ptr<Scene> scene() {
    auto created = Scene::create(
        RegionSceneScope{id<WorldId>(1), {id<RegionId>(2), id<RegionInstanceId>(3), 1}});
    REQUIRE(created);
    return std::move(created).value();
}

std::unique_ptr<ReplicationServer> publisher(const ReplicationLimits& limits = {}) {
    auto created = ReplicationServer::create(limits);
    REQUIRE(created);
    return std::move(created).value();
}

struct Harness {
    std::unique_ptr<SessionAuthority> sessions;
    Pair pair;
    SessionSnapshot session;
    std::unique_ptr<Scene> source;
    std::unique_ptr<ReplicationServer> server;

    explicit Harness(const ReplicationLimits& limits = {}, const SessionLimits& session_limits = {})
        : sessions(authority(session_limits)), session(ready(*sessions, pair)), source(scene()),
          server(publisher(limits)) {}

    EntityHandle entity(std::uint32_t number, Transform transform = {},
                        std::optional<EntityHandle> parent = {}) {
        auto created =
            source->create_entity({id<WorldEntityId>(number),
                                   {},
                                   Name{"PRIVATE_AUTHORING_NAME_" + std::to_string(number)},
                                   transform,
                                   parent});
        REQUIRE(created);
        return created.value();
    }
    ReplicationUpdate prepare(std::span<const EntityHandle> selected, MonotonicTime now = 0ns) {
        auto result = server->prepare(*source, selected, *sessions, *pair.server, now);
        REQUIRE(result);
        REQUIRE(result.value());
        return *result.value();
    }
    Result<ReplicationControlResult> ack(const ReplicationUpdate& update, std::uint64_t sequence,
                                         MonotonicTime now = 0ns) {
        return server->control(
            *sessions, *pair.server,
            ReplicationAck{envelope(session, sequence), update.stream, update.revision}, now);
    }
    std::unique_ptr<ReplicaClient> client(std::size_t maximum = 32) {
        auto created = ReplicaClient::create(session, maximum);
        REQUIRE(created);
        return std::move(created).value();
    }
};

template <typename T> void rejected(const Result<T>& result, const char* operation = nullptr) {
    REQUIRE_FALSE(result);
    CHECK_FALSE(result.error().context.operation.empty());
    CHECK(result.error().context.entity_id.empty());
    CHECK(result.error().context.path.empty());
    CHECK(result.error().context.request_id.empty());
    if (operation) {
        CHECK(result.error().context.operation == operation);
    }
}

void unchanged(const ReplicaSnapshot& before, const ReplicaSnapshot& after) {
    CHECK(before.stream == after.stream);
    CHECK(before.revision == after.revision);
    CHECK(before.has_baseline == after.has_baseline);
    CHECK(before.entities == after.entities);
}
} // namespace

TEST_CASE("Replication limits reject unbounded state and time settings", "[net-replication]") {
    ReplicationLimits limits;
    for (const auto count :
         {std::size_t{0}, std::size_t{33}, std::numeric_limits<std::size_t>::max()}) {
        limits.entities_per_stream = count;
        rejected(ReplicationServer::create(limits));
    }
    limits = {};
    limits.streams = 0;
    rejected(ReplicationServer::create(limits));
    limits.streams = 65;
    rejected(ReplicationServer::create(limits));
    for (const auto duration : {-1ns, 0ns, MonotonicTime{61s}}) {
        limits = {};
        limits.acknowledgement_timeout = duration;
        rejected(ReplicationServer::create(limits));
    }
    limits = {};
    limits.controls_per_window = 65537;
    rejected(ReplicationServer::create(limits));
    limits.controls_per_window = 0;
    rejected(ReplicationServer::create(limits));
    limits = {};
    limits.control_window = 2h;
    rejected(ReplicationServer::create(limits));
    limits = {};
    limits.resyncs_per_window = 65;
    rejected(ReplicationServer::create(limits));
    limits.resyncs_per_window = 0;
    rejected(ReplicationServer::create(limits));
    limits = {};
    limits.resync_window = 0ns;
    rejected(ReplicationServer::create(limits));
    limits.resync_window = 2h;
    rejected(ReplicationServer::create(limits));
}

TEST_CASE("Explicit Region selection flattens world shear without authoring fields",
          "[net-replication]") {
    Harness h;
    Transform parent;
    parent.translation = {10, 20, 30};
    parent.scale = {2, 1, 1};
    const auto root = h.entity(10, parent);
    Transform child;
    child.translation = {3, 4, 5};
    child.rotation = {std::cos(std::acos(-1.0) / 8), 0, 0, std::sin(std::acos(-1.0) / 8)};
    const auto leaf = h.entity(11, child, root);
    const std::array selection{leaf};
    const auto baseline = h.prepare(selection);
    REQUIRE(baseline.baseline);
    REQUIRE(baseline.changes.size() == 1);
    CHECK(baseline.changes.front().id.value == 1);
    const auto& pose = *baseline.changes.front().pose;
    CHECK(pose.coefficients[0] == Catch::Approx(std::sqrt(2.0)));
    CHECK(pose.coefficients[1] == Catch::Approx(-std::sqrt(2.0)));
    CHECK(pose.coefficients[3] == 16);
    CHECK(pose.coefficients[7] == 24);
    CHECK(pose.coefficients[11] == 35);
    REQUIRE_FALSE(decompose_transform(h.source->entity_state(leaf).value().world_matrix));
    auto encoded = encode_replication_update(baseline);
    REQUIRE(encoded);
    const std::string bytes{reinterpret_cast<const char*>(encoded.value().data()),
                            encoded.value().size()};
    CHECK(bytes.find("PRIVATE_AUTHORING_NAME") == std::string::npos);
    for (const auto number : {10U, 11U}) {
        const auto private_id = id<WorldEntityId>(number).value().bytes();
        CHECK(std::search(encoded.value().begin(), encoded.value().end(), private_id.begin(),
                          private_id.end()) == encoded.value().end());
    }
    REQUIRE(h.ack(baseline, 2));
    REQUIRE(h.source->set_name(leaf, Name{"CHANGED_PRIVATE_NAME"}));
    auto no_change = h.server->prepare(*h.source, selection, *h.sessions, *h.pair.server, 0ns);
    REQUIRE(no_change);
    CHECK_FALSE(no_change.value());
    CHECK(h.sessions->current(*h.pair.server, 0ns).value().last_sequence == 2);
}

TEST_CASE("Replication rejects foreign stale duplicated and nonauthoritative selections",
          "[net-replication]") {
    ReplicationLimits limits;
    limits.entities_per_stream = 1;
    Harness h{limits};
    const auto entity = h.entity(10);
    const std::array duplicate{entity, entity};
    rejected(h.server->prepare(*h.source, duplicate, *h.sessions, *h.pair.server, 0ns));
    CHECK(h.server->stats().value().streams == 0);
    auto other = scene();
    const std::array foreign{entity};
    rejected(h.server->prepare(*other, foreign, *h.sessions, *h.pair.server, 0ns));
    auto authored = Scene::create(AuthoredSceneScope{id<SceneId>(10)});
    REQUIRE(authored);
    rejected(h.server->prepare(*authored.value(), {}, *h.sessions, *h.pair.server, 0ns));
    auto replica = Scene::create(ReplicaSceneScope{id<WorldId>(1), id<ReplicaSessionId>(2)});
    REQUIRE(replica);
    rejected(h.server->prepare(*replica.value(), {}, *h.sessions, *h.pair.server, 0ns));
    const auto baseline = h.prepare(foreign);
    rejected(h.server->prepare(*other, {}, *h.sessions, *h.pair.server, 0ns));
    REQUIRE(h.source->destroy_entity(entity));
    rejected(h.server->prepare(*h.source, foreign, *h.sessions, *h.pair.server, 0ns));
    CHECK(h.server->stats().value().pending_updates == 1);
    REQUIRE(h.ack(baseline, 2));
    const auto removed = h.prepare({});
    REQUIRE(removed.changes.size() == 1);
    CHECK(removed.changes.front().kind == ReplicationChange::despawn);
    REQUIRE(h.source->close());
    rejected(h.server->prepare(*h.source, {}, *h.sessions, *h.pair.server, 0ns));
}

TEST_CASE("Unchanged snapshots wait for application ACK and then produce bounded lifecycle deltas",
          "[net-replication]") {
    Harness h;
    auto client = h.client();
    const auto first = h.entity(10);
    const std::array initial{first};
    const auto baseline = h.prepare(initial);
    REQUIRE(client->apply(baseline).value() == ReplicaApplyResult::applied);
    Transform moved;
    moved.translation = {2, 3, 4};
    REQUIRE(h.source->set_transform(first, moved));
    const auto second = h.entity(11);
    const std::array expanded{second, first};
    CHECK(h.prepare(expanded) == baseline);
    CHECK(h.server->stats().value().pending_updates == 1);
    REQUIRE(h.ack(baseline, 2).value() == ReplicationControlResult::acknowledged);
    const auto delta = h.prepare(expanded);
    CHECK_FALSE(delta.baseline);
    CHECK(delta.base_revision == baseline.revision);
    REQUIRE(delta.changes.size() == 2);
    CHECK(delta.changes[0].kind == ReplicationChange::update);
    CHECK(delta.changes[0].id == baseline.changes[0].id);
    CHECK(delta.changes[1].kind == ReplicationChange::spawn);
    CHECK(delta.changes[1].id.value > delta.changes[0].id.value);
    REQUIRE(client->apply(delta).value() == ReplicaApplyResult::applied);
    REQUIRE(h.ack(delta, 3));
    REQUIRE(h.source->destroy_entity(first));
    const auto replacement = h.entity(12);
    CHECK(replacement.slot == first.slot);
    CHECK(replacement.generation != first.generation);
    const std::array final_selection{second, replacement};
    const auto final = h.prepare(final_selection);
    REQUIRE(final.changes.size() == 2);
    CHECK(final.changes[0].kind == ReplicationChange::despawn);
    CHECK(final.changes[1].kind == ReplicationChange::spawn);
    CHECK(final.changes[1].id.value > delta.changes[1].id.value);
    REQUIRE(client->apply(final));
    CHECK(client->snapshot().value().entities.size() == 2);
}

TEST_CASE("Maximum baseline and complete replacement fit one bounded outer frame",
          "[net-replication]") {
    Harness h;
    auto client = h.client();
    std::vector<EntityHandle> initial;
    std::vector<EntityHandle> replacement;
    for (std::uint32_t index = 0; index < 32; ++index) {
        initial.push_back(h.entity(100 + index));
        replacement.push_back(h.entity(200 + index));
    }
    const auto baseline = h.prepare(initial);
    REQUIRE(encode_replication_update(baseline).value().size() == 3454);
    REQUIRE(client->apply(baseline));
    REQUIRE(h.ack(baseline, 2));
    const auto delta = h.prepare(replacement);
    REQUIRE(delta.changes.size() == 64);
    CHECK(encode_replication_update(delta).value().size() == 3742);
    REQUIRE(client->apply(delta));
    CHECK(client->snapshot().value().entities.size() == 32);
    CHECK(h.server->stats().value().mapped_entities == 32);
    replacement.push_back(initial.front());
    rejected(h.server->prepare(*h.source, replacement, *h.sessions, *h.pair.server, 0ns));
}

TEST_CASE("Session readiness provenance expiry and revocation gate every preparation",
          "[net-replication]") {
    SessionLimits limits;
    limits.session_idle_timeout = 10ms;
    Harness h{{}, limits};
    const auto baseline = h.prepare({});
    Pair diagnostic{"client-a", false};
    rejected(h.server->prepare(*h.source, {}, *h.sessions, *diagnostic.server, 0ns));
    rejected(h.server->prepare(*h.source, {}, *h.sessions, *h.pair.client, 0ns));
    SECTION("Read-only preparation does not refresh the lease") {
        CHECK(h.prepare({}, 9ms) == baseline);
        CHECK(h.sessions->current(*h.pair.server, 9ms).value().last_sequence == 1);
        rejected(h.server->prepare(*h.source, {}, *h.sessions, *h.pair.server, 10ms));
    }
    SECTION("Readiness can be withdrawn without replacing the stream") {
        REQUIRE(
            h.sessions->apply(*h.pair.server, SetReadyRequest{envelope(h.session, 2), false}, 0ns));
        rejected(h.server->prepare(*h.source, {}, *h.sessions, *h.pair.server, 0ns));
        REQUIRE(
            h.sessions->apply(*h.pair.server, SetReadyRequest{envelope(h.session, 3), true}, 0ns));
        CHECK(h.prepare({}) == baseline);
        REQUIRE(h.ack(baseline, 4));
    }
    SECTION("Immediate account revocation") {
        REQUIRE(h.sessions->revoke_account(h.session.account));
        rejected(h.server->prepare(*h.source, {}, *h.sessions, *h.pair.server, 0ns));
        rejected(h.ack(baseline, 2));
    }
    SECTION("Closed transport") {
        REQUIRE(h.pair.server->close());
        rejected(h.server->prepare(*h.source, {}, *h.sessions, *h.pair.server, 0ns));
    }
}

TEST_CASE("ACK ownership exact shared sequence and duplicate semantics preserve pending state",
          "[net-replication]") {
    Harness h;
    Pair second{"client-b"};
    const auto second_session = ready(*h.sessions, second, 2);
    const auto first = h.entity(10);
    const std::array selected{first};
    const auto baseline = h.prepare(selected);
    auto b = h.server->prepare(*h.source, selected, *h.sessions, *second.server, 0ns);
    REQUIRE(b);
    REQUIRE(b.value());
    rejected(h.server->control(
        *h.sessions, *second.server,
        ReplicationAck{envelope(h.session, 2), baseline.stream, baseline.revision}, 0ns));
    auto malformed = ReplicationAck{envelope(h.session, 2), baseline.stream, baseline.revision + 1};
    rejected(h.server->control(*h.sessions, *h.pair.server, malformed, 0ns));
    malformed.revision = baseline.revision;
    malformed.authority.ownership_epoch += 1;
    rejected(h.server->control(*h.sessions, *h.pair.server, malformed, 0ns));
    malformed.authority = envelope(h.session, 2);
    malformed.authority.character = second_session.character;
    rejected(h.server->control(*h.sessions, *h.pair.server, malformed, 0ns));
    rejected(h.ack(baseline, 0));
    rejected(h.ack(baseline, 3));
    CHECK(h.sessions->current(*h.pair.server, 0ns).value().last_sequence == 1);
    REQUIRE(h.ack(baseline, 2).value() == ReplicationControlResult::acknowledged);
    rejected(h.ack(baseline, 2));
    Transform moved;
    moved.translation.x = 1;
    REQUIRE(h.source->set_transform(first, moved));
    const auto delta = h.prepare(selected);
    REQUIRE(h.ack(baseline, 3).value() == ReplicationControlResult::duplicate);
    CHECK(h.prepare(selected) == delta);
    REQUIRE(h.ack(delta, 4));
    rejected(h.ack(baseline, 5));
    CHECK(h.sessions->current(*h.pair.server, 0ns).value().last_sequence == 4);
    CHECK(h.server->stats().value().streams == 2);
}

TEST_CASE("Rejected controls spend bounded work but cannot consume sequence or extend leases",
          "[net-replication]") {
    ReplicationLimits limits;
    limits.controls_per_window = 2;
    Harness h{limits};
    const auto baseline = h.prepare({});
    const ReplicationAck future{envelope(h.session, 2), baseline.stream, baseline.revision + 1};
    rejected(h.server->control(*h.sessions, *h.pair.server, future, 0ns));
    rejected(h.server->control(*h.sessions, *h.pair.server, future, 0ns));
    rejected(h.ack(baseline, 2), "net.replication.rate.control");
    CHECK(h.sessions->current(*h.pair.server, 0ns).value().last_sequence == 1);
    REQUIRE(h.ack(baseline, 2, 1s));
    SessionLimits short_session;
    short_session.session_idle_timeout = 10ms;
    Harness short_h{{}, short_session};
    const auto pending = short_h.prepare({});
    rejected(short_h.server->control(
        *short_h.sessions, *short_h.pair.server,
        ReplicationAck{envelope(short_h.session, 2), pending.stream, 99}, 9ms));
    rejected(short_h.server->prepare(*short_h.source, {}, *short_h.sessions, *short_h.pair.server,
                                     10ms));
}

TEST_CASE("Missing client baseline requests a newer full snapshot and fences old ACKs",
          "[net-replication]") {
    Harness h;
    auto client = h.client();
    const auto entity = h.entity(10);
    const std::array selected{entity};
    const auto baseline = h.prepare(selected);
    REQUIRE(client->apply(baseline));
    REQUIRE(h.ack(baseline, 2));
    REQUIRE(client->discard_baseline());
    CHECK(client->apply(baseline).value() == ReplicaApplyResult::resync_required);
    Transform moved;
    moved.translation.y = 17;
    REQUIRE(h.source->set_transform(entity, moved));
    const auto delta = h.prepare(selected);
    auto fresh_client = h.client();
    const auto fresh_before = fresh_client->snapshot().value();
    CHECK_FALSE(fresh_before.stream);
    CHECK(fresh_before.revision == 0);
    CHECK(fresh_client->apply(delta).value() == ReplicaApplyResult::resync_required);
    unchanged(fresh_before, fresh_client->snapshot().value());
    CHECK(client->apply(delta).value() == ReplicaApplyResult::resync_required);
    const auto lost = client->snapshot().value();
    CHECK_FALSE(lost.has_baseline);
    CHECK(lost.entities.empty());
    CHECK(lost.revision == baseline.revision);
    REQUIRE(h.server
                ->control(*h.sessions, *h.pair.server,
                          ReplicationResync{envelope(h.session, 3), baseline.stream, lost.revision},
                          0ns)
                .value() == ReplicationControlResult::resync_requested);
    rejected(h.ack(delta, 4));
    const auto replacement = h.prepare(selected);
    CHECK(replacement.baseline);
    CHECK(replacement.revision > delta.revision);
    CHECK(replacement.changes[0].id == delta.changes[0].id);
    REQUIRE(client->apply(replacement));
    REQUIRE(fresh_client->apply(replacement).value() == ReplicaApplyResult::applied);
    unchanged(client->snapshot().value(), fresh_client->snapshot().value());
    CHECK(client->snapshot().value().entities.front().pose.coefficients[7] == 17);
    REQUIRE(h.ack(replacement, 4));
}

TEST_CASE("Lost ACK resync preserves newly issued IDs whether pending data was applied or lost",
          "[net-replication]") {
    Harness h;
    auto client = h.client();
    const auto first = h.entity(10);
    const std::array initial{first};
    const auto baseline = h.prepare(initial);
    REQUIRE(client->apply(baseline));
    REQUIRE(h.ack(baseline, 2));
    const auto second = h.entity(11);
    const std::array expanded{first, second};
    const auto pending = h.prepare(expanded);
    SECTION("Applied pending update with lost ACK") {
        REQUIRE(client->apply(pending));
    }
    SECTION("Pending update never reached the cache") {}
    const auto known = client->snapshot().value().revision;
    REQUIRE(h.server->control(*h.sessions, *h.pair.server,
                              ReplicationResync{envelope(h.session, 3), baseline.stream, known},
                              0ns));
    const auto replacement = h.prepare(expanded);
    REQUIRE(replacement.changes.size() == 2);
    CHECK(replacement.changes.back().id == pending.changes.back().id);
    REQUIRE(client->apply(replacement));
    CHECK(client->snapshot().value().entities.size() == 2);
}

TEST_CASE("ACK deadlines require explicit bounded resync without recycling revisions",
          "[net-replication]") {
    ReplicationLimits limits;
    limits.acknowledgement_timeout = 5ms;
    Harness h{limits};
    const auto original = h.prepare({});
    rejected(h.ack(original, 2, 5ms), "net.replication.ack-timeout");
    rejected(h.server->prepare(*h.source, {}, *h.sessions, *h.pair.server, 5ms),
             "net.replication.ack-timeout");
    REQUIRE(h.server->control(*h.sessions, *h.pair.server,
                              ReplicationResync{envelope(h.session, 2), original.stream, 0}, 5ms));
    const auto replacement = h.prepare({}, 5ms);
    CHECK(replacement.revision == original.revision + 1);
    rejected(h.ack(original, 3, 5ms));
    REQUIRE(h.ack(replacement, 3, 5ms));
}

TEST_CASE("Resync amplification has a separate fixed window including invalid known revisions",
          "[net-replication]") {
    Harness h;
    const auto original = h.prepare({});
    REQUIRE(h.ack(original, 2));
    for (std::uint64_t sequence = 3; sequence <= 6; ++sequence) {
        REQUIRE(h.server->control(
            *h.sessions, *h.pair.server,
            ReplicationResync{envelope(h.session, sequence), original.stream, 0}, 0ns));
        CHECK(h.prepare({}).baseline);
    }
    rejected(h.server->control(*h.sessions, *h.pair.server,
                               ReplicationResync{envelope(h.session, 7), original.stream, 0}, 0ns),
             "net.replication.rate.resync");
    CHECK(h.sessions->current(*h.pair.server, 0ns).value().last_sequence == 6);
    REQUIRE(h.server->control(*h.sessions, *h.pair.server,
                              ReplicationResync{envelope(h.session, 7), original.stream, 0}, 10s));
    CHECK(h.prepare({}, 10s).baseline);

    ReplicationLimits one;
    one.resyncs_per_window = 1;
    Harness other{one};
    const auto initial = other.prepare({});
    rejected(other.server->control(
        *other.sessions, *other.pair.server,
        ReplicationResync{envelope(other.session, 2), initial.stream, 999}, 0ns));
    rejected(other.server->control(*other.sessions, *other.pair.server,
                                   ReplicationResync{envelope(other.session, 2), initial.stream, 0},
                                   0ns),
             "net.replication.rate.resync");
    CHECK(other.sessions->current(*other.pair.server, 0ns).value().last_sequence == 1);
}

TEST_CASE(
    "Reconnect replaces one account stream only after a valid capture and fences stale teardown",
    "[net-replication]") {
    ReplicationLimits limits;
    limits.streams = 1;
    Harness h{limits};
    const auto entity = h.entity(10);
    const std::array selected{entity};
    const auto previous = h.prepare(selected);
    Pair other{"client-b"};
    static_cast<void>(ready(*h.sessions, other, 2));
    rejected(h.server->prepare(*h.source, selected, *h.sessions, *other.server, 0ns),
             "net.replication.capacity");
    Pair replacement;
    const auto session = ready(*h.sessions, replacement);
    const std::array duplicate{entity, entity};
    rejected(h.server->prepare(*h.source, duplicate, *h.sessions, *replacement.server, 0ns));
    CHECK(h.server->stats().value().streams == 1);
    auto renewed = h.server->prepare(*h.source, selected, *h.sessions, *replacement.server, 0ns);
    REQUIRE(renewed);
    REQUIRE(renewed.value());
    CHECK(renewed.value()->stream != previous.stream);
    CHECK(renewed.value()->session == session.session);
    CHECK(renewed.value()->baseline);
    CHECK(renewed.value()->revision == 1);
    CHECK_FALSE(h.server->forget(h.session.session).value());
    rejected(h.ack(previous, 2));
    rejected(h.server->control(
        *h.sessions, *replacement.server,
        ReplicationAck{envelope(h.session, 2), previous.stream, previous.revision}, 0ns));
    auto fresh_client = ReplicaClient::create(session);
    REQUIRE(fresh_client);
    rejected(fresh_client.value()->apply(previous));
    REQUIRE(fresh_client.value()->apply(*renewed.value()));
    REQUIRE(h.server->forget(session.session).value());
    CHECK(h.server->stats().value().streams == 0);
}

TEST_CASE("Replica updates are atomic and reject impossible operations and retired IDs",
          "[net-replication]") {
    Harness h;
    auto client = h.client(1);
    const auto entity = h.entity(10);
    const std::array selected{entity};
    const auto baseline = h.prepare(selected);
    REQUIRE(client->apply(baseline));
    const auto before = client->snapshot().value();
    auto invalid = baseline;
    invalid.revision = 2;
    invalid.base_revision = 1;
    invalid.baseline = false;
    invalid.changes = {{ReplicationChange::update, ReplicaId{1}, ReplicatedPose{}},
                       {ReplicationChange::update, ReplicaId{2}, ReplicatedPose{}}};
    invalid.changes[0].pose->coefficients[3] = 25;
    rejected(client->apply(invalid));
    unchanged(before, client->snapshot().value());
    invalid.changes = {{ReplicationChange::despawn, ReplicaId{2}, {}}};
    rejected(client->apply(invalid));
    invalid.changes = {{ReplicationChange::spawn, ReplicaId{1}, ReplicatedPose{}}};
    rejected(client->apply(invalid));
    invalid.changes = {{ReplicationChange::spawn, ReplicaId{2}, ReplicatedPose{}}};
    rejected(client->apply(invalid));
    invalid.changes = {{ReplicationChange::update, ReplicaId{1}, ReplicatedPose{}}};
    invalid.changes[0].pose->coefficients[0] = std::numeric_limits<double>::infinity();
    rejected(client->apply(invalid));
    unchanged(before, client->snapshot().value());
    auto removal = invalid;
    removal.changes = {{ReplicationChange::despawn, ReplicaId{1}, {}}};
    REQUIRE(client->apply(removal));
    auto resurrect = baseline;
    resurrect.revision = 3;
    rejected(client->apply(resurrect));
    REQUIRE(client->discard_baseline());
    rejected(client->apply(resurrect));
    CHECK(client->snapshot().value().entities.empty());
    resurrect.changes[0].id = ReplicaId{2};
    REQUIRE(client->apply(resurrect));
    CHECK(client->snapshot().value().entities.front().id.value == 2);
}

TEST_CASE("Replica revision ownership and missing-base outcomes cannot roll back accepted data",
          "[net-replication]") {
    Harness h;
    auto client = h.client();
    const auto baseline = h.prepare({});
    REQUIRE(client->apply(baseline));
    CHECK(client->apply(baseline).value() == ReplicaApplyResult::duplicate);
    auto conflicting = baseline;
    conflicting.changes.push_back({ReplicationChange::spawn, ReplicaId{1}, ReplicatedPose{}});
    rejected(client->apply(conflicting));
    auto next = conflicting;
    next.revision = 2;
    next.base_revision = 1;
    next.baseline = false;
    auto missing = next;
    missing.revision = 3;
    missing.base_revision = 2;
    const auto previous = client->snapshot().value();
    CHECK(client->apply(missing).value() == ReplicaApplyResult::resync_required);
    unchanged(previous, client->snapshot().value());
    next.stream = id<ReplicaSessionId>(100);
    rejected(client->apply(next));
    next.stream = baseline.stream;
    next.ownership_epoch += 1;
    rejected(client->apply(next));
    next.ownership_epoch = baseline.ownership_epoch;
    REQUIRE(client->apply(next));
    rejected(client->apply(baseline));
    auto maximum = next;
    maximum.revision = std::numeric_limits<std::uint64_t>::max();
    maximum.base_revision = 0;
    maximum.baseline = true;
    maximum.changes[0].id.value = std::numeric_limits<std::uint64_t>::max();
    REQUIRE(client->apply(maximum));
    rejected(client->apply(next));
    REQUIRE(client->discard_baseline());
    CHECK(client->apply(maximum).value() == ReplicaApplyResult::resync_required);
}

TEST_CASE("Replication owner clocks bounds and snapshots remain independent of caller mutation",
          "[net-replication]") {
    Harness h;
    auto client = h.client();
    auto baseline = h.prepare({}, 10ns);
    rejected(h.server->prepare(*h.source, {}, *h.sessions, *h.pair.server, 9ns),
             "net.replication.time");
    REQUIRE(client->apply(baseline));
    std::optional<Error> server_error;
    std::optional<Error> client_error;
    std::thread thread{[&] {
        auto server_result = h.server->stats();
        auto client_result = client->discard_baseline();
        if (!server_result) {
            server_error = server_result.error();
        }
        if (!client_result) {
            client_error = client_result.error();
        }
    }};
    thread.join();
    REQUIRE(server_error);
    REQUIRE(client_error);
    CHECK(server_error->context.operation == "net.replication.owner");
    CHECK(client_error->context.operation == "net.replica.owner");
    baseline.revision = 999;
    CHECK(h.prepare({}, 10ns).revision == 1);
    auto copy = client->snapshot().value();
    copy.entities.push_back({ReplicaId{10}, {}});
    CHECK(client->snapshot().value().entities.empty());
    rejected(ReplicaClient::create(h.session, 0));
    rejected(ReplicaClient::create(h.session, 33));
    auto not_ready = h.session;
    not_ready.ready = false;
    rejected(ReplicaClient::create(not_ready));
}

TEST_CASE("Replication deadline overflow rejects before installing a stream", "[net-replication]") {
    SessionLimits limits;
    limits.session_lifetime = 1ns;
    limits.session_idle_timeout = 1ns;
    auto sessions = authority(limits);
    Pair pair;
    const MonotonicTime now{std::numeric_limits<MonotonicTime::rep>::max() - 1};
    static_cast<void>(ready(*sessions, pair, 1, now));
    ReplicationLimits replication_limits;
    replication_limits.acknowledgement_timeout = 2ns;
    auto server = publisher(replication_limits);
    auto source = scene();
    rejected(server->prepare(*source, {}, *sessions, *pair.server, now), "net.replication.time");
    CHECK(server->stats().value().streams == 0);
}
