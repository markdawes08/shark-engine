// Included by the existing actual-session fixture translation unit. This keeps
// one TLS/Store/physics harness rather than copying it for regional acceptance.
#include "shark/server/regions.hpp"

namespace {
struct RegionalFixture {
    Fixture base;
    std::unique_ptr<WorldCellResidency> content;
    std::unique_ptr<WorldTraversal> traversal;
    std::unique_ptr<RegionWorld> world;
    std::optional<RegionBinding> a;
    std::optional<RegionBinding> b;
    std::uint64_t command_b{2};
    std::uint64_t input_a{1};
    std::array<RegionDefinition, 2> regions{
        {{id<RegionId>(101), -10, .5, -10, 10, 12}, {id<RegionId>(102), .5, 10, -10, 10, 12}}};
    std::optional<StoreMetadata> durable;

    explicit RegionalFixture(bool runtime = false, bool npc = false, bool persistent = false,
                             const char* rules_source = nullptr)
        : base{false, false, character_body(), true, false, npc} {
        base.movement.reset(); // The regional host privately owns the gameplay owners.
        if (runtime) {
            durable = take(base.authority->metadata());
            take(base.authority->initialize_economy(base.record.account, base.record.character,
                                                    {100, 20}));
            const auto peer = *base.peer_session;
            take(base.authority->initialize_economy(peer.account, peer.character, {100, 20}));
            base.authority.reset();
            SessionLimits limits;
            limits.commands_per_window = 128;
            base.authority = take(CharacterAuthority::open_runtime(
                base.root / "Movement.db", durable->id, durable->world, limits));
            base.session = base.login(base.pair);
            const auto logged = take(base.authority->apply(
                *base.peer->server, LoginRequest{peer.account, peer.character}, base.now));
            base.peer_session = take(base.authority->apply(
                *base.peer->server, SetReadyRequest{envelope(logged, 1), true}, base.now));
        }
        const WorldCellGeometry geometry{{{0, 0, 0}, {0, 0, 1024}, {1024, 0, 1024}, {1024, 0, 0}},
                                         {{0, 1, 2}, {0, 2, 3}},
                                         {{0, 1, 2}, {0, 2, 3}}};
        const auto source = id<SceneId>(500);
        const auto first_cell = base.origin.cell.x;
        content = take(WorldCellResidency::create(
            [geometry, source, first_cell, npc](const WorldCellReadRequest& r,
                                                std::stop_token) -> Result<std::vector<std::byte>> {
                auto selected = geometry;
                if (npc) {
                    const double x = r.key.cell.x == first_cell ? 1020 : 0;
                    selected = {{{x, 0, 0}, {x, 0, 8}, {x + 4, 0, 0}, {x + 4, 0, 8}},
                                {{0, 1, 2}, {2, 1, 3}},
                                {{0, 1, 2}, {2, 1, 3}}};
                }
                return encode_world_cell(
                    {r.key,
                     r.content_revision,
                     {source, WorldPosition{r.key.world, r.key.cell, {0, 0, 0}}, {}},
                     selected});
            }));
        traversal =
            take(WorldTraversal::create(*content, *base.physics, *base.scene, {base.origin, 1}));
        for (int index = 0; index < 2; ++index) {
            auto cell = base.origin.cell;
            cell.x += index;
            const WorldCellKey key{base.origin.world, cell};
            const auto job = take(content->submit(key, 1, take(PreparedAssets::create())));
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            for (;;) {
                REQUIRE(std::chrono::steady_clock::now() < deadline);
                const auto jobs = take(content->jobs());
                const auto found = std::find_if(jobs.begin(), jobs.end(),
                                                [&](const auto& j) { return j.id == job; });
                REQUIRE(found != jobs.end());
                if (found->state == WorldCellJobState::ready)
                    break;
                std::this_thread::yield();
            }
            REQUIRE(take(content->drain()) == 1);
            const auto loaded = take(content->take(job));
            REQUIRE(loaded.resident_generation);
            take(traversal->activate(key, *loaded.resident_generation,
                                     id<WorldEntityId>(510 + static_cast<unsigned>(index))));
        }
        if (npc) {
            for (const auto body : {base.body, base.peer_body}) {
                auto pose = take(base.physics->body_state(*base.scene, body)).pose;
                pose.translation.y = -base.origin.offset[1] + .5;
                take(base.physics->teleport(*base.scene, body, pose));
            }
        }
        InterestLimits limits;
        limits.enter_radius = 10;
        limits.exit_radius = 12;
        if (persistent) {
            REQUIRE(runtime);
            const auto claim_a = take(base.authority->submit_region_claim(
                base.session.character, base.record.entity, regions[0].id,
                id<RegionInstanceId>(104), base.now + 10s, base.now));
            const auto claim_b = take(base.authority->submit_region_claim(
                base.peer_session->character,
                std::get<WorldEntityId>(take(base.scene->reference(base.peer_entity)).identity),
                regions[1].id, id<RegionInstanceId>(105), base.now + 10s, base.now));
            settled();
            for (const auto ticket : {claim_a, claim_b}) {
                const auto completion = take(base.authority->take_region(ticket, base.now));
                REQUIRE(completion.outcome);
                take(*completion.outcome);
            }
        }
        world = take(RegionWorld::create(
            *base.scene, *base.physics, {base.origin, 1}, *traversal,
            take(CombatRules::create(
                {"scripts/regional.lua",
                 rules_source
                     ? rules_source
                     : "function on_combat(kind,tick,actor,target,distance) "
                       "if kind==1 or kind==3 then return true,25,10 end return true,0,5 end"})),
            regions, limits, persistent));
        a = take(world->bind(*base.authority, *base.pair.server, base.entity, base.now));
        b = take(world->bind(*base.authority, *base.peer->server, base.peer_entity, base.now));
    }
    ~RegionalFixture() {
        world.reset();
        if (traversal) {
            if (!traversal->close()) {
                static_cast<void>(base.physics->close());
                if (!traversal->close())
                    std::terminate();
            }
        }
    }
    RegionGrant grant(bool second = false) const {
        const auto reference = take(base.scene->reference(second ? base.peer_entity : base.entity));
        return take(world->current_grant(std::get<WorldEntityId>(reference.identity)));
    }
    void move(std::int16_t x = 1000, std::int16_t z = 0) {
        take(world->submit_movement(
            grant(), *base.authority, *base.pair.server,
            {envelope(base.session, base.next_command), a->gameplay.movement.stream, input_a, x, z},
            base.now));
        ++base.next_command;
        ++input_a;
    }
    RegionTick tick() {
        base.now += 50ms;
        const std::array<TlsTransport*, 2> connections{base.pair.server.get(),
                                                       base.peer->server.get()};
        const auto report = take(world->step(*base.authority, connections, base.now));
        REQUIRE_FALSE(report.error);
        REQUIRE_FALSE(report.gameplay.error);
        return report;
    }
    CombatState state(bool second = false) const {
        return take(world->view(grant(second), *base.authority,
                                *(second ? base.peer->server : base.pair.server), base.now));
    }
    ReplicationUpdate interest(bool second = false) {
        auto& pair = second ? *base.peer : base.pair;
        const auto g = grant(second);
        const auto published =
            take(world->publish_interest(g, *base.authority, *pair.server, base.now));
        REQUIRE(published.status == InterestPublishStatus::sent);
        const auto frame = pair.receive(false, base.now);
        REQUIRE(frame.kind == MessageKind::replication_state);
        const auto update = take(decode_replication_update(frame.payload));
        auto& command = second ? command_b : base.next_command;
        CHECK(take(world->control_interest(
                  g, *base.authority, *pair.server,
                  ReplicationAck{envelope(second ? *base.peer_session : base.session, command++),
                                 update.stream, update.revision},
                  base.now)) == ReplicationControlResult::acknowledged);
        return update;
    }
    void settled() {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            if (take(base.authority->poll(base.now)).unsettled == 0)
                return;
            std::this_thread::yield();
        }
    }
};
} // namespace

