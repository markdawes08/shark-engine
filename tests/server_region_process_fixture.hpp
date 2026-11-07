#pragma once

#include "server_gameplay_fixture.hpp"

// Real owned Server children; reuses the actual mTLS/Store fixture in this TU.
#include "region_process_test_hooks.hpp"
#include "shark/server/region_executor.hpp"
#include "shark/server/region_gateway.hpp"

namespace {
const CombatActorCut& process_actor(const RegionProcessImage& image, WorldEntityId entity) {
    const auto found =
        std::find_if(image.combat.actors.begin(), image.combat.actors.end(), [&](const auto& row) {
            return row.entity.identity == EntityIdentity{entity};
        });
    REQUIRE(found != image.combat.actors.end());
    return *found;
}
const MovementActorCut& process_mover(const RegionProcessImage& image, WorldEntityId entity) {
    const auto found = std::find_if(
        image.movement.actors.begin(), image.movement.actors.end(),
        [&](const auto& row) { return row.entity.identity == EntityIdentity{entity}; });
    REQUIRE(found != image.movement.actors.end());
    return *found;
}
const PhysicsTransferBody& process_body(const RegionProcessImage& image, WorldEntityId entity) {
    const auto found = std::find_if(
        image.physics.bodies.begin(), image.physics.bodies.end(),
        [&](const auto& row) { return row.native.entity.identity == EntityIdentity{entity}; });
    REQUIRE(found != image.physics.bodies.end());
    return *found;
}
struct ProcessFixtureOptions {
    std::optional<WorldLocalFrame> frame;
    std::vector<RegionDefinition> regions;
    std::vector<RegionImageCell> cells;
    std::optional<math::Vec3> player_a, player_b, npc;
    WorldCellResidencyLimits residency_limits{};
};
struct ProcessFixture {
    Fixture base{false, false, character_body(), true, false, true};
    RegionInstanceId initial{id<RegionInstanceId>(800)};
    std::unique_ptr<RegionGateway> gateway;
    std::uint64_t input_a{1}, action_a{1}, command_b{2}, action_b{1};
    WorldEntityId a, b;
    fs::path journal;
    explicit ProcessFixture(bool force = false, const char* source = nullptr,
                            const ProcessFixtureOptions* options = nullptr)
        : base(false, false, character_body(), true, false, true,
               options && options->frame ? std::optional<WorldPosition>{options->frame->origin}
                                         : std::nullopt),
          a{base.record.entity},
          b{std::get<WorldEntityId>(take(base.scene->reference(base.peer_entity)).identity)},
          journal{base.root / "region-journal"} {
        const auto metadata = take(base.authority->metadata());
        take(base.authority->initialize_economy(base.record.account, base.record.character,
                                                {100, 20}));
        take(base.authority->initialize_economy(base.peer_session->account,
                                                base.peer_session->character, {100, 20}));
        const auto old_peer = *base.peer_session;
        base.authority.reset();
        SessionLimits limits;
        limits.commands_per_window = 256;
        base.authority = take(CharacterAuthority::open_runtime(
            base.root / "Movement.db", metadata.id, metadata.world, limits));
        base.session = base.login(base.pair);
        const auto logged = take(base.authority->apply(
            *base.peer->server, LoginRequest{old_peer.account, old_peer.character}, base.now));
        base.peer_session = take(base.authority->apply(
            *base.peer->server, SetReadyRequest{envelope(logged, 1), true}, base.now));
        const auto initial_region = [&](const std::optional<math::Vec3>& point, RegionId fallback) {
            if (options && point) {
                for (const auto& region : options->regions)
                    if (point->x >= region.min_x && point->x < region.max_x &&
                        point->z >= region.min_z && point->z < region.max_z)
                        return region.id;
                FAIL("Fixture player starts outside its declared logical regions");
            }
            return fallback;
        };
        const auto claim_a = take(base.authority->submit_region_claim(
            base.session.character, a,
            initial_region(options ? options->player_a : std::nullopt, id<RegionId>(101)), initial,
            base.now + 10s, base.now));
        const auto claim_b = take(base.authority->submit_region_claim(
            base.peer_session->character, b,
            initial_region(options ? options->player_b : std::nullopt, id<RegionId>(102)), initial,
            base.now + 10s, base.now));
        settled();
        std::vector<RegionWriter> writers;
        for (const auto ticket : {claim_a, claim_b}) {
            const auto completion = take(base.authority->take_region(ticket, base.now));
            REQUIRE(completion.outcome);
            const auto value = take(*completion.outcome);
            writers.push_back(std::get<RegionOwnership>(value).writer);
        }
        auto rules = take(CombatRules::create_portable(
            {"scripts/process.lua",
             source ? source
                    : "function on_combat(k,t,a,b,d,s) s[1]=s[1]+1 return true,10+s[1],10 end"}));
        const auto scene_stats = take(base.scene->stats());
        const auto source_scope = take(base.scene->reference(base.entity)).scope;
        const auto native = take(base.physics->capture_transfer(*base.scene));
        const CombatCut empty_combat{{scene_stats.instance, scene_stats.revision},
                                     source_scope,
                                     native.world,
                                     0,
                                     MonotonicTime{},
                                     0,
                                     false,
                                     {},
                                     CombatRulesStats{0, false},
                                     {},
                                     {},
                                     {}};
        RegionProcessImage seeds{
            {base.origin, 1},
            {{id<RegionId>(101), -10, .5, -10, 10, 12}, {id<RegionId>(102), .5, 10, -10, 10, 12}},
            {},
            native,
            take(base.movement->capture_cut(*base.scene, *base.physics)),
            empty_combat,
            take(rules->capture_portable()),
            {},
            {},
            {},
            {},
            writers,
            0,
            initial};
        seeds.physics.config.gravity = {0, -9.81, 0};
        if (options) {
            if (options->frame)
                seeds.frame = *options->frame;
            if (!options->regions.empty())
                seeds.regions = options->regions;
            seeds.residency_limits = options->residency_limits;
        }
        for (const auto entity : {base.entity, base.peer_entity, base.npc_entity}) {
            const auto state = take(base.scene->entity_state(entity));
            auto transform = state.local_transform;
            transform.translation.y = -base.origin.offset[1] + .5;
            if (options) {
                const auto& position =
                    entity == base.entity
                        ? options->player_a
                        : (entity == base.peer_entity ? options->player_b : options->npc);
                if (position)
                    transform.translation = *position;
            }
            seeds.entities.push_back({state.reference,
                                      state.name,
                                      transform,
                                      {},
                                      state.camera,
                                      state.mesh_renderer,
                                      state.rigid_body});
        }
        const WorldCellGeometry geometry{{{0, 0, 0}, {0, 0, 1024}, {1024, 0, 1024}, {1024, 0, 0}},
                                         {{0, 1, 2}, {0, 2, 3}},
                                         {{0, 1, 2}, {0, 2, 3}}};
        for (int n = 0; n < 2; ++n) {
            auto cell = base.origin.cell;
            cell.x += n;
            const WorldCellKey key{base.origin.world, cell};
            seeds.cells.push_back(
                {key, 1, id<WorldEntityId>(810 + static_cast<unsigned>(n)),
                 take(encode_world_cell(
                     {key,
                      1,
                      {id<SceneId>(812), WorldPosition{key.world, key.cell, {0, 0, 0}}, {}},
                      geometry}))});
        }
        seeds.npcs.push_back({take(base.scene->reference(base.npc_entity)), base.npc_body,
                              force ? std::optional<math::Vec3>{{2, 0, 0}} : std::nullopt});
        if (options && !options->cells.empty())
            seeds.cells = options->cells;
        const std::array<TlsTransport*, 2> transports{base.pair.server.get(),
                                                      base.peer->server.get()};
        for (auto* transport : transports) {
            const auto entity = transport == base.pair.server.get() ? a : b;
            const auto handle = take(base.scene->find(entity));
            seeds.peers.push_back(
                {LocalGameplayAccess::connection_id(*transport),
                 take(base.authority->authorize_entity(*transport, *base.scene, handle, base.now)),
                 entity});
        }
        LocalGameplayAccess access{*base.authority, transports};
        auto bootstrap = take(RegionExecutor::bootstrap(seeds, initial, access, base.now));
        auto image = take(bootstrap->capture());
        bootstrap.reset(); // No gateway native simulation runs after this point.
        base.movement.reset();
        base.physics.reset();
        if (force) {
            auto body = std::find_if(
                image.physics.bodies.begin(), image.physics.bodies.end(), [&](const auto& row) {
                    return row.native.entity.identity == EntityIdentity{id<WorldEntityId>(40)};
                });
            REQUIRE(body != image.physics.bodies.end());
            body->pending_force = {20, 0, 0};
        }
        REQUIRE(fs::create_directory(journal));
        gateway = take(RegionGateway::create(*base.authority, fs::path{SHARK_REGION_SERVER},
                                             journal, image, transports, base.now));
    }
    ~ProcessFixture() {
        shark::server::detail::clear_region_control_fault();
        gateway.reset();
        if (base.complete) {
            std::error_code ignored;
            for (const auto* file :
                 {"checkpoint.sharkregion", "staged.sharkregion", "checkpoint.sharkregion.partial",
                  "staged.sharkregion.partial", ".shark-region-journal.lock"})
                fs::remove(journal / file, ignored);
            fs::remove(journal, ignored);
        }
    }
    void settled() {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (take(base.authority->poll(base.now)).unsettled != 0) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
    }
    void finish() {
        take(gateway->close(base.now));
        base.complete = true;
    }
    std::array<TlsTransport*, 2> live() const {
        return {base.pair.server.get(), base.peer->server.get()};
    }
    RegionProcessImage image() const {
        return take(gateway->snapshot());
    }
    RegionProcessImage tick() {
        base.now += 50ms;
        return take(gateway->step(live(), base.now));
    }
    RegionRouteReceipt move(std::int16_t x = 1000) {
        const auto current = image();
        const auto& mover = process_mover(current, a);
        const auto receipt =
            take(gateway->submit(*base.pair.server,
                                 MovementInput{envelope(base.session, base.next_command++),
                                               mover.stream, input_a++, x, 0},
                                 base.now));
        return receipt;
    }
    RegionRouteReceipt combat(bool second, CombatActionKind kind) {
        const auto current = image();
        const auto& actor = process_actor(current, second ? b : a);
        const auto& target = process_actor(current, second ? a : b);
        auto& command = second ? command_b : base.next_command;
        auto& action = second ? action_b : action_a;
        return take(gateway->submit(
            *(second ? base.peer->server : base.pair.server),
            CombatAction{envelope(second ? *base.peer_session : base.session, command++),
                         actor.stream, action++, target.target, kind},
            base.now));
    }
    void commit() {
        take(gateway->begin_commit(base.now + 10s, base.now));
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (take(gateway->poll_commit(base.now)) == RegionGatewayPhase::committing) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        CHECK(take(gateway->stats()).phase == RegionGatewayPhase::committed);
    }
    void recover(unsigned instance) {
        take(gateway->prepare_recovery(id<RegionInstanceId>(instance), live(), base.now));
        commit();
        take(gateway->activate(base.now));
    }
    ReplicationUpdate interest() {
        const auto reply = take(gateway->publish_interest(*base.pair.server, base.now));
        REQUIRE(reply.status == InterestPublishStatus::sent);
        const auto frame = base.pair.receive(false, base.now);
        REQUIRE(frame.kind == MessageKind::replication_state);
        const auto update = take(decode_replication_update(frame.payload));
        take(gateway->control_interest(*base.pair.server,
                                       ReplicationAck{envelope(base.session, base.next_command++),
                                                      update.stream, update.revision},
                                       base.now));
        return update;
    }
};
} // namespace
