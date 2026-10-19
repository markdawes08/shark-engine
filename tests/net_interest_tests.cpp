#include <shark/net/interest.hpp>

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

    explicit Pair(const std::string& identity = "client-a", MonotonicTime now = 0ns) {
        auto listening = TlsListener::listen_loopback_authenticated(
            0, {fixture("server.pem"), fixture("server-key.pem")}, fixture("ca.pem"));
        REQUIRE(listening);
        listener = std::move(listening).value();
        auto connecting = TlsTransport::connect_loopback_authenticated(
            listener->port().value(), {fixture("ca.pem"), "shark.test"},
            {fixture("session/" + identity + ".pem"), fixture("session/" + identity + "-key.pem")},
            now);
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
                    break;
                }
            }
            std::this_thread::yield();
        }
    }

    Frame receive(MonotonicTime now = 0ns) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            REQUIRE(server->poll(now));
            REQUIRE(client->poll(now));
            auto value = client->receive();
            REQUIRE(value);
            if (value.value()) {
                return *value.value();
            }
            std::this_thread::yield();
        }
    }

    ReplicationUpdate update(MonotonicTime now = 0ns) {
        const auto frame = receive(now);
        REQUIRE(frame.kind == MessageKind::replication_state);
        auto decoded = decode_replication_update(frame.payload);
        REQUIRE(decoded);
        return decoded.value();
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

std::unique_ptr<InterestServer> publisher(const InterestLimits& limits) {
    auto created = InterestServer::create(limits);
    REQUIRE(created);
    return std::move(created).value();
}

struct Harness {
    std::unique_ptr<SessionAuthority> sessions;
    Pair pair;
    SessionSnapshot session;
    std::unique_ptr<Scene> source;
    std::unique_ptr<InterestServer> server;
    std::vector<InterestEntry> catalog;

    explicit Harness(const InterestLimits& limits = {}, const SessionLimits& session_limits = {},
                     MonotonicTime now = 0ns)
        : sessions(authority(session_limits)), pair("client-a", now),
          session(ready(*sessions, pair, 1, now)), source(scene()), server(publisher(limits)) {
        const auto observer = entity(1);
        catalog.push_back(
            {observer, InterestVisibility::hidden, session.character, 0, false, true});
    }

    EntityHandle entity(std::uint32_t number, math::Vec3 position = {}) {
        Transform transform;
        transform.translation = position;
        auto created = source->create_entity({id<WorldEntityId>(number),
                                              {},
                                              Name{"PRIVATE_" + std::to_string(number)},
                                              transform,
                                              {}});
        REQUIRE(created);
        return created.value();
    }
    EntityHandle public_entity(std::uint32_t number, math::Vec3 position = {}) {
        const auto handle = entity(number, position);
        catalog.push_back({handle, InterestVisibility::public_pose});
        return handle;
    }
    void move(EntityHandle handle, math::Vec3 position) {
        Transform transform;
        transform.translation = position;
        REQUIRE(source->set_transform(handle, transform));
    }
    InterestPublishResult publish(MonotonicTime now = 0ns) {
        auto result = server->publish(*source, catalog, *sessions, *pair.server, now);
        REQUIRE(result);
        return result.value();
    }
    void ack(const ReplicationUpdate& update, std::uint64_t sequence, MonotonicTime now = 0ns) {
        auto result = server->control(
            *source, catalog, *sessions, *pair.server,
            ReplicationAck{envelope(session, sequence), update.stream, update.revision}, now);
        REQUIRE(result);
        CHECK(result.value() == ReplicationControlResult::acknowledged);
    }
};

template <typename T> void rejected(const Result<T>& result) {
    REQUIRE_FALSE(result);
    CHECK_FALSE(result.error().context.operation.empty());
    CHECK(result.error().context.entity_id.empty());
    CHECK(result.error().context.request_id.empty());
    CHECK(result.error().context.path.empty());
}

std::vector<double> positions(const ReplicationUpdate& update) {
    std::vector<double> result;
    for (const auto& record : update.changes) {
        if (record.pose) {
            result.push_back(record.pose->coefficients[3]);
        }
    }
    std::ranges::sort(result);
    return result;
}
} // namespace