TEST_CASE("Co-located region grants preserve sessions replicas combat and pending durable identity",
          "[server-regions]") {
    RegionalFixture f{true};
    REQUIRE(f.a->grant.region == f.regions[0].id);
    REQUIRE(f.b->grant.region == f.regions[1].id);
    const auto original = f.grant();
    const auto entity = take(f.base.scene->reference(f.base.entity));
    const auto native = f.base.body;
    const auto a_baseline = f.interest();
    const auto b_baseline = f.interest(true);
    CHECK(a_baseline.changes.size() == 2);
    CHECK(b_baseline.changes.size() == 2);
    auto replica = take(ReplicaClient::create(f.base.session));
    CHECK(take(replica->apply(a_baseline)) == ReplicaApplyResult::applied);
    const auto initial_replicas = take(replica->snapshot());
    const auto now = f.base.now;
    const auto operation = TransferRequest{{f.durable->id, f.base.record.account, 1},
                                           f.base.record.character,
                                           f.base.peer_session->character,
                                           1,
                                           1,
                                           {7, 2}};
    const auto ticket = take(f.world->submit_transfer(
        original, *f.base.authority, *f.base.pair.server,
        envelope(f.base.session, f.base.next_command++), operation, now + 10s, now));
    CHECK_FALSE(f.world->take_transfer(f.grant(true), *f.base.authority, *f.base.peer->server,
                                       ticket, now));
    take(f.world->submit_combat(original, *f.base.authority, *f.base.pair.server,
                                {envelope(f.base.session, f.base.next_command++),
                                 f.a->gameplay.stream, 1, f.b->gameplay.target,
                                 CombatActionKind::strike},
                                now));
    f.move();
    f.move(); // Admitted before ownership changes; retained inputs survive it.
    const auto first = f.tick();
    REQUIRE(first.gameplay.outcomes.size() == 1);
    CHECK(first.gameplay.outcomes.front().outcome.amount == 25);
    CHECK(f.state(true).health == 75);
    const auto cooldown = f.state().ready_tick;
    const auto crossed = f.tick();
    REQUIRE(crossed.transitions.size() == 1);
    const auto current = f.grant();
    CHECK(current.region == f.regions[1].id);
    CHECK(current.generation != original.generation);
    CHECK(crossed.transitions.front().before == original);
    CHECK(crossed.transitions.front().after == current);
    CHECK(take(f.base.scene->reference(f.base.entity)) == entity);
    CHECK(take(f.base.physics->find_body(*f.base.scene, entity)) == native);
    const auto state = f.state();
    REQUIRE(state.movement);
    CHECK(state.movement->stream == f.a->gameplay.movement.stream);
    CHECK(state.movement->last_input == 2);
    CHECK(state.stream == f.a->gameplay.stream);
    CHECK(state.ready_tick == cooldown);
    CHECK(state.session == f.base.session.session);
    CHECK(state.ownership_epoch == f.base.session.ownership_epoch);
    CHECK_FALSE(f.world->submit_movement(
        original, *f.base.authority, *f.base.pair.server,
        {envelope(f.base.session, f.base.next_command), f.a->gameplay.movement.stream, 3, 1000, 0},
        f.base.now));
    CHECK_FALSE(f.world->take_transfer(original, *f.base.authority, *f.base.pair.server, ticket,
                                       f.base.now));
    f.settled();
    const auto result = take(f.world->take_transfer(current, *f.base.authority, *f.base.pair.server,
                                                    ticket, f.base.now));
    REQUIRE(result.delivery == RuntimeDelivery::ready);
    REQUIRE(result.transfer == operation);
    REQUIRE(result.outcome);
    REQUIRE(*result.outcome);
    const auto transferred = std::get<TransferResult>(result.outcome->value());
    CHECK(transferred.receipt.status == TransferStatus::committed);
    CHECK_FALSE(transferred.replayed);
    CHECK(transferred.receipt.source->state == EconomyState{93, 18});
    CHECK(transferred.receipt.target->state == EconomyState{107, 22});
    const auto retry =
        take(f.world->submit_transfer(current, *f.base.authority, *f.base.pair.server,
                                      envelope(f.base.session, f.base.next_command++),
                                      *result.transfer, f.base.now + 10s, f.base.now));
    f.settled();
    const auto replay = take(
        f.world->take_transfer(current, *f.base.authority, *f.base.pair.server, retry, f.base.now));
    REQUIRE(replay.outcome);
    REQUIRE(*replay.outcome);
    const auto replayed = std::get<TransferResult>(replay.outcome->value());
    CHECK(replayed.replayed);
    CHECK(replayed.receipt == transferred.receipt);
    const auto delta = f.interest();
    CHECK(delta.stream == a_baseline.stream);
    CHECK_FALSE(delta.baseline);
    for (const auto& change : delta.changes)
        CHECK(change.kind == ReplicationChange::update);
    CHECK(take(replica->apply(delta)) == ReplicaApplyResult::applied);
    const auto final_replicas = take(replica->snapshot());
    REQUIRE(final_replicas.entities.size() == initial_replicas.entities.size());
    for (std::size_t i = 0; i < final_replicas.entities.size(); ++i)
        CHECK(final_replicas.entities[i].id == initial_replicas.entities[i].id);
    CHECK(take(f.base.authority->authorize_entity(*f.base.pair.server, *f.base.scene, f.base.entity,
                                                  f.base.now))
              .session == f.base.session.session);
    CHECK(take(f.world->stats()).transitions == 1);
    f.base.complete = true;
}

