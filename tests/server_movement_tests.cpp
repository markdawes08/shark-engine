#include <shark/server/movement.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>

namespace {
using namespace shark;
using namespace shark::net;
using namespace shark::server;
using namespace shark::storage;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().context.operation << ": " << result.error().message);
    }
    REQUIRE(result);
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result) {
        INFO(result.error().context.operation << ": " << result.error().message);
    }
    REQUIRE(result);
}
template <typename T> T id(unsigned value) {
    std::string text = "a0400000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 15U];
        value >>= 4U;
    }
    return T{take(StableId::parse(text))};
}
std::string pem(const fs::path& name) {
    std::ifstream input{fs::path{SHARK_NETWORK_FIXTURE_DIRECTORY} / name, std::ios::binary};
    REQUIRE(input);
    return {std::istreambuf_iterator<char>{input}, {}};
}
SessionEnvelope envelope(const SessionSnapshot& session, std::uint64_t sequence) {
    return {session.session, session.ownership_epoch, sequence, session.character};
}
RigidBody character_body() {
    RigidBody result;
    result.motion = BodyMotion::dynamic_body;
    result.shape = ColliderShape::sphere;
    result.friction = 0;
    return result;
}
PhysicsWorldConfig profile() {
    PhysicsWorldConfig result;
    result.tick_hz = movement_tick_hz;
    result.collision_substeps = movement_collision_substeps;
    result.gravity = {};
    return result;
}
struct Pair {
    std::unique_ptr<TlsListener> listener;
    std::unique_ptr<TlsTransport> client;
    std::unique_ptr<TlsTransport> server;
    Pair() {
        listener = take(TlsListener::listen_loopback_authenticated(
            0, {pem("server.pem"), pem("server-key.pem")}, pem("ca.pem")));
        client = take(TlsTransport::connect_loopback_authenticated(
            take(listener->port()), {pem("ca.pem"), "shark.test"},
            {pem("session/client-a.pem"), pem("session/client-a-key.pem")}, 0ns));
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            if (!server) {
                auto accepted = take(listener->accept(0ns));
                if (accepted)
                    server = std::move(*accepted);
            }
            take(client->poll(0ns));
            if (server) {
                take(server->poll(0ns));
                if (take(client->stats()).state == TransportState::ready &&
                    take(server->stats()).state == TransportState::ready)
                    break;
            }
            std::this_thread::yield();
        }
    }
    Frame receive(bool at_server, MonotonicTime now) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            take(client->poll(now));
            take(server->poll(now));
            auto frame = take((at_server ? server : client)->receive());
            if (frame)
                return std::move(*frame);
            std::this_thread::yield();
        }
    }
};
struct Fixture {
    fs::path root;
    bool complete{false};
    Pair pair;
    std::unique_ptr<CharacterAuthority> authority;
    CharacterRecord record{id<AccountId>(2),
                           id<CharacterId>(10),
                           id<WorldEntityId>(11),
                           1,
                           {Name{"Mover"}, {id<WorldId>(1), {}, {0, 0, 0}}}};
    SessionSnapshot session{
        id<SessionId>(12), id<AccountId>(2), id<CharacterId>(10), 1, 0, false, false};
    std::unique_ptr<Scene> scene;
    std::unique_ptr<PhysicsWorld> physics;
    std::unique_ptr<CharacterMovement> movement;
    EntityHandle entity;
    PhysicsBodyHandle body;
    MovementState initial{id<SessionId>(12), 1, id<MovementStreamId>(13), 0, 0, {}, {}};
    WorldPosition origin{id<WorldId>(1), {1234, 0, -99}, {1023.9, 100, 1}};
    MonotonicTime now{0};
    std::uint64_t next_command{2};
    std::uint64_t next_input{1};

