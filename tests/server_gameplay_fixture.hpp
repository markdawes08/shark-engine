#pragma once

#include <shark/net/combat_client.hpp>
#include <shark/net/fault_link.hpp>
#include <shark/net/movement_prediction.hpp>
#include <shark/server/combat.hpp>
#include <shark/server/movement.hpp>
#include <shark/world_traversal.hpp>

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
        UNSCOPED_INFO(result.error().context.operation << ": " << result.error().message);
    }
    REQUIRE(result);
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result) {
        UNSCOPED_INFO(result.error().context.operation << ": " << result.error().message);
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
    EntityHandle npc_entity{};
    PhysicsBodyHandle npc_body{};

    explicit Fixture(bool bind = true, bool wall = false,
                     const RigidBody& settings = character_body(), bool duel = false,
                     bool scripted = false, bool npc = false,
                     std::optional<WorldPosition> initial_origin = {}) {
        if (initial_origin)
            origin = *initial_origin;
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
        if (scripted)
            seed.script = Script{"scripts/actor.lua"};
        entity = take(scene->create_entity(seed));
        auto native_profile = profile();
        if (npc)
            native_profile.gravity = {0, -9.81, 0};
        physics = take(PhysicsWorld::create(native_profile));
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
            if (scripted)
                opponent.script = Script{"scripts/actor.lua"};
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
        if (npc) {
            EntitySeed npc_seed{id<WorldEntityId>(40)};
            npc_seed.name = Name{"Regional NPC"};
            npc_seed.transform.translation = {-2, -origin.offset[1] + .5, 4};
            npc_seed.rigid_body = character_body();
            npc_entity = take(scene->create_entity(npc_seed));
            npc_body = take(physics->create_body(*scene, npc_entity));
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
    std::unique_ptr<ScriptRuntime> scripts;
    explicit Duel(bool wall = false, const char* source = combat_source, bool scripted = false,
                  const char* entity_source = nullptr)
        : fixture(false, wall, character_body(), true, scripted) {
        combat = take(
            CharacterCombat::create(*fixture.scene, *fixture.physics,
                                    take(CombatRules::create({"scripts/combat.lua", source}))));
        a = take(combat->bind(*fixture.authority, *fixture.pair.server, *fixture.movement,
                              *fixture.scene, *fixture.physics, fixture.entity, fixture.now));
        b = take(combat->bind(*fixture.authority, *fixture.peer->server, *fixture.movement,
                              *fixture.scene, *fixture.physics, fixture.peer_entity, fixture.now));
        if (scripted) {
            const std::array<ScriptSourceSnapshot, 1> sources{
                {{"scripts/actor.lua", entity_source ? entity_source : R"(
local age=0
function on_init() shark.log.info('init') end
function on_fixed_update() age=age+1; if age==1 then shark.log.info('first') end end
function on_contact_enter() shark.log.info('enter') end
function on_destroy() shark.log.info('destroy') end
)"}}};
            scripts = take(ScriptRuntime::create(
                *fixture.scene, sources, {ScriptProfile::server_gameplay, 2, 1024 * 1024, 100000}));
            take(scripts->start(*fixture.scene, *fixture.physics));
        }
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
                                        *fixture.physics, transports, fixture.now, scripts.get()));
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
    CombatReconnectReport replace(bool second = false) {
        auto replacement = std::make_unique<Pair>(second);
        const auto original = second ? *fixture.peer_session : fixture.session;
        const auto login = take(fixture.authority->apply(
            *replacement->server, LoginRequest{original.account, original.character}, fixture.now));
        const auto ready = take(fixture.authority->apply(
            *replacement->server, SetReadyRequest{envelope(login, 1), true}, fixture.now));
        const auto detached = step();
        REQUIRE_FALSE(detached.error);
        REQUIRE(detached.fenced == 1);
        auto report = take(combat->reconnect(*fixture.authority, *replacement->server,
                                             *fixture.movement, *fixture.scene, *fixture.physics,
                                             second ? fixture.peer_entity : fixture.entity,
                                             fixture.now, scripts.get()));
        REQUIRE_FALSE(report.error);
        REQUIRE(report.state);
        auto& binding = second ? *b : *a;
        binding.stream = report.state->stream;
        binding.target = report.state->self;
        if (report.state->movement)
            binding.movement = *report.state->movement;
        if (second) {
            fixture.peer = std::move(replacement);
            fixture.peer_session = ready;
            command_b = 2;
            action_b = 1;
        } else {
            fixture.pair = std::move(*replacement);
            fixture.session = ready;
            command_a = 2;
            action_a = 1;
        }
        return report;
    }
};
} // namespace