TEST_CASE("A projectile and its once-only outcome cross co-located ownership without rebind",
          "[server-regions]") {
    RegionalFixture f;
    const auto original = f.grant();
    auto client = take(CombatClient::create(f.base.session, f.state()));
    take(f.world->submit_combat(original, *f.base.authority, *f.base.pair.server,
                                {envelope(f.base.session, f.base.next_command++),
                                 f.a->gameplay.stream, 1, f.b->gameplay.target,
                                 CombatActionKind::projectile},
                                f.base.now));
    f.move();
    f.move();
    std::size_t effects = 0, launches = 0, impacts = 0, transitions = 0;
    for (int tick = 0; tick < 8; ++tick) {
        const auto report = f.tick();
        launches += report.gameplay.projectile_launches;
        impacts += report.gameplay.projectile_impacts;
        transitions += report.transitions.size();
        const auto state = f.state();
        const auto bytes = take(encode_combat_reply(CombatReply{state}));
        take(f.base.pair.server->send(MessageKind::combat_state, bytes));
        const auto frame = f.base.pair.receive(false, f.base.now);
        REQUIRE(frame.kind == MessageKind::combat_state);
        const auto observed = take(decode_combat_reply(frame.payload));
        const auto presentation = take(client->apply(observed));
        effects += presentation.effects.size();
        CHECK(take(client->apply(observed)).effects.empty());
        CHECK(state.stream == f.a->gameplay.stream);
        CHECK(state.session == f.base.session.session);
    }
    CHECK(launches == 1);
    CHECK(impacts == 1);
    CHECK(effects == 1);
    CHECK(transitions == 1);
    CHECK(f.grant().region == f.regions[1].id);
    CHECK(f.state(true).health == 75);
    CHECK_FALSE(
        f.world->submit_combat(original, *f.base.authority, *f.base.pair.server,
                               {envelope(f.base.session, f.base.next_command), f.a->gameplay.stream,
                                2, f.b->gameplay.target, CombatActionKind::strike},
                               f.base.now));
    f.base.complete = true;
}