    explicit Fixture(bool bind = true, bool wall = false,
                     const RigidBody& settings = character_body()) {
        const fs::path parent{SHARK_MOVEMENT_SCRATCH};
        REQUIRE(parent.is_absolute());
        fs::create_directories(parent);
        root = parent / take(generate_stable_id()).to_string();
        REQUIRE(fs::create_directory(root));
        SessionLimits limits;
        limits.commands_per_window = 128;
        authority = take(CharacterAuthority::create(
            take(Store::create(root / "Movement.db", origin.world)), limits));
        take(authority->provision_account(id<AccountId>(2)));
        take(authority->enroll_certificate(
            id<AccountId>(2), take(certificate_fingerprint(pem("session/client-a.pem")))));
        const auto roster = take(authority->roster(*pair.server, id<AccountId>(2), now));
        record = take(authority->create_character(*pair.server, id<AccountId>(2), roster.revision,
                                                  {Name{"Mover"}, origin}, now));
        session = login(pair);
        scene = take(Scene::create(
            RegionSceneScope{origin.world, {id<RegionId>(3), id<RegionInstanceId>(4), 1}}));
        EntitySeed seed{record.entity};
        seed.rigid_body = settings;
        entity = take(scene->create_entity(seed));
        physics = take(PhysicsWorld::create(profile()));
        body = take(physics->create_body(*scene, entity));
        if (wall) {
            EntitySeed fixed{id<WorldEntityId>(5)};
            fixed.transform.translation.x = 2;
            fixed.rigid_body = RigidBody{};
            fixed.rigid_body->half_extents = {0.1, 10, 10};
            take(physics->create_body(*scene, take(scene->create_entity(fixed))));
        }
        take(physics->begin_simulation(*scene));
        movement = take(CharacterMovement::create(*scene, *physics, origin));
        if (bind)
            initial = take(movement->bind(*authority, *pair.server, *scene, *physics, entity, now));
    }
    ~Fixture() {
        movement.reset();
        physics.reset();
        scene.reset();
        authority.reset();
        if (!complete)
            return;
        std::error_code ignored;
        for (const auto* name : {"Movement.db", "Movement.db-journal"})
            fs::remove(root / name, ignored);
        fs::remove(root, ignored);
    }
    SessionSnapshot login(Pair& connection) {
        const auto logged = take(authority->apply(
            *connection.server, LoginRequest{record.account, record.character}, now));
        return take(
            authority->apply(*connection.server, SetReadyRequest{envelope(logged, 1), true}, now));
    }
    MovementInput input(std::int16_t x = 1000, std::int16_t z = 0) const {
        return {envelope(session, next_command), initial.stream, next_input, x, z};
    }
    void submit(std::int16_t x = 1000, std::int16_t z = 0) {
        take(movement->submit(*authority, *pair.server, *scene, *physics, input(x, z), now));
        ++next_command;
        ++next_input;
    }
    MovementTick step(bool connected = true) {
        now += 50ms;
        std::array<TlsTransport*, 1> live{pair.server.get()};
        return take(movement->step(*authority, *scene, *physics,
                                   connected ? std::span<TlsTransport* const>{live}
                                             : std::span<TlsTransport* const>{},
                                   now));
    }
    double x() const {
        return take(scene->entity_state(entity)).local_transform.translation.x;
    }
};
} // namespace

TEST_CASE("Server movement admits real TLS input and publishes the committed correction",
          "[server-movement]") {
    Fixture fixture;
    REQUIRE(fixture.initial.completed_tick == 0);
    REQUIRE(fixture.initial.last_input == 0);
    REQUIRE(take(fixture.authority->authorize_entity(*fixture.pair.server, *fixture.scene,
                                                     fixture.entity, 0ns)) == fixture.session);
    const auto requested = fixture.input();
    take(fixture.pair.client->send(MessageKind::movement_input,
                                   take(encode_movement_input(requested))));
    const auto received = fixture.pair.receive(true, fixture.now);
    REQUIRE(received.kind == MessageKind::movement_input);
    take(fixture.movement->submit(*fixture.authority, *fixture.pair.server, *fixture.scene,
                                  *fixture.physics, take(decode_movement_input(received.payload)),
                                  fixture.now));
    const auto tick = fixture.step();
    REQUIRE(tick.completed_tick == 1);
    REQUIRE(tick.fenced == 0);
    REQUIRE(tick.deliveries.size() == 1);
    const auto& state = tick.deliveries.front().state;
    REQUIRE(state.completed_tick == 1);
    REQUIRE(state.last_input == 1);
    REQUIRE(state.stream == fixture.initial.stream);
    REQUIRE(state.position.x == Catch::Approx(0.3).margin(1e-6));
    REQUIRE(state.velocity.x == Catch::Approx(6).margin(1e-6));
    take(fixture.pair.server->send(MessageKind::movement_state,
                                   take(encode_movement_reply(MovementReply{state}))));
    const auto correction = fixture.pair.receive(false, fixture.now);
    REQUIRE(correction.kind == MessageKind::movement_state);
    REQUIRE(std::get<MovementState>(take(decode_movement_reply(correction.payload))) == state);
    REQUIRE_FALSE(fixture.pair.client->send(MessageKind::movement_state, correction.payload));
    REQUIRE_FALSE(fixture.pair.server->send(MessageKind::movement_input, received.payload));
    const auto global =
        take(fixture.movement->position(*fixture.scene, *fixture.physics, fixture.initial.stream));
    REQUIRE(global.cell.x == 1235);
    REQUIRE(global.cell.z == -99);
    REQUIRE(global.offset[0] == Catch::Approx(0.2).margin(1e-6));
    const auto stopped = fixture.step();
    REQUIRE(stopped.deliveries.front().state.last_input == 1);
    REQUIRE(fixture.x() == Catch::Approx(state.position.x).margin(1e-6));
    REQUIRE(take(fixture.physics->velocity(*fixture.scene, fixture.body)).linear.x == 0);
    fixture.complete = true;
}