TEST_CASE("Interest limits bound selection traffic and time", "[net-interest]") {
    REQUIRE(InterestServer::create());
    for (const auto count :
         {std::size_t{0}, std::size_t{129}, std::numeric_limits<std::size_t>::max()}) {
        InterestLimits limits;
        limits.candidate_limit = count;
        rejected(InterestServer::create(limits));
    }
    for (const auto value : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
        InterestLimits limits;
        limits.enter_radius = value;
        rejected(InterestServer::create(limits));
    }
    for (const auto value : {99.0, 1000001.0, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
        InterestLimits limits;
        limits.exit_radius = value;
        rejected(InterestServer::create(limits));
    }
    for (const auto duration : {-1ns, 0ns, 999999ns, MonotonicTime{1001ms}}) {
        InterestLimits limits;
        limits.publish_period = duration;
        rejected(InterestServer::create(limits));
    }
    for (const auto count : {std::size_t{0}, std::size_t{1048577}}) {
        InterestLimits limits;
        limits.framed_bytes_per_second = count;
        rejected(InterestServer::create(limits));
    }
    for (const auto count : {std::size_t{0}, std::size_t{3761}, std::size_t{1048577}}) {
        InterestLimits limits;
        limits.framed_burst_bytes = count;
        rejected(InterestServer::create(limits));
    }
    for (const auto count : {std::size_t{0}, std::size_t{65537}}) {
        InterestLimits limits;
        limits.work_units = count;
        rejected(InterestServer::create(limits));
    }
    InterestLimits limits;
    limits.replication.entities_per_stream = 33;
    rejected(InterestServer::create(limits));
    limits = {};
    limits.replication.streams = 0;
    rejected(InterestServer::create(limits));
}

TEST_CASE("Interest derives private visibility and observer from the authenticated character",
          "[net-interest]") {
    Harness h;
    Pair second{"client-b"};
    const auto session_b = ready(*h.sessions, second, 2);
    const auto observer_b = h.entity(2);
    h.catalog.push_back(
        {observer_b, InterestVisibility::hidden, session_b.character, 0, false, true});
    h.public_entity(10, {10, 0, 0});
    h.catalog.push_back(
        {h.entity(11, {11, 0, 0}), InterestVisibility::owner_pose, h.session.character});
    h.catalog.push_back(
        {h.entity(12, {12, 0, 0}), InterestVisibility::owner_pose, session_b.character});
    h.catalog.push_back({h.entity(13, {13, 0, 0}), InterestVisibility::hidden, {}, 255, true});
    CHECK(h.publish().selected_entities == 2);
    const auto first = h.pair.update();
    CHECK(positions(first) == std::vector<double>{10, 11});
    auto other = h.server->publish(*h.source, h.catalog, *h.sessions, *second.server, 0ns);
    REQUIRE(other);
    CHECK(other.value().selected_entities == 2);
    const auto second_update = second.update();
    CHECK(positions(second_update) == std::vector<double>{10, 12});
    CHECK(first.session != second_update.session);
    CHECK(first.stream != second_update.stream);
    CHECK(h.sessions->current(*h.pair.server, 0ns).value().last_sequence == 1);
    CHECK(h.server->stats().value().streams == 2);
}

TEST_CASE("Interest hysteresis includes boundaries and retires exited replica identities",
          "[net-interest]") {
    Harness h;
    const auto actor = h.public_entity(10, {100, 0, 0});
    REQUIRE(h.publish().status == InterestPublishStatus::sent);
    auto update = h.pair.update();
    const auto first_id = update.changes.front().id;
    h.ack(update, 2);
    h.move(actor, {110, 0, 0});
    CHECK(h.publish(100ms).selected_entities == 1);
    update = h.pair.update(100ms);
    CHECK(update.changes.front().id == first_id);
    h.ack(update, 3, 100ms);
    h.move(actor, {std::nextafter(110.0, 111.0), 0, 0});
    CHECK(h.publish(200ms).selected_entities == 0);
    update = h.pair.update(200ms);
    REQUIRE(update.changes.size() == 1);
    CHECK(update.changes.front().kind == ReplicationChange::despawn);
    h.ack(update, 4, 200ms);
    h.move(actor, {105, 0, 0});
    CHECK(h.publish(300ms).status == InterestPublishStatus::unchanged);
    h.move(actor, {100, 0, 0});
    CHECK(h.publish(400ms).selected_entities == 1);
    update = h.pair.update(400ms);
    CHECK(update.changes.front().kind == ReplicationChange::spawn);
    CHECK(update.changes.front().id.value > first_id.value);
}

TEST_CASE("Interest distance handles tiny radii and opposite finite extremes", "[net-interest]") {
    SECTION("tiny positive radius does not underflow squared distance") {
        InterestLimits limits;
        limits.enter_radius = 1e-300;
        Harness h{limits};
        h.public_entity(10, {1e-300, 1e-300, 1e-300});
        h.public_entity(11, {1e-300, 0, 0});
        REQUIRE(h.publish().selected_entities == 1);
        const auto update = h.pair.update();
        REQUIRE(update.changes.size() == 1);
        CHECK(update.changes.front().pose->coefficients[7] == 0);
    }
    SECTION("finite subtraction overflow is spatially outside") {
        Harness h;
        h.move(h.catalog.front().entity, {-std::numeric_limits<double>::max(), 0, 0});
        h.public_entity(10, {std::numeric_limits<double>::max(), 0, 0});
        CHECK(h.publish().selected_entities == 0);
        CHECK(h.pair.update().changes.empty());
    }
}

TEST_CASE("Interest required rows and priorities cannot bypass privacy or radius",
          "[net-interest]") {
    InterestLimits limits;
    limits.replication.entities_per_stream = 2;
    Harness h{limits};
    h.public_entity(10, {10, 0, 0});
    h.catalog.back().required = true;
    h.public_entity(11, {11, 0, 0});
    h.catalog.back().priority = 200;
    h.public_entity(12, {12, 0, 0});
    h.catalog.back().priority = 1;
    h.catalog.push_back({h.entity(13), InterestVisibility::hidden, {}, 255, true});
    h.catalog.push_back(
        {h.entity(14), InterestVisibility::owner_pose, id<CharacterId>(2), 255, true});
    h.catalog.push_back(
        {h.entity(15, {111, 0, 0}), InterestVisibility::public_pose, {}, 255, true});
    const auto published = h.publish();
    CHECK(published.eligible_entities == 3);
    CHECK(published.omitted_entities == 1);
    const auto update = h.pair.update();
    CHECK(positions(update) == std::vector<double>{10, 11});
    h.ack(update, 2);
    h.catalog[2].required = true;
    h.catalog[3].required = true;
    rejected(h.server->publish(*h.source, h.catalog, *h.sessions, *h.pair.server, 100ms));
    CHECK(h.pair.server->stats().value().send_queue_frames == 0);
    CHECK(h.sessions->current(*h.pair.server, 100ms).value().last_sequence == 2);
}

TEST_CASE("Interest ties retain membership then choose deterministic local handles",
          "[net-interest]") {
    InterestLimits limits;
    limits.replication.entities_per_stream = 1;
    Harness h{limits};
    h.public_entity(10, {10, 0, 0});
    h.public_entity(11, {-10, 0, 0});
    std::ranges::reverse(h.catalog);
    CHECK(h.publish().selected_entities == 1);
    auto update = h.pair.update();
    CHECK(positions(update) == std::vector<double>{10});
    h.ack(update, 2);
    const auto closer = h.public_entity(12, {1, 0, 0});
    CHECK(h.publish(100ms).status == InterestPublishStatus::unchanged);
    h.catalog.back().priority = 1;
    CHECK(h.publish(200ms).status == InterestPublishStatus::sent);
    update = h.pair.update(200ms);
    CHECK(positions(update) == std::vector<double>{1});
    CHECK(update.changes.size() == 2);
    CHECK(h.source->reference(closer));
}

TEST_CASE("Interest selects cached world translations and preserves world shear",
          "[net-interest]") {
    Harness h;
    const auto parent = h.entity(10, {200, 0, 0});
    Transform parent_transform;
    parent_transform.translation = {20, 0, 0};
    parent_transform.scale = {2, 1, 1};
    REQUIRE(h.source->set_transform(parent, parent_transform));
    Transform child_transform;
    child_transform.translation = {3, 0, 0};
    child_transform.rotation = {std::cos(std::acos(-1.0) / 8), 0, 0, std::sin(std::acos(-1.0) / 8)};
    auto child = h.source->create_entity(
        {id<WorldEntityId>(11), {}, Name{"PRIVATE_CHILD"}, child_transform, parent});
    REQUIRE(child);
    h.catalog.push_back({child.value(), InterestVisibility::public_pose});
    CHECK(h.publish().selected_entities == 1);
    const auto update = h.pair.update();
    REQUIRE(update.changes.size() == 1);
    const auto pose = *update.changes.front().pose;
    CHECK(pose.coefficients[3] == Catch::Approx(26));
    CHECK(pose.coefficients[0] == Catch::Approx(std::sqrt(2.0)));
    CHECK(pose.coefficients[4] == Catch::Approx(std::sqrt(0.5)));
}

TEST_CASE("Interest coalesces current Scene state without resending pending frames",
          "[net-interest]") {
    Harness h;
    const auto actor = h.public_entity(10, {1, 0, 0});
    const auto first = h.publish();
    REQUIRE(first.status == InterestPublishStatus::sent);
    CHECK(h.pair.server->stats().value().send_queue_frames == 1);
    h.move(actor, {2, 0, 0});
    CHECK(h.publish(50ms).status == InterestPublishStatus::cadence_limited);
    CHECK(h.publish(100ms).status == InterestPublishStatus::pending_ack);
    h.move(actor, {3, 0, 0});
    CHECK(h.publish(200ms).status == InterestPublishStatus::pending_ack);
    CHECK(h.pair.server->stats().value().send_queue_frames == 1);
    const auto baseline = h.pair.update(200ms);
    CHECK(positions(baseline) == std::vector<double>{1});
    h.ack(baseline, 2, 200ms);
    h.move(actor, {4, 0, 0});
    const auto next = h.publish(300ms);
    CHECK(next.status == InterestPublishStatus::sent);
    CHECK(positions(h.pair.update(300ms)) == std::vector<double>{4});
    const auto stats = h.server->stats().value();
    CHECK(stats.publications == 2);
    CHECK(stats.framed_bytes == first.framed_bytes + next.framed_bytes);
    CHECK(stats.coalesced == 3);
    CHECK(h.sessions->current(*h.pair.server, 300ms).value().last_sequence == 2);
}

TEST_CASE("Interest leaves busy FIFO untouched and allocates baseline only on admission",
          "[net-interest]") {
    Harness h;
    const auto actor = h.public_entity(10, {1, 0, 0});
    auto reply = encode_session_reply(SessionRejected{});
    REQUIRE(reply);
    REQUIRE(h.pair.server->send(MessageKind::session_reply, reply.value()));
    const auto blocked = h.publish();
    CHECK(blocked.status == InterestPublishStatus::queue_limited);
    CHECK(blocked.framed_bytes == 0);
    CHECK(h.server->stats().value().publications == 0);
    REQUIRE(h.pair.receive().kind == MessageKind::session_reply);
    h.move(actor, {2, 0, 0});
    REQUIRE(h.publish(100ms).status == InterestPublishStatus::sent);
    const auto update = h.pair.update(100ms);
    CHECK(update.baseline);
    CHECK(update.revision == 1);
    CHECK(update.changes.front().id.value == 1);
    CHECK(positions(update) == std::vector<double>{2});
}

TEST_CASE("Interest token bucket debits exact framed bytes and refills without revision gaps",
          "[net-interest]") {
    InterestLimits limits;
    limits.framed_burst_bytes = 3762;
    limits.framed_bytes_per_second = 1000;
    limits.publish_period = 1ms;
    Harness h{limits};
    for (std::uint32_t index = 0; index < 32; ++index) {
        h.public_entity(10 + index, {static_cast<double>(index), 0, 0});
    }
    const auto first = h.publish();
    REQUIRE(first.status == InterestPublishStatus::sent);
    CHECK(first.framed_bytes == 3474);
    auto update = h.pair.update();
    h.ack(update, 2);
    for (std::size_t index = 1; index < h.catalog.size(); ++index) {
        h.move(h.catalog[index].entity, {static_cast<double>(index), 1, 0});
    }
    const auto limited = h.publish(100ms);
    CHECK(limited.status == InterestPublishStatus::byte_limited);
    CHECK(limited.framed_bytes == 0);
    CHECK(h.pair.server->stats().value().send_queue_frames == 0);
    CHECK(h.server->stats().value().framed_bytes == first.framed_bytes);
    CHECK(h.publish(3185ms).status == InterestPublishStatus::byte_limited);
    REQUIRE(h.publish(3186ms).status == InterestPublishStatus::sent);
    update = h.pair.update(3186ms);
    CHECK(update.revision == 2);
    CHECK(update.changes.size() == 32);
    CHECK(h.server->stats().value().framed_bytes == 6948);

    // A legal session interval times the maximum byte rate exceeds uint64.
    // Admission must cap credit before multiplying; this checks service clock
    // arithmetic using real authenticated peers, not an idle socket lifetime.
    InterestLimits large;
    large.framed_bytes_per_second = 1048576;
    SessionLimits long_session;
    long_session.session_lifetime = 24h;
    long_session.session_idle_timeout = 24h;
    Harness refill{large, long_session};
    const auto actor = refill.public_entity(10);
    REQUIRE(refill.publish().status == InterestPublishStatus::sent);
    refill.ack(refill.pair.update(), 2);
    refill.move(actor, {1, 0, 0});
    CHECK(refill.publish(MonotonicTime{24h} - 1ns).status == InterestPublishStatus::sent);
    CHECK(refill.server->stats().value().publications == 2);
}

TEST_CASE("Interest work exhaustion publishes nothing and consumes the selection cadence",
          "[net-interest]") {
    InterestLimits limits;
    limits.work_units = 1;
    Harness h{limits};
    h.public_entity(10);
    const auto first = h.publish();
    CHECK(first.status == InterestPublishStatus::work_limited);
    CHECK(first.work.selection_units() == 1);
    CHECK(first.work.policy_rows == 2);
    CHECK(h.pair.server->stats().value().send_queue_frames == 0);
    CHECK(h.publish(1ms).status == InterestPublishStatus::cadence_limited);
    CHECK(h.publish(100ms).status == InterestPublishStatus::work_limited);
    CHECK(h.server->stats().value().publications == 0);
    CHECK(h.server->stats().value().selection_units == 2);
    CHECK(h.sessions->current(*h.pair.server, 100ms).value().last_sequence == 1);
    auto other = scene();
    auto observer = other->create_entity({id<WorldEntityId>(1), {}, {}, {}, {}});
    REQUIRE(observer);
    const std::array replacement_catalog{InterestEntry{observer.value(), InterestVisibility::hidden,
                                                       h.session.character, 0, false, true}};
    auto replaced =
        h.server->publish(*other, replacement_catalog, *h.sessions, *h.pair.server, 101ms);
    REQUIRE(replaced);
    CHECK(replaced.value().status == InterestPublishStatus::privacy_closed);
    CHECK(h.server->stats().value().streams == 0);
}

TEST_CASE("Interest privacy revocation closes queued output before cadence permits selection",
          "[net-interest]") {
    Harness h;
    h.public_entity(10);
    REQUIRE(h.publish().status == InterestPublishStatus::sent);
    REQUIRE(h.pair.server->stats().value().send_queue_frames == 1);
    h.catalog.back().visibility = InterestVisibility::hidden;
    const auto result = h.publish(1ms);
    CHECK(result.status == InterestPublishStatus::privacy_closed);
    CHECK(result.work.selection_units() == 0);
    CHECK(h.pair.server->stats().value().state == TransportState::closed);
    CHECK(h.pair.server->stats().value().send_queue_frames == 0);
    CHECK(h.sessions->stats().value().live_sessions == 0);
    CHECK(h.server->stats().value().streams == 0);
    CHECK(h.server->stats().value().privacy_closures == 1);
}

TEST_CASE("Interest privacy revalidation precedes ACK admission and remains after ACK",
          "[net-interest]") {
    SECTION("control cannot renew a newly private disclosure") {
        InterestLimits limits;
        limits.work_units = 16;
        Harness h{limits};
        h.public_entity(10);
        h.catalog.back().visibility = InterestVisibility::owner_pose;
        h.catalog.back().owner = h.session.character;
        REQUIRE(h.publish().status == InterestPublishStatus::sent);
        const auto update = h.pair.update();
        h.catalog.back().owner = id<CharacterId>(2);
        for (std::uint32_t index = 0; index < 64; ++index) {
            h.catalog.push_back({h.entity(100 + index), InterestVisibility::hidden});
        }
        rejected(h.server->control(
            *h.source, h.catalog, *h.sessions, *h.pair.server,
            ReplicationAck{envelope(h.session, 2), update.stream, update.revision}, 1ms));
        CHECK(h.pair.server->stats().value().state == TransportState::closed);
        CHECK(h.sessions->stats().value().live_sessions == 0);
    }
    SECTION("acknowledged prior membership still fails closed") {
        Harness h;
        h.public_entity(10);
        REQUIRE(h.publish().status == InterestPublishStatus::sent);
        h.ack(h.pair.update(), 2);
        h.catalog.pop_back();
        CHECK(h.publish(1ms).status == InterestPublishStatus::privacy_closed);
    }
}

TEST_CASE("Interest rejects untrusted catalog shapes and closes uncertain admitted policy",
          "[net-interest]") {
    for (const auto variation : {0, 1, 2, 3, 4, 5}) {
        Harness h;
        h.public_entity(10);
        REQUIRE(h.publish().status == InterestPublishStatus::sent);
        switch (variation) {
        case 0:
            h.catalog.push_back(h.catalog.back());
            break;
        case 1:
            h.catalog.back().visibility = static_cast<InterestVisibility>(99);
            break;
        case 2:
            h.catalog.push_back({EntityHandle{}, InterestVisibility::hidden});
            break;
        case 3:
            h.catalog.front().observer = false;
            break;
        case 4:
            while (h.catalog.size() <= 128) {
                h.catalog.push_back(h.catalog.front());
            }
            break;
        default:
            REQUIRE(h.source->close());
            break;
        }
        CHECK(h.publish(1ms).status == InterestPublishStatus::privacy_closed);
        CHECK(h.pair.server->stats().value().send_queue_frames == 0);
        CHECK(h.sessions->stats().value().live_sessions == 0);
    }
    Harness fresh;
    fresh.catalog.front().owner.reset();
    rejected(fresh.server->publish(*fresh.source, fresh.catalog, *fresh.sessions,
                                   *fresh.pair.server, 0ns));
    CHECK(fresh.pair.server->stats().value().send_queue_frames == 0);
}

TEST_CASE("Interest destruction is a normal despawn and slot reuse gets a new replica identity",
          "[net-interest]") {
    Harness h;
    const auto old = h.public_entity(10, {10, 0, 0});
    REQUIRE(h.publish().status == InterestPublishStatus::sent);
    auto update = h.pair.update();
    const auto first_id = update.changes.front().id;
    h.ack(update, 2);
    REQUIRE(h.source->destroy_entity(old));
    h.catalog.pop_back();
    const auto replacement = h.public_entity(11, {11, 0, 0});
    CHECK(replacement != old);
    REQUIRE(h.publish(100ms).status == InterestPublishStatus::sent);
    update = h.pair.update(100ms);
    REQUIRE(update.changes.size() == 2);
    CHECK(update.changes.front().kind == ReplicationChange::despawn);
    CHECK(update.changes.front().id == first_id);
    CHECK(update.changes.back().id.value > first_id.value);
    CHECK(h.server->stats().value().privacy_closures == 0);
}

TEST_CASE("Interest live-session checks close expired or revoked recipients", "[net-interest]") {
    SECTION("expiry cannot leave queued data writable") {
        SessionLimits sessions;
        sessions.session_idle_timeout = 100ms;
        Harness h{{}, sessions};
        h.public_entity(10);
        REQUIRE(h.publish().status == InterestPublishStatus::sent);
        rejected(h.server->publish(*h.source, h.catalog, *h.sessions, *h.pair.server, 101ms));
        CHECK(h.pair.server->stats().value().state == TransportState::closed);
        CHECK(h.pair.server->stats().value().send_queue_frames == 0);
        REQUIRE(h.server->forget(h.session.session));
        CHECK(h.server->stats().value().streams == 0);
    }
    SECTION("revocation is checked even within selection cadence") {
        Harness h;
        h.public_entity(10);
        REQUIRE(h.publish().status == InterestPublishStatus::sent);
        REQUIRE(h.sessions->revoke_session(h.session.session));
        rejected(h.server->publish(*h.source, h.catalog, *h.sessions, *h.pair.server, 1ms));
        CHECK(h.pair.server->stats().value().state == TransportState::closed);
    }
}

TEST_CASE("Interest reconnect reclaims bounded streams and stale teardown cannot affect it",
          "[net-interest]") {
    InterestLimits limits;
    limits.replication.streams = 1;
    Harness h{limits};
    h.public_entity(10);
    REQUIRE(h.publish().status == InterestPublishStatus::sent);
    const auto old = h.pair.update();
    Pair replacement;
    const auto next = ready(*h.sessions, replacement, 1, 100ms);
    auto published =
        h.server->publish(*h.source, h.catalog, *h.sessions, *replacement.server, 100ms);
    REQUIRE(published);
    REQUIRE(published.value().status == InterestPublishStatus::sent);
    const auto baseline = replacement.update(100ms);
    CHECK(baseline.session == next.session);
    CHECK(baseline.stream != old.stream);
    CHECK(baseline.revision == 1);
    CHECK(h.server->stats().value().streams == 1);
    auto forgotten = h.server->forget(h.session.session);
    REQUIRE(forgotten);
    CHECK_FALSE(forgotten.value());
    rejected(h.server->control(*h.source, h.catalog, *h.sessions, *h.pair.server,
                               ReplicationAck{envelope(h.session, 2), old.stream, old.revision},
                               100ms));
    CHECK(h.server->stats().value().streams == 1);
    CHECK(h.sessions->current(*replacement.server, 100ms).value().session == next.session);
}

TEST_CASE("Interest resync preserves cadence and waits for queued reliable bytes",
          "[net-interest]") {
    Harness h;
    h.public_entity(10);
    REQUIRE(h.publish().status == InterestPublishStatus::sent);
    // Decode only after delivery; the resync then races a separately queued
    // session reply to prove no newer baseline overtakes reliable output.
    const auto baseline = h.pair.update();
    auto reply = encode_session_reply(SessionRejected{});
    REQUIRE(reply);
    REQUIRE(h.pair.server->send(MessageKind::session_reply, reply.value()));
    auto resync =
        h.server->control(*h.source, h.catalog, *h.sessions, *h.pair.server,
                          ReplicationResync{envelope(h.session, 2), baseline.stream, 0}, 1ms);
    REQUIRE(resync);
    CHECK(resync.value() == ReplicationControlResult::resync_requested);
    CHECK(h.publish(1ms).status == InterestPublishStatus::cadence_limited);
    CHECK(h.publish(100ms).status == InterestPublishStatus::queue_limited);
    REQUIRE(h.pair.receive(100ms).kind == MessageKind::session_reply);
    REQUIRE(h.publish(200ms).status == InterestPublishStatus::sent);
    const auto next = h.pair.update(200ms);
    CHECK(next.baseline);
    CHECK(next.revision == 2);
    CHECK(next.changes.front().id == baseline.changes.front().id);
}

TEST_CASE("Interest default work budget bounds a full crowded catalog", "[net-interest]") {
    Harness h;
    for (std::uint32_t index = 1; index < 128; ++index) {
        h.public_entity(10 + index, {static_cast<double>(index % 50), 0, 0});
        h.catalog.back().priority = static_cast<std::uint8_t>(index % 4);
    }
    std::ranges::reverse(h.catalog);
    const auto first = h.publish();
    REQUIRE(first.status == InterestPublishStatus::sent);
    CHECK(first.selected_entities == 32);
    CHECK(first.eligible_entities == 127);
    CHECK(first.omitted_entities == 95);
    CHECK(first.work.selection_units() <= 8192);
    CHECK(first.work.policy_rows == 128);
    CHECK(first.work.policy_scene_reads == 128);
    CHECK(first.work.scene_reads == 128);
    h.ack(h.pair.update(), 2);
    const auto next = h.publish(100ms);
    CHECK(next.status == InterestPublishStatus::unchanged);
    CHECK(next.work.selection_units() <= 8192);
    CHECK(next.work.privacy_checks == 32);
    CHECK(h.server->stats().value().last_sent_entities == 32);
}

TEST_CASE("Interest owner time and cross-connection controls reject without mutation",
          "[net-interest]") {
    Harness h;
    h.public_entity(10);
    REQUIRE(h.publish(100ms).status == InterestPublishStatus::sent);
    const auto baseline = h.pair.update(100ms);
    rejected(h.server->publish(*h.source, h.catalog, *h.sessions, *h.pair.server, 99ms));
    bool wrong_thread_rejected = false;
    std::thread worker{[&] { wrong_thread_rejected = !h.server->stats(); }};
    worker.join();
    CHECK(wrong_thread_rejected);
    Pair second{"client-b"};
    const auto second_session = ready(*h.sessions, second, 2, 100ms);
    rejected(h.server->control(
        *h.source, h.catalog, *h.sessions, *second.server,
        ReplicationAck{envelope(h.session, 2), baseline.stream, baseline.revision}, 100ms));
    CHECK(h.sessions->current(*second.server, 100ms).value().last_sequence ==
          second_session.last_sequence);
    CHECK(h.sessions->current(*h.pair.server, 100ms).value().last_sequence == 1);
    h.ack(baseline, 2, 100ms);
    CHECK(h.server->stats().value().streams == 1);

    // Absolute times close to the signed-clock maximum must use differences,
    // not unchecked additions for cadence or token accounting.
    InterestLimits late_limits;
    late_limits.replication.acknowledgement_timeout = 100ms;
    SessionLimits late_sessions;
    late_sessions.session_lifetime = 1s;
    late_sessions.session_idle_timeout = 1s;
    const auto start = MonotonicTime::max() - 2s;
    Harness late{late_limits, late_sessions, start};
    const auto late_actor = late.public_entity(10);
    REQUIRE(late.publish(start).status == InterestPublishStatus::sent);
    late.ack(late.pair.update(start), 2, start);
    late.move(late_actor, {1, 0, 0});
    CHECK(late.publish(start + 500ms).status == InterestPublishStatus::sent);
    rejected(late.server->publish(*late.source, late.catalog, *late.sessions, *late.pair.server,
                                  MonotonicTime::max()));
    CHECK(late.pair.server->stats().value().state == TransportState::closed);
}