TEST_CASE("Region coverage rejects invalid layouts and stops movement across an unowned gap",
          "[server-regions]") {
    Fixture f{false};
    const std::array<MovementDomain, 2> domain{{{-1, 0, -1, 1}, {0, 1, -1, 0}}};
    auto pose = take(f.physics->body_state(*f.scene, f.body)).pose;
    pose.translation = {-.1, 0, .2};
    take(f.physics->teleport(*f.scene, f.body, pose));
    f.movement =
        take(CharacterMovement::create(*f.scene, *f.physics, f.origin, std::nullopt, domain));
    f.initial =
        take(f.movement->bind(*f.authority, *f.pair.server, *f.scene, *f.physics, f.entity, f.now));
    f.submit(707, -707);
    const auto stopped = f.step();
    REQUIRE(stopped.deliveries.size() == 1);
    CHECK(stopped.deliveries.front().state.last_input == 1);
    CHECK(stopped.deliveries.front().state.position.x == Catch::Approx(-.1).margin(.000001));
    CHECK(stopped.deliveries.front().state.position.z == Catch::Approx(.2).margin(.000001));
    CHECK_FALSE(CharacterMovement::create(*f.scene, *f.physics, f.origin, std::nullopt,
                                          std::array{MovementDomain{1, 0, 0, 1}}));
    f.complete = true;
    RegionalFixture regions;
    InterestLimits limits;
    limits.enter_radius = 10;
    limits.exit_radius = 12;
    auto invalid = regions.regions;
    invalid[1].min_x = 0;
    CHECK_FALSE(RegionWorld::create(
        *regions.base.scene, *regions.base.physics, {regions.base.origin, 1}, *regions.traversal,
        take(CombatRules::create({"rules.lua", combat_source})), invalid, limits));
    invalid = regions.regions;
    invalid[1].min_x = 1;
    CHECK_FALSE(RegionWorld::create(
        *regions.base.scene, *regions.base.physics, {regions.base.origin, 1}, *regions.traversal,
        take(CombatRules::create({"rules.lua", combat_source})), invalid, limits));
    invalid = regions.regions;
    invalid[1].id = invalid[0].id;
    CHECK_FALSE(RegionWorld::create(
        *regions.base.scene, *regions.base.physics, {regions.base.origin, 1}, *regions.traversal,
        take(CombatRules::create({"rules.lua", combat_source})), invalid, limits));
    regions.base.complete = true;
}