TEST_CASE("Movement shape and bounded queue rejection preserve exact shared admission",
          "[server-movement]") {
    Fixture fixture;
    auto invalid = fixture.input(1000, 1000);
    REQUIRE_FALSE(fixture.movement->submit(*fixture.authority, *fixture.pair.server, *fixture.scene,
                                           *fixture.physics, invalid, fixture.now));
    invalid = fixture.input();
    invalid.stream = id<MovementStreamId>(90);
    REQUIRE_FALSE(fixture.movement->submit(*fixture.authority, *fixture.pair.server, *fixture.scene,
                                           *fixture.physics, invalid, fixture.now));
    invalid = fixture.input();
    invalid.input_sequence = 2;
    REQUIRE_FALSE(fixture.movement->submit(*fixture.authority, *fixture.pair.server, *fixture.scene,
                                           *fixture.physics, invalid, fixture.now));
    invalid = fixture.input();
    ++invalid.authority.sequence;
    REQUIRE_FALSE(fixture.movement->submit(*fixture.authority, *fixture.pair.server, *fixture.scene,
                                           *fixture.physics, invalid, fixture.now));
    for (std::size_t index = 0; index < movement_pending_inputs; ++index)
        fixture.submit();
    REQUIRE(fixture.x() == 0);
    REQUIRE(take(fixture.movement->stats()).pending_inputs == 8);
    const auto full = fixture.input();
    REQUIRE_FALSE(fixture.movement->submit(*fixture.authority, *fixture.pair.server, *fixture.scene,
                                           *fixture.physics, full, fixture.now));
    const auto first = fixture.step();
    REQUIRE(first.deliveries.front().state.last_input == 1);
    REQUIRE(fixture.x() == Catch::Approx(0.3).margin(1e-6));
    fixture.submit(); // Same session/input numbers rejected by the full queue now succeed.
    for (std::uint64_t tick = 2; tick <= 9; ++tick) {
        const auto advanced = fixture.step();
        REQUIRE(advanced.completed_tick == tick);
        REQUIRE(advanced.deliveries.front().state.last_input == tick);
        REQUIRE(fixture.x() == Catch::Approx(static_cast<double>(tick) * 0.3).margin(2e-6));
    }
    REQUIRE(take(fixture.movement->stats()).pending_inputs == 0);
    const auto position = fixture.x();
    fixture.step();
    REQUIRE(fixture.x() == Catch::Approx(position).margin(1e-6));
    fixture.complete = true;
}

TEST_CASE("Authoritative character movement collides with native world geometry",
          "[server-movement]") {
    Fixture fixture{true, true};
    bool contact = false;
    for (unsigned tick = 0; tick < 16; ++tick) {
        fixture.submit();
        const auto result = fixture.step();
        for (const auto& event : result.contacts)
            contact = contact || event.phase == PhysicsContactPhase::enter;
        REQUIRE(fixture.x() < 1.6);
    }
    REQUIRE(contact);
    REQUIRE(fixture.x() > 1.0);
    REQUIRE(take(fixture.physics->stats()).completed_ticks == 16);
    REQUIRE_FALSE(take(fixture.movement->stats()).faulted);
    fixture.complete = true;
}

