#include <shark/net/fault_link.hpp>
#include <shark/net/movement_prediction.hpp>
#include <shark/server/combat.hpp>
#include <shark/server/movement.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
    explicit Pair(bool second = false) {
        listener = take(TlsListener::listen_loopback_authenticated(
            0, {pem("server.pem"), pem("server-key.pem")}, pem("ca.pem")));
        client = take(TlsTransport::connect_loopback_authenticated(
            take(listener->port()), {pem("ca.pem"), "shark.test"},
            {pem(second ? "session/client-b.pem" : "session/client-a.pem"),
             pem(second ? "session/client-b-key.pem" : "session/client-a-key.pem")},
            0ns));
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
    std::unique_ptr<Pair> peer;
    std::optional<SessionSnapshot> peer_session;
    EntityHandle peer_entity;
    PhysicsBodyHandle peer_body;

    explicit Fixture(bool bind = true, bool wall = false,
                     const RigidBody& settings = character_body(), bool duel = false) {
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
        if (duel) {
            peer = std::make_unique<Pair>(true);
            const auto account = id<AccountId>(20);
            take(authority->provision_account(account));
            take(authority->enroll_certificate(
                account, take(certificate_fingerprint(pem("session/client-b.pem")))));
            const auto list = take(authority->roster(*peer->server, account, now));
            const auto other = take(authority->create_character(
                *peer->server, account, list.revision, {Name{"Opponent"}, origin}, now));
            const auto logged =
                take(authority->apply(*peer->server, LoginRequest{account, other.character}, now));
            peer_session = take(
                authority->apply(*peer->server, SetReadyRequest{envelope(logged, 1), true}, now));
            EntitySeed opponent{other.entity};
            opponent.rigid_body = settings;
            opponent.transform.translation.x = 2;
            peer_entity = take(scene->create_entity(opponent));
            peer_body = take(physics->create_body(*scene, peer_entity));
        }
        if (wall) {
            EntitySeed fixed{id<WorldEntityId>(5)};
            fixed.transform.translation.x = duel ? 1 : 2;
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

constexpr const char* combat_source = R"(
function on_combat(kind, tick, actor, target, distance)
  if kind == 1 then return true, 25, 10 end
  return true, 0, 5
end
)";
struct Duel {
    Fixture fixture;
    std::unique_ptr<CharacterCombat> combat;
    std::optional<CombatBinding> a;
    std::optional<CombatBinding> b;
    std::uint64_t command_a{2}, command_b{2}, action_a{1}, action_b{1};
    explicit Duel(bool wall = false, const char* source = combat_source)
        : fixture(false, wall, character_body(), true) {
        combat = take(
            CharacterCombat::create(*fixture.scene, *fixture.physics,
                                    take(CombatRules::create({"scripts/combat.lua", source}))));
        a = take(combat->bind(*fixture.authority, *fixture.pair.server, *fixture.movement,
                              *fixture.scene, *fixture.physics, fixture.entity, fixture.now));
        b = take(combat->bind(*fixture.authority, *fixture.peer->server, *fixture.movement,
                              *fixture.scene, *fixture.physics, fixture.peer_entity, fixture.now));
    }
    CombatAction send(CombatActionKind kind = CombatActionKind::strike, bool second = false) {
        auto& pair = second ? *fixture.peer : fixture.pair;
        const auto& session = second ? *fixture.peer_session : fixture.session;
        auto& command = second ? command_b : command_a;
        auto& sequence = second ? action_b : action_a;
        const auto& self = second ? *b : *a;
        const auto& target = second ? *a : *b;
        CombatAction action{envelope(session, command), self.stream, sequence, target.target, kind};
        take(pair.client->send(MessageKind::combat_action, take(encode_combat_action(action))));
        const auto frame = pair.receive(true, fixture.now);
        REQUIRE(frame.kind == MessageKind::combat_action);
        take(combat->submit(*fixture.authority, *pair.server, *fixture.scene, *fixture.physics,
                            take(decode_combat_action(frame.payload)), fixture.now));
        ++command;
        ++sequence;
        return action;
    }
    CombatTick step() {
        fixture.now += 50ms;
        const std::array<TlsTransport*, 2> transports{fixture.pair.server.get(),
                                                      fixture.peer->server.get()};
        auto report = take(combat->step(*fixture.authority, *fixture.movement, *fixture.scene,
                                        *fixture.physics, transports, fixture.now));
        for (const auto& rebound : report.rebindings) {
            if (rebound.previous == a->target)
                a = rebound.binding;
            if (rebound.previous == b->target)
                b = rebound.binding;
        }
        return report;
    }
    CombatState state(bool second = false, bool selected = true) {
        const std::array<CombatTargetId, 1> targets{second ? a->target : b->target};
        auto& pair = second ? *fixture.peer : fixture.pair;
        return take(combat->view(*fixture.authority, *pair.server, *fixture.scene, *fixture.physics,
                                 selected ? std::span<const CombatTargetId>{targets}
                                          : std::span<const CombatTargetId>{},
                                 fixture.now));
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

TEST_CASE("Predicted movement reconciles actual native corrections through bounded semantic faults",
          "[server-movement][movement-connected]") {
    Fixture fixture{true, true};
    auto predictor = take(MovementPredictor::create(fixture.session, fixture.initial, {0, 8}));
    FaultLinkConfig config;
    config.seed = 1337;
    config.delay = 100ms;
    config.jitter = 30ms;
    config.reorder_delay = 80ms;
    config.loss_per_10000 = 500;
    config.duplication_per_10000 = 10000;
    config.reorder_per_10000 = 5000;
    auto link = take(FaultLink::create(config));
    bool saturated = false;
    bool duplicate = false;
    std::optional<MovementState> latest;
    for (unsigned tick = 0; tick < 40; ++tick) {
        for (const auto& delivery : take(link->drain(fixture.now, 64))) {
            const auto before = take(predictor->snapshot());
            const auto outcome = predictor->apply(take(decode_movement_reply(delivery.payload)));
            if (!outcome) {
                // Deliberately reordered older corrections cannot roll back a
                // newer accepted state or repeat the predicted input history.
                const auto after = take(predictor->snapshot());
                REQUIRE(after.revision == before.revision);
                REQUIRE(after.authoritative == before.authoritative);
                REQUIRE(after.predicted == before.predicted);
            } else {
                duplicate = duplicate || outcome.value() == PredictionCorrectionResult::duplicate;
            }
        }
        const auto before = take(predictor->snapshot());
        auto prepared =
            predictor->prepare(envelope(fixture.session, fixture.next_command), 1000, 0);
        if (before.pending_inputs == 8) {
            REQUIRE_FALSE(prepared);
            saturated = true;
        } else {
            const auto exact = take(std::move(prepared));
            REQUIRE(take(predictor->send(*fixture.pair.client, exact)) ==
                    PredictionSendResult::admitted);
            const auto received = fixture.pair.receive(true, fixture.now);
            REQUIRE(received.kind == MessageKind::movement_input);
            take(fixture.movement->submit(
                *fixture.authority, *fixture.pair.server, *fixture.scene, *fixture.physics,
                take(decode_movement_input(received.payload)), fixture.now));
            ++fixture.next_command;
        }
        const auto advanced = fixture.step();
        REQUIRE(advanced.deliveries.size() == 1);
        latest = advanced.deliveries.front().state;
        take(fixture.pair.server->send(MessageKind::movement_state,
                                       take(encode_movement_reply(MovementReply{*latest}))));
        const auto received = fixture.pair.receive(false, fixture.now);
        REQUIRE(received.kind == MessageKind::movement_state);
        if (tick == 5)
            take(link->partition_until(fixture.now, fixture.now + 500ms));
        take(link->send(fixture.now, received.payload, latest->completed_tick));
        REQUIRE(take(predictor->snapshot()).pending_inputs <= 8);
        REQUIRE(fixture.x() < 1.6);
    }
    REQUIRE(saturated);
    REQUIRE(duplicate);
    REQUIRE(take(link->stats()).partition_drops > 0);
    REQUIRE(take(link->stats()).reordered_messages > 0);
    REQUIRE(take(fixture.physics->stats()).completed_ticks == 40);
    const auto committed_revision = take(fixture.scene->stats()).revision;
    const auto final_result = take(predictor->apply(MovementReply{*latest}));
    REQUIRE(final_result == PredictionCorrectionResult::applied);
    const auto final = take(predictor->snapshot());
    REQUIRE(final.pending_inputs == 0);
    REQUIRE(final.predicted.position == latest->position);
    REQUIRE(final.predicted.velocity == latest->velocity);
    REQUIRE(take(fixture.scene->stats()).revision == committed_revision);
    REQUIRE(take(predictor->apply(MovementReply{*latest})) ==
            PredictionCorrectionResult::duplicate);
    fixture.complete = true;
}

TEST_CASE("A second authenticated client interpolates published native character movement",
          "[server-movement][movement-connected]") {
    Fixture fixture;
    Pair observer{true};
    const auto account = id<AccountId>(20);
    take(fixture.authority->provision_account(account));
    take(fixture.authority->enroll_certificate(
        account, take(certificate_fingerprint(pem("session/client-b.pem")))));
    const auto roster = take(fixture.authority->roster(*observer.server, account, 0ns));
    const auto record = take(fixture.authority->create_character(
        *observer.server, account, roster.revision, {Name{"Observer"}, fixture.origin}, 0ns));
    const auto logged = take(
        fixture.authority->apply(*observer.server, LoginRequest{account, record.character}, 0ns));
    const auto ready = take(fixture.authority->apply(
        *observer.server, SetReadyRequest{envelope(logged, 1), true}, 0ns));
    auto publisher = take(ReplicationServer::create());
    auto replica = take(ReplicaClient::create(ready));
    std::uint64_t control_sequence = 2;
    const std::array<EntityHandle, 1> selection{fixture.entity};
    // Reject another durable world before creating a stream or admitting bytes.
    auto foreign = take(Scene::create(
        RegionSceneScope{id<WorldId>(99), {id<RegionId>(3), id<RegionInstanceId>(4), 1}}));
    const std::array<EntityHandle, 1> foreign_selection{
        take(foreign->create_entity(EntitySeed{id<WorldEntityId>(90)}))};
    REQUIRE_FALSE(fixture.authority->publish_replication(*publisher, *foreign, foreign_selection,
                                                         *observer.server, 0ns, 4096));
    REQUIRE(take(publisher->stats()).streams == 0);
    REQUIRE(take(observer.server->stats()).send_queue_frames == 0);
    for (unsigned tick = 0; tick < 6; ++tick) {
        fixture.submit();
        fixture.step();
        const auto publication = take(fixture.authority->publish_replication(
            *publisher, *fixture.scene, selection, *observer.server, fixture.now, 4096));
        REQUIRE(publication.status == ReplicationPublishStatus::sent);
        const auto received = observer.receive(false, fixture.now);
        REQUIRE(received.kind == MessageKind::replication_state);
        const auto update = take(decode_replication_update(received.payload));
        REQUIRE(update.session == ready.session);
        REQUIRE(take(replica->apply(update, fixture.now)) == ReplicaApplyResult::applied);
        if (tick != 0) {
            const auto committed = take(replica->snapshot());
            const auto sampled = take(replica->sample(fixture.now - 25ms));
            REQUIRE(sampled.interpolated);
            REQUIRE(sampled.entities.size() == 1);
            REQUIRE(sampled.entities.front().pose.coefficients[3] ==
                    Catch::Approx(static_cast<double>(tick) * 0.3 + 0.15).margin(2e-6));
            REQUIRE(take(replica->snapshot()).entities == committed.entities);
        }
        take(observer.client->send(
            MessageKind::replication_control,
            take(encode_replication_control(ReplicationAck{envelope(ready, control_sequence),
                                                           update.stream, update.revision}))));
        const auto control = observer.receive(true, fixture.now);
        REQUIRE(control.kind == MessageKind::replication_control);
        REQUIRE(take(fixture.authority->control_replication(
                    *publisher, *observer.server, take(decode_replication_control(control.payload)),
                    fixture.now)) == ReplicationControlResult::acknowledged);
        ++control_sequence;
    }
    REQUIRE(take(replica->stats()).presentation_frames == 6);
    REQUIRE(take(fixture.movement->stats()).completed_ticks == 6);
    fixture.complete = true;
}

TEST_CASE("Authenticated combat uses server Lua damage cooldowns and bounded interaction outcomes",
          "[server-combat]") {
    Duel duel;
    const auto original = duel.send();
    auto queued = original;
    ++queued.action_sequence;
    ++queued.authority.sequence;
    REQUIRE_FALSE(duel.combat->submit(*duel.fixture.authority, *duel.fixture.pair.server,
                                      *duel.fixture.scene, *duel.fixture.physics, queued,
                                      duel.fixture.now));
    const auto hit = duel.step();
    REQUIRE_FALSE(hit.error);
    REQUIRE(hit.outcomes.size() == 1);
    REQUIRE(hit.outcomes.front().outcome.outcome == CombatOutcome::hit);
    REQUIRE(hit.outcomes.front().outcome.amount == 25);
    REQUIRE(duel.state(true).health == 75);
    REQUIRE(duel.state().ready_tick == 11);
    REQUIRE_FALSE(duel.combat->submit(*duel.fixture.authority, *duel.fixture.pair.server,
                                      *duel.fixture.scene, *duel.fixture.physics, original,
                                      duel.fixture.now));
    const auto state = duel.state();
    take(duel.fixture.pair.server->send(MessageKind::combat_state,
                                        take(encode_combat_reply(CombatReply{state}))));
    const auto received = duel.fixture.pair.receive(false, duel.fixture.now);
    REQUIRE(received.kind == MessageKind::combat_state);
    REQUIRE(std::get<CombatState>(take(decode_combat_reply(received.payload))) == state);
    REQUIRE_FALSE(duel.fixture.pair.client->send(MessageKind::combat_state, received.payload));
    REQUIRE_FALSE(duel.fixture.pair.server->send(MessageKind::combat_action,
                                                 take(encode_combat_action(original))));
    duel.send();
    REQUIRE(duel.step().outcomes.front().outcome.outcome == CombatOutcome::cooldown);
    REQUIRE(duel.state(true).health == 75);
    while (take(duel.combat->stats()).completed_ticks < 10)
        REQUIRE_FALSE(duel.step().error);
    duel.send(CombatActionKind::interact);
    const auto interaction = duel.step();
    REQUIRE_FALSE(interaction.error);
    REQUIRE(interaction.outcomes.front().outcome.outcome == CombatOutcome::interacted);
    REQUIRE(interaction.outcomes.front().outcome.amount == 0);
    REQUIRE(duel.state().ready_tick == 16);
    REQUIRE(duel.state(true).health == 75);
    duel.fixture.complete = true;
}

TEST_CASE("Combat range visibility and checked target generations reject before Lua execution",
          "[server-combat]") {
    constexpr const char* never = "function on_combat(...) error('must not run') end";
    SECTION("current three-dimensional range") {
        Duel duel{false, never};
        auto pose =
            take(duel.fixture.physics->body_state(*duel.fixture.scene, duel.fixture.peer_body))
                .pose;
        pose.translation.y = 4;
        take(duel.fixture.physics->teleport(*duel.fixture.scene, duel.fixture.peer_body, pose));
        duel.send();
        const auto report = duel.step();
        REQUIRE_FALSE(report.error);
        REQUIRE(report.outcomes.front().outcome.outcome == CombatOutcome::out_of_range);
        REQUIRE(duel.state(true).health == 100);
        duel.fixture.complete = true;
    }
    SECTION("a committed native wall blocks the target ray") {
        Duel duel{true, never};
        duel.send();
        const auto report = duel.step();
        REQUIRE_FALSE(report.error);
        REQUIRE(report.outcomes.front().outcome.outcome == CombatOutcome::blocked);
        REQUIRE(duel.state(true).health == 100);
        duel.fixture.complete = true;
    }
    SECTION("unknown life token and foreign authenticated actor cannot admit an action") {
        Duel duel;
        CombatAction action{envelope(duel.fixture.session, 2), duel.a->stream, 1,
                            id<CombatTargetId>(99), CombatActionKind::strike};
        REQUIRE_FALSE(duel.combat->submit(*duel.fixture.authority, *duel.fixture.pair.server,
                                          *duel.fixture.scene, *duel.fixture.physics, action,
                                          duel.fixture.now));
        action.target = duel.b->target;
        REQUIRE_FALSE(duel.combat->submit(*duel.fixture.authority, *duel.fixture.peer->server,
                                          *duel.fixture.scene, *duel.fixture.physics, action,
                                          duel.fixture.now));
        duel.send();
        REQUIRE(duel.step().outcomes.front().outcome.amount == 25);
        duel.fixture.complete = true;
    }
}

TEST_CASE("Death discards queued movement and respawn rotates checked body and action identities",
          "[server-combat]") {
    Duel duel;
    auto near =
        take(duel.fixture.physics->body_state(*duel.fixture.scene, duel.fixture.peer_body)).pose;
    near.translation.x = 1;
    take(duel.fixture.physics->teleport(*duel.fixture.scene, duel.fixture.peer_body, near));
    const auto old = *duel.b;
    const auto old_body = duel.fixture.peer_body;
    const auto entity = take(duel.fixture.scene->reference(duel.fixture.peer_entity));
    for (unsigned strike = 0; strike < 3; ++strike) {
        while (take(duel.combat->stats()).completed_ticks < strike * 10)
            REQUIRE_FALSE(duel.step().error);
        duel.send();
        REQUIRE(duel.step().outcomes.front().outcome.amount == 25);
    }
    while (take(duel.combat->stats()).completed_ticks < 30)
        REQUIRE_FALSE(duel.step().error);
    for (std::uint64_t input = 1; input <= 8; ++input) {
        take(duel.fixture.movement->submit(*duel.fixture.authority, *duel.fixture.peer->server,
                                           *duel.fixture.scene, *duel.fixture.physics,
                                           {envelope(*duel.fixture.peer_session, duel.command_b),
                                            old.movement.stream, input,
                                            static_cast<std::int16_t>(input == 1 ? 0 : 1000), 0},
                                           duel.fixture.now));
        ++duel.command_b;
    }
    duel.send();
    const auto death = duel.step();
    REQUIRE_FALSE(death.error);
    REQUIRE(death.deaths == 1);
    REQUIRE(death.released_movements == 1);
    REQUIRE(death.removed_bodies == 1);
    REQUIRE(duel.state(true).health == 0);
    REQUIRE(duel.state(true).respawn_tick == 91);
    REQUIRE_FALSE(duel.state(true).movement);
    REQUIRE(take(duel.fixture.movement->stats()).pending_inputs == 0);
    REQUIRE(take(duel.fixture.physics->stats()).bodies == 1);
    REQUIRE_FALSE(duel.fixture.physics->body_state(*duel.fixture.scene, old_body));
    REQUIRE_FALSE(take(duel.fixture.scene->entity_state(duel.fixture.peer_entity)).rigid_body);
    REQUIRE_FALSE(duel.fixture.scene->set_name(duel.fixture.peer_entity, Name{"Bypass"}));
    REQUIRE(std::any_of(death.movement.contacts.begin(), death.movement.contacts.end(),
                        [](const auto& event) {
                            return event.phase == PhysicsContactPhase::exit &&
                                   event.cause == PhysicsContactCause::body_removed;
                        }));
    REQUIRE(take(duel.fixture.physics->stats()).pending_events == 0);
    while (take(duel.combat->stats()).completed_ticks < 90) {
        REQUIRE_FALSE(duel.step().error);
        REQUIRE(duel.state(true).health == 0);
    }
    const auto respawn = duel.step();
    REQUIRE_FALSE(respawn.error);
    REQUIRE(respawn.respawns == 1);
    REQUIRE(respawn.created_bodies == 1);
    REQUIRE(respawn.teleported_bodies == 1);
    REQUIRE(duel.b->stream != old.stream);
    REQUIRE(duel.b->target != old.target);
    REQUIRE(duel.b->movement.stream != old.movement.stream);
    REQUIRE(duel.b->movement.last_input == 0);
    REQUIRE(duel.state(true).health == 100);
    REQUIRE(take(duel.fixture.scene->reference(duel.fixture.peer_entity)) == entity);
    REQUIRE(take(duel.fixture.scene->entity_state(duel.fixture.peer_entity))
                .local_transform.translation.x == 2);
    REQUIRE_FALSE(duel.fixture.movement->submit(
        *duel.fixture.authority, *duel.fixture.peer->server, *duel.fixture.scene,
        *duel.fixture.physics,
        {envelope(*duel.fixture.peer_session, duel.command_b), old.movement.stream, 9, 1000, 0},
        duel.fixture.now));
    auto stale = CombatAction{envelope(duel.fixture.session, duel.command_a), duel.a->stream,
                              duel.action_a, old.target, CombatActionKind::strike};
    REQUIRE_FALSE(duel.combat->submit(*duel.fixture.authority, *duel.fixture.pair.server,
                                      *duel.fixture.scene, *duel.fixture.physics, stale,
                                      duel.fixture.now));
    duel.send(CombatActionKind::strike, true);
    REQUIRE(duel.step().outcomes.front().outcome.amount == 25);
    duel.fixture.complete = true;
}

TEST_CASE("Combat retains fixed-tick effects across session fencing and a failing server rule",
          "[server-combat]") {
    SECTION("a later observation fences regressed tick time before native work") {
        Duel duel;
        const std::array<CombatTargetId, 1> targets{duel.b->target};
        take(duel.combat->view(*duel.fixture.authority, *duel.fixture.pair.server,
                               *duel.fixture.scene, *duel.fixture.physics, targets, 100ms));
        const std::array<TlsTransport*, 2> transports{duel.fixture.pair.server.get(),
                                                      duel.fixture.peer->server.get()};
        REQUIRE_FALSE(duel.combat->step(*duel.fixture.authority, *duel.fixture.movement,
                                        *duel.fixture.scene, *duel.fixture.physics, transports,
                                        50ms));
        REQUIRE(take(duel.fixture.physics->stats()).completed_ticks == 0);
        REQUIRE_FALSE(take(duel.combat->stats()).faulted);
        REQUIRE(take(duel.combat->stats()).actors == 2);
        duel.fixture.complete = true;
    }
    SECTION("revoked target cannot be hit by an already admitted attack") {
        Duel duel;
        duel.send();
        REQUIRE(take(duel.fixture.authority->revoke_session(duel.fixture.peer_session->session)));
        const auto report = duel.step();
        REQUIRE_FALSE(report.error);
        REQUIRE(report.fenced == 1);
        REQUIRE(report.outcomes.front().outcome.outcome == CombatOutcome::target_unavailable);
        REQUIRE(report.outcomes.front().outcome.amount == 0);
        REQUIRE(take(duel.combat->stats()).actors == 1);
        REQUIRE(duel.state(false, false).health == 100);
        duel.fixture.complete = true;
    }
    SECTION("a later Lua failure preserves the earlier hit and completed native tick") {
        Duel duel{false, R"(calls=0
function on_combat(kind,tick,actor,target,distance)
  calls=calls+1
  if calls==2 then error('later combat failure') end
  return true,25,10
end)"};
        duel.send();
        duel.send(CombatActionKind::strike, true);
        const auto report = duel.step();
        REQUIRE(report.error);
        REQUIRE(report.error->context.path == "scripts/combat.lua");
        REQUIRE(report.error->context.request_id == "1");
        REQUIRE(report.outcomes.size() == 1);
        REQUIRE(report.outcomes.front().outcome.amount == 25);
        REQUIRE(report.movement.completed_tick == 1);
        REQUIRE(take(duel.combat->stats()).faulted);
        REQUIRE(take(duel.combat->stats()).completed_ticks == 1);
        const std::array<TlsTransport*, 2> transports{duel.fixture.pair.server.get(),
                                                      duel.fixture.peer->server.get()};
        REQUIRE_FALSE(duel.combat->step(*duel.fixture.authority, *duel.fixture.movement,
                                        *duel.fixture.scene, *duel.fixture.physics, transports,
                                        duel.fixture.now + 50ms));
        REQUIRE(take(duel.fixture.physics->stats()).completed_ticks == 1);
        duel.fixture.complete = true;
    }
}

TEST_CASE("Combat preserves completed movement counters when its correction publication fails",
          "[server-combat]") {
    Fixture fixture{false};
    auto edge = fixture.origin;
    edge.cell.x = world_cell_max;
    edge.offset[0] = 1023.9;
    auto movement = take(CharacterMovement::create(*fixture.scene, *fixture.physics, edge));
    auto combat = take(
        CharacterCombat::create(*fixture.scene, *fixture.physics,
                                take(CombatRules::create({"scripts/combat.lua", combat_source}))));
    const auto bound = take(combat->bind(*fixture.authority, *fixture.pair.server, *movement,
                                         *fixture.scene, *fixture.physics, fixture.entity, 0ns));
    take(movement->submit(*fixture.authority, *fixture.pair.server, *fixture.scene,
                          *fixture.physics,
                          {envelope(fixture.session, 2), bound.movement.stream, 1, 1000, 0}, 0ns));
    const std::array<TlsTransport*, 1> transports{fixture.pair.server.get()};
    REQUIRE_FALSE(combat->step(*fixture.authority, *movement, *fixture.scene, *fixture.physics,
                               transports, 50ms));
    REQUIRE(take(combat->stats()).faulted);
    REQUIRE(take(combat->stats()).completed_ticks == 1);
    REQUIRE(take(fixture.physics->stats()).completed_ticks == 1);
    REQUIRE_FALSE(combat->step(*fixture.authority, *movement, *fixture.scene, *fixture.physics,
                               transports, 100ms));
    REQUIRE(take(fixture.physics->stats()).completed_ticks == 1);
    fixture.complete = true;
}