TEST_CASE("Native NPC pursuit changes regional writer while preserving its body and public replica",
          "[server-regions]") {
    RegionalFixture f{false, true};
    const auto original = take(f.world->bind_npc(f.base.npc_entity));
    CHECK(original.region == f.regions[0].id);
    CHECK_FALSE(f.world->bind_npc(f.base.npc_entity));
    CHECK_FALSE(f.world->submit_npc_velocity(f.grant(), {1, 0, 0}));
    CHECK_FALSE(f.world->submit_movement(
        original, *f.base.authority, *f.base.pair.server,
        {envelope(f.base.session, f.base.next_command), f.a->gameplay.movement.stream, 1, 1000, 0},
        f.base.now));
    const auto reference = take(f.base.scene->reference(f.base.npc_entity));
    const auto baseline = f.interest();
    REQUIRE(baseline.changes.size() == 3);
    auto replicas = take(ReplicaClient::create(f.base.session));
    take(replicas->apply(baseline));
    const auto before = take(replicas->snapshot());
    const auto start = take(world_position_from_local(f.base.origin, {-2, -100, 4}, 8192));
    const auto goal = take(world_position_from_local(f.base.origin, {2, -100, 4}, 8192));
    const auto route = take(f.traversal->path(start, goal));
    auto cursor = take(f.traversal->begin(route));
    std::size_t transitions = 0, contacts = 0;
    for (int tick = 0; tick < 70; ++tick) {
        const auto offered = take(f.traversal->advance(route, cursor, .1));
        const auto target = take(world_position_relative_to(offered.position, f.base.origin, 8192));
        const auto current = take(f.base.physics->body_state(*f.base.scene, f.base.npc_body));
        const auto grant = take(f.world->current_grant(id<WorldEntityId>(40)));
        take(f.world->submit_npc_velocity(grant, {(target[0] - current.pose.translation.x) * 20, 0,
                                                  (target[2] - current.pose.translation.z) * 20}));
        CHECK_FALSE(f.world->submit_npc_velocity(grant, {}));
        const auto report = f.tick();
        CHECK(report.npc_velocity_writes == 1);
        transitions += report.transitions.size();
        for (const auto& event : report.gameplay.movement.contacts)
            if (event.first.body == f.base.npc_body || event.second.body == f.base.npc_body)
                ++contacts;
        const auto actual = take(f.base.physics->body_state(*f.base.scene, f.base.npc_body));
        CHECK(std::abs(actual.pose.translation.x - target[0]) < .02);
        CHECK(std::abs(actual.pose.translation.z - target[2]) < .02);
        CHECK(std::abs(actual.pose.translation.y + f.base.origin.offset[1] - .5) < .04);
        cursor = offered; // Adopt only after the committed native move agrees.
    }
    const auto current = take(f.world->current_grant(id<WorldEntityId>(40)));
    CHECK(current.region == f.regions[1].id);
    CHECK(current.generation != original.generation);
    CHECK(transitions == 1);
    CHECK(contacts > 0);
    CHECK_FALSE(f.world->submit_npc_velocity(original, {}));
    CHECK(take(f.base.physics->find_body(*f.base.scene, reference)) == f.base.npc_body);
    CHECK(take(f.world->npcs()).size() == 1);
    const auto final = take(f.base.physics->body_state(*f.base.scene, f.base.npc_body));
    CHECK(std::abs(final.pose.translation.x - 2) < .02);
    const auto delta = f.interest();
    CHECK(delta.stream == baseline.stream);
    CHECK_FALSE(delta.baseline);
    for (const auto& change : delta.changes)
        CHECK(change.kind == ReplicationChange::update);
    take(replicas->apply(delta));
    const auto after = take(replicas->snapshot());
    REQUIRE(after.entities.size() == before.entities.size());
    for (std::size_t i = 0; i < after.entities.size(); ++i)
        CHECK(after.entities[i].id == before.entities[i].id);
    f.base.complete = true;
}

TEST_CASE("Regional NPC native writes retain partial effects when gameplay preflight rejects",
          "[server-regions]") {
    RegionalFixture f{false, true};
    const auto grant = take(f.world->bind_npc(f.base.npc_entity));
    take(f.world->submit_npc_velocity(grant, {2, 0, 0}));
    auto scripts = take(ScriptRuntime::create(*f.base.scene, {}, {ScriptProfile::server_gameplay}));
    const std::array<TlsTransport*, 2> transports{f.base.pair.server.get(),
                                                  f.base.peer->server.get()};
    const auto report = take(f.world->step(*f.base.authority, transports, 50ms, scripts.get()));
    REQUIRE(report.error);
    CHECK(report.error->context.operation == "server.combat.scripts");
    CHECK(report.npc_velocity_writes == 1);
    CHECK_FALSE(report.gameplay.movement_completed);
    CHECK(take(f.base.physics->stats()).completed_ticks == 0);
    CHECK(take(f.base.physics->velocity(*f.base.scene, f.base.npc_body)).linear.x == 2);
    CHECK(take(f.world->stats()).faulted);
    CHECK_FALSE(f.world->step(*f.base.authority, transports, 50ms));
    f.base.complete = true;
}