TEST_CASE("Queued movement loses authority before native work and never reaches replacements",
          "[server-movement]") {
    Fixture fixture;
    fixture.submit();
    fixture.step();
    fixture.submit();
    const auto before = fixture.x();
    SECTION("missing transport") {
        const auto result = fixture.step(false);
        REQUIRE(result.fenced == 1);
        REQUIRE(result.deliveries.empty());
    }
    SECTION("revoked account") {
        take(fixture.authority->revoke_account(fixture.record.account));
        const auto result = fixture.step();
        REQUIRE(result.fenced == 1);
        REQUIRE(result.deliveries.empty());
    }
    SECTION("replacement session and fresh stream") {
        Pair replacement;
        const auto session = fixture.login(replacement);
        fixture.now += 50ms;
        const std::array<TlsTransport*, 2> live{fixture.pair.server.get(),
                                                replacement.server.get()};
        const auto result = take(fixture.movement->step(*fixture.authority, *fixture.scene,
                                                        *fixture.physics, live, fixture.now));
        REQUIRE(result.fenced == 1);
        REQUIRE(result.deliveries.empty());
        const auto rebound =
            take(fixture.movement->bind(*fixture.authority, *replacement.server, *fixture.scene,
                                        *fixture.physics, fixture.entity, fixture.now));
        REQUIRE(rebound.stream != fixture.initial.stream);
        REQUIRE(rebound.session == session.session);
        REQUIRE(rebound.last_input == 0);
        auto old = fixture.input();
        old.authority = envelope(session, 2);
        REQUIRE_FALSE(fixture.movement->submit(*fixture.authority, *replacement.server,
                                               *fixture.scene, *fixture.physics, old, fixture.now));
    }
    SECTION("body generation replaced") {
        take(fixture.physics->remove_body(*fixture.scene, fixture.body));
        const auto replacement = take(fixture.physics->create_runtime_body(
            *fixture.scene, take(fixture.scene->reference(fixture.entity)), character_body()));
        REQUIRE(replacement != fixture.body);
        const auto result = fixture.step();
        REQUIRE(result.fenced == 1);
        REQUIRE(result.deliveries.empty());
        REQUIRE_FALSE(fixture.physics->body_state(*fixture.scene, fixture.body));
        fixture.body = replacement;
    }
    SECTION("entity generation replaced") {
        take(fixture.physics->remove_body(*fixture.scene, fixture.body));
        take(fixture.physics->end_simulation(*fixture.scene));
        const auto old = take(fixture.scene->reference(fixture.entity));
        take(fixture.scene->destroy_entity(fixture.entity));
        EntitySeed replacement{fixture.record.entity};
        replacement.transform.translation.x = before;
        replacement.rigid_body = character_body();
        fixture.entity = take(fixture.scene->create_entity(replacement));
        fixture.body = take(fixture.physics->create_body(*fixture.scene, fixture.entity));
        take(fixture.physics->begin_simulation(*fixture.scene));
        REQUIRE_FALSE(fixture.scene->resolve(old));
        const auto result = fixture.step();
        REQUIRE(result.fenced == 1);
        REQUIRE(result.deliveries.empty());
    }
    REQUIRE(fixture.x() == Catch::Approx(before).margin(1e-6));
    REQUIRE(take(fixture.physics->velocity(*fixture.scene, fixture.body)).linear.x == 0);
    REQUIRE(take(fixture.movement->stats()).pending_inputs == 0);
    fixture.complete = true;
}

TEST_CASE("Movement admission preserves durable ownership profile and owner-thread boundaries",
          "[server-movement]") {
    SECTION("only the ready owned authoritative entity can bind") {
        Fixture fixture{false};
        auto foreign = take(Scene::create(
            RegionSceneScope{id<WorldId>(99), {id<RegionId>(3), id<RegionInstanceId>(4), 1}}));
        const auto wrong = take(foreign->create_entity(EntitySeed{fixture.record.entity}));
        REQUIRE_FALSE(fixture.authority->authorize_entity(*fixture.pair.server, *foreign, wrong,
                                                          fixture.now));
        auto not_ready = take(fixture.authority->apply(
            *fixture.pair.server, SetReadyRequest{envelope(fixture.session, 2), false},
            fixture.now));
        REQUIRE_FALSE(fixture.movement->bind(*fixture.authority, *fixture.pair.server,
                                             *fixture.scene, *fixture.physics, fixture.entity,
                                             fixture.now));
        fixture.session = take(fixture.authority->apply(
            *fixture.pair.server, SetReadyRequest{envelope(not_ready, 3), true}, fixture.now));
        const auto bound =
            take(fixture.movement->bind(*fixture.authority, *fixture.pair.server, *fixture.scene,
                                        *fixture.physics, fixture.entity, fixture.now));
        REQUIRE(bound.session == fixture.session.session);
        REQUIRE(take(fixture.authority->authorize_entity(*fixture.pair.server, *fixture.scene,
                                                         fixture.entity, fixture.now))
                    .last_sequence == 3);
        bool rejected = false;
        std::thread foreign_thread{[&] { rejected = !fixture.movement->stats(); }};
        foreign_thread.join();
        REQUIRE(rejected);
        fixture.complete = true;
    }
    SECTION("native collider profile is checked rather than inferred from dynamic motion") {
        auto settings = character_body();
        settings.radius = 0.75;
        Fixture fixture{false, false, settings};
        REQUIRE_FALSE(fixture.movement->bind(*fixture.authority, *fixture.pair.server,
                                             *fixture.scene, *fixture.physics, fixture.entity,
                                             fixture.now));
        REQUIRE(take(fixture.movement->stats()).characters == 0);
        fixture.complete = true;
    }
    SECTION("observing live ownership cannot refresh an idle session") {
        Fixture fixture{false};
        const auto observed = take(fixture.authority->authorize_entity(
            *fixture.pair.server, *fixture.scene, fixture.entity, 10s));
        REQUIRE(observed.last_sequence == fixture.session.last_sequence);
        REQUIRE_FALSE(fixture.authority->authorize_entity(*fixture.pair.server, *fixture.scene,
                                                          fixture.entity, 60s));
        fixture.complete = true;
    }
    SECTION("an empty movement owner cannot adopt a replacement physics world") {
        Fixture fixture{false};
        take(fixture.physics->close());
        auto replacement = take(PhysicsWorld::create(profile()));
        take(replacement->create_body(*fixture.scene, fixture.entity));
        take(replacement->begin_simulation(*fixture.scene));
        REQUIRE_FALSE(fixture.movement->bind(*fixture.authority, *fixture.pair.server,
                                             *fixture.scene, *replacement, fixture.entity,
                                             fixture.now));
        REQUIRE(take(fixture.movement->stats()).characters == 0);
        fixture.complete = true;
    }
}

TEST_CASE("Movement speed ceilings and uncertain completed ticks retain truthful outcomes",
          "[server-movement]") {
    SECTION("terminal vertical velocity leaves room for horizontal input") {
        Fixture fixture;
        take(fixture.physics->set_velocity(*fixture.scene, fixture.body,
                                           {{0, -physics_maximum_linear_speed, 0}, {}}));
        fixture.submit();
        const auto tick = fixture.step();
        REQUIRE(tick.deliveries.front().state.velocity.x == Catch::Approx(6).margin(1e-6));
        REQUIRE(tick.deliveries.front().state.velocity.y == Catch::Approx(-250).margin(1e-6));
        REQUIRE_FALSE(take(fixture.movement->stats()).faulted);
        fixture.complete = true;
    }
    SECTION("a completed native move with an unrepresentable global result cannot replay") {
        Fixture fixture{false};
        auto edge = fixture.origin;
        edge.cell.x = world_cell_max;
        edge.offset[0] = 1023.9;
        auto movement = take(CharacterMovement::create(*fixture.scene, *fixture.physics, edge));
        const auto initial =
            take(movement->bind(*fixture.authority, *fixture.pair.server, *fixture.scene,
                                *fixture.physics, fixture.entity, 0ns));
        auto input = fixture.input();
        input.stream = initial.stream;
        take(movement->submit(*fixture.authority, *fixture.pair.server, *fixture.scene,
                              *fixture.physics, input, 0ns));
        const std::array<TlsTransport*, 1> live{fixture.pair.server.get()};
        REQUIRE_FALSE(
            movement->step(*fixture.authority, *fixture.scene, *fixture.physics, live, 50ms));
        REQUIRE(fixture.x() == Catch::Approx(0.3).margin(1e-6));
        const auto stats = take(movement->stats());
        REQUIRE(stats.faulted);
        REQUIRE(stats.completed_ticks == 1);
        REQUIRE_FALSE(
            movement->step(*fixture.authority, *fixture.scene, *fixture.physics, live, 100ms));
        REQUIRE(take(fixture.physics->stats()).completed_ticks == 1);
        fixture.complete = true;
    }
}
