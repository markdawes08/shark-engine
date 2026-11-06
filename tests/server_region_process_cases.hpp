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
struct ProcessFixture {
    Fixture base{false, false, character_body(), true, false, true};
    RegionInstanceId initial{id<RegionInstanceId>(800)};
    std::unique_ptr<RegionGateway> gateway;
    std::uint64_t input_a{1}, action_a{1}, command_b{2}, action_b{1};
    WorldEntityId a, b;
    fs::path journal;
    explicit ProcessFixture(bool force = false, const char* source = nullptr)
        : a{base.record.entity},
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
        const auto claim_a = take(base.authority->submit_region_claim(
            base.session.character, a, id<RegionId>(101), initial, base.now + 10s, base.now));
        const auto claim_b = take(base.authority->submit_region_claim(
            base.peer_session->character, b, id<RegionId>(102), initial, base.now + 10s, base.now));
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
        for (const auto entity : {base.entity, base.peer_entity, base.npc_entity}) {
            const auto state = take(base.scene->entity_state(entity));
            auto transform = state.local_transform;
            transform.translation.y = -base.origin.offset[1] + .5;
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

TEST_CASE(
    "Separate native region processes retain admitted queues flights state and recipient replicas",
    "[server-region-process]") {
    ProcessFixture f;
    const auto initial = f.image();
    const auto first_pid = take(f.gateway->stats()).source_pid;
    const auto baseline = f.interest();
    const auto metadata = take(f.base.authority->metadata());
    const TransferRequest original{{metadata.id, f.base.record.account, 1},
                                   f.base.record.character,
                                   f.base.peer_session->character,
                                   1,
                                   1,
                                   {7, 2}};
    const auto economic = take(f.gateway->submit_transfer(
        *f.base.pair.server, envelope(f.base.session, f.base.next_command++), original,
        f.base.now + 10s, f.base.now));
    auto replicas = take(ReplicaClient::create(f.base.session));
    take(replicas->apply(baseline));
    const auto ids = take(replicas->snapshot());
    CHECK(f.move().status == RegionRouteStatus::admitted);
    CHECK(f.move().status == RegionRouteStatus::admitted);
    CHECK(f.combat(false, CombatActionKind::projectile).status == RegionRouteStatus::admitted);
    CHECK(f.combat(true, CombatActionKind::projectile).status == RegionRouteStatus::admitted);
    f.tick();
    f.tick();
    f.move();
    f.move();
    const auto cut = f.image();
    REQUIRE(process_mover(cut, f.a).pending.size() == 2);
    REQUIRE(process_actor(cut, f.a).projectile);
    REQUIRE(process_actor(cut, f.b).projectile);
    CHECK(cut.rules.state[0] == 2);
    CHECK(cut.physics.active_pairs.size() >= 2);
    take(f.gateway->prepare_handoff(id<RegionInstanceId>(820), f.live(), f.base.now));
    const auto prepared = take(f.gateway->stats());
    CHECK(prepared.source_pid == first_pid);
    CHECK(prepared.destination_pid != 0);
    CHECK(prepared.destination_pid != first_pid);
    CHECK(f.move(0).status == RegionRouteStatus::buffered);
    CHECK(f.move(0).status == RegionRouteStatus::buffered);
    f.commit();
    take(f.gateway->activate(f.base.now));
    const auto installed = f.image();
    CHECK(installed.physics.completed_tick == cut.physics.completed_tick);
    for (const auto& writer : installed.writers) {
        CHECK(writer.instance == id<RegionInstanceId>(820));
        CHECK(writer.epoch == 2);
        CHECK(writer.region == id<RegionId>(102));
    }
    CHECK(installed.rules.state == cut.rules.state);
    CHECK(process_actor(installed, f.a).stream == process_actor(initial, f.a).stream);
    CHECK(process_actor(installed, f.a).target == process_actor(initial, f.a).target);
    CHECK(process_mover(installed, f.a).stream == process_mover(initial, f.a).stream);
    CHECK(process_actor(installed, f.a).projectile->request ==
          process_actor(cut, f.a).projectile->request);
    CHECK(process_actor(installed, f.b).projectile->decision ==
          process_actor(cut, f.b).projectile->decision);
    CHECK(installed.physics.next_contact_sequence == cut.physics.next_contact_sequence);
    CHECK_FALSE(f.gateway->take_transfer(*f.base.peer->server, economic, f.base.now));
    f.settled();
    const auto durable = take(f.gateway->take_transfer(*f.base.pair.server, economic, f.base.now));
    CHECK(durable.ticket == economic);
    CHECK(durable.transfer == original);
    CHECK(durable.writer == initial.writers.front());
    REQUIRE(durable.outcome);
    const auto value = take(*durable.outcome);
    const auto& balances = std::get<TransferResult>(value).receipt;
    REQUIRE(balances.source);
    REQUIRE(balances.target);
    CHECK(balances.source->state == EconomyState{93, 18});
    CHECK(balances.target->state == EconomyState{107, 22});
    const auto drained = take(f.gateway->drain_routes(f.live(), f.base.now));
    REQUIRE(drained.size() == 2);
    for (const auto& receipt : drained)
        CHECK(receipt.status == RegionRouteStatus::admitted);
    CHECK(process_mover(f.image(), f.a).pending.size() == 4);
    CHECK(process_mover(f.tick(), f.a).applied == 3);
    for (int n = 0; n < 8; ++n)
        f.tick();
    const auto finished = f.image();
    CHECK(process_actor(finished, f.a).health == 88);
    CHECK(process_actor(finished, f.b).health == 89);
    CHECK_FALSE(process_actor(finished, f.a).projectile);
    CHECK_FALSE(process_actor(finished, f.b).projectile);
    CHECK(f.combat(false, CombatActionKind::strike).status == RegionRouteStatus::admitted);
    const auto hit = f.tick();
    CHECK(hit.rules.state[0] == 3);
    CHECK(process_actor(hit, f.b).health == 76);
    const auto delta = f.interest();
    CHECK(delta.stream == baseline.stream);
    for (const auto& change : delta.changes)
        CHECK(change.kind == ReplicationChange::update);
    take(replicas->apply(delta));
    const auto after = take(replicas->snapshot());
    REQUIRE(after.entities.size() == ids.entities.size());
    for (std::size_t n = 0; n < after.entities.size(); ++n)
        CHECK(after.entities[n].id == ids.entities[n].id);
    CHECK(take(f.gateway->stats()).source_pid != first_pid);
    CHECK(take(f.base.authority->authorize_entity(*f.base.pair.server, *f.base.scene, f.base.entity,
                                                  f.base.now))
              .session == f.base.session.session);
    f.finish();
}

TEST_CASE("A queued native force and NPC intent survive actual process staging exactly once",
          "[server-region-process]") {
    ProcessFixture f{true};
    const auto npc = id<WorldEntityId>(40);
    const auto cut = f.image();
    CHECK(process_body(cut, npc).pending_force == math::Vec3{20, 0, 0});
    REQUIRE(cut.npcs.front().pending);
    take(f.gateway->prepare_handoff(id<RegionInstanceId>(821), f.live(), f.base.now));
    f.commit();
    take(f.gateway->activate(f.base.now));
    CHECK(process_body(f.image(), npc).pending_force == math::Vec3{20, 0, 0});
    const auto first = f.tick();
    CHECK(process_body(first, npc).pending_force == math::Vec3{});
    CHECK(process_body(first, npc).velocity.linear.x > 2);
    CHECK_FALSE(first.npcs.front().pending);
    const auto second = f.tick();
    CHECK(process_body(second, npc).velocity.linear.x == Catch::Approx(0).margin(.0001));
    f.finish();
}

TEST_CASE("Post-cut buffering is bounded and consumes no command until the new executor admits it",
          "[server-region-process]") {
    ProcessFixture f;
    const auto before = f.base.next_command;
    take(f.gateway->prepare_handoff(id<RegionInstanceId>(822), f.live(), f.base.now));
    for (std::size_t n = 0; n < region_gateway_maximum_routes; ++n)
        CHECK(f.move(0).status == RegionRouteStatus::buffered);
    const auto snapshot = f.image();
    CHECK_FALSE(
        f.gateway->submit(*f.base.pair.server,
                          MovementInput{envelope(f.base.session, f.base.next_command),
                                        process_mover(snapshot, f.a).stream, f.input_a, 0, 0},
                          f.base.now));
    CHECK(take(f.base.authority->authorize_entity(*f.base.pair.server, *f.base.scene, f.base.entity,
                                                  f.base.now))
              .last_sequence == before - 1);
    f.commit();
    take(f.gateway->activate(f.base.now));
    const auto receipts = take(f.gateway->drain_routes(f.live(), f.base.now));
    REQUIRE(receipts.size() == region_gateway_maximum_routes);
    for (const auto& receipt : receipts)
        CHECK(receipt.status == RegionRouteStatus::admitted);
    CHECK(process_mover(f.image(), f.a).pending.size() == movement_pending_inputs);
    CHECK(take(f.base.authority->authorize_entity(*f.base.pair.server, *f.base.scene, f.base.entity,
                                                  f.base.now))
              .last_sequence == before + region_gateway_maximum_routes - 1);
    f.finish();
}

TEST_CASE("Confirmed source or destination loss restores a fenced actual child before and after "
          "ownership commit",
          "[server-region-process]") {
    ProcessFixture f;
    const auto original_image = f.image();
    const TransferRequest original{
        {original_image.writers.front().store, f.base.session.account, 1},
        f.base.session.character,
        f.base.peer_session->character,
        1,
        1,
        {7, 2}};
    const auto economic = take(f.gateway->submit_transfer(
        *f.base.pair.server, envelope(f.base.session, f.base.next_command++), original,
        f.base.now + 10s, f.base.now));
    f.move(0);
    f.tick();
    const auto cut = f.image();
    SECTION("source lost before prepare") {
        take(f.gateway->terminate_executor(RegionProcessRole::source));
    }
    SECTION("destination lost before SQL") {
        take(f.gateway->prepare_handoff(id<RegionInstanceId>(823), f.live(), f.base.now));
        take(f.gateway->terminate_executor(RegionProcessRole::destination));
    }
    SECTION("destination lost after SQL") {
        take(f.gateway->prepare_handoff(id<RegionInstanceId>(823), f.live(), f.base.now));
        f.commit();
        take(f.gateway->terminate_executor(RegionProcessRole::destination));
    }
    f.recover(824);
    CHECK(f.image().physics.completed_tick == cut.physics.completed_tick);
    CHECK(process_actor(f.image(), f.a).stream == process_actor(cut, f.a).stream);
    CHECK(process_mover(f.image(), f.a).applied == 1);
    CHECK(process_mover(f.tick(), f.a).applied == 1);
    f.settled();
    const auto completion =
        take(f.gateway->take_transfer(*f.base.pair.server, economic, f.base.now));
    CHECK(completion.ticket == economic);
    CHECK(completion.transfer == original);
    CHECK(completion.writer == original_image.writers.front());
    REQUIRE(completion.outcome);
    const auto outcome = take(*completion.outcome);
    const auto& receipt = std::get<TransferResult>(outcome).receipt;
    REQUIRE(receipt.source);
    REQUIRE(receipt.target);
    CHECK(receipt.source->state == EconomyState{93, 18});
    CHECK(receipt.target->state == EconomyState{107, 22});
    f.finish();
}

TEST_CASE("Lost real worker replies require hard fencing and never repeat shared session admission",
          "[server-region-process]") {
    ProcessFixture f;
    const auto first_pid = take(f.gateway->stats()).source_pid;
    SECTION("freeze request partitioned before processing") {
        shark::server::detail::set_region_control_fault(
            shark::server::detail::RegionControlPoint::freeze,
            shark::server::detail::RegionControlFault::drop_request);
        CHECK_FALSE(f.gateway->prepare_handoff(id<RegionInstanceId>(825), f.live(), f.base.now));
    }
    SECTION("freeze reply lost before SQL") {
        shark::server::detail::set_region_control_fault(
            shark::server::detail::RegionControlPoint::freeze,
            shark::server::detail::RegionControlFault::drop_reply);
        CHECK_FALSE(f.gateway->prepare_handoff(id<RegionInstanceId>(825), f.live(), f.base.now));
    }
    SECTION("activation reply lost after SQL") {
        take(f.gateway->prepare_handoff(id<RegionInstanceId>(825), f.live(), f.base.now));
        f.commit();
        shark::server::detail::set_region_control_fault(
            shark::server::detail::RegionControlPoint::activate,
            shark::server::detail::RegionControlFault::drop_reply);
        CHECK_FALSE(f.gateway->activate(f.base.now));
    }
    SECTION("admitted movement commit reply lost") {
        shark::server::detail::set_region_control_fault(
            shark::server::detail::RegionControlPoint::movement_commit,
            shark::server::detail::RegionControlFault::drop_reply);
        CHECK(f.move(0).status == RegionRouteStatus::uncertain);
        CHECK(take(f.base.authority->authorize_entity(*f.base.pair.server, *f.base.scene,
                                                      f.base.entity, f.base.now))
                  .last_sequence == 2);
    }
    CHECK(take(f.gateway->stats()).phase == RegionGatewayPhase::recovery_required);
    f.recover(826);
    CHECK(take(f.gateway->stats()).source_pid != first_pid);
    const auto restored = f.image();
    const auto& mover = process_mover(restored, f.a);
    if (f.input_a == 2) {
        CHECK(mover.accepted == 1);
        CHECK(mover.pending.size() == 1);
        CHECK(take(f.base.authority->authorize_entity(*f.base.pair.server, *f.base.scene,
                                                      f.base.entity, f.base.now))
                  .last_sequence == 2);
        CHECK_FALSE(f.gateway->submit(
            *f.base.pair.server, MovementInput{envelope(f.base.session, 2), mover.stream, 1, 0, 0},
            f.base.now));
        CHECK(process_mover(f.tick(), f.a).applied == 1);
    } else
        CHECK(mover.accepted == 0);
    f.finish();
}

TEST_CASE(
    "A dead life and its unavailable original flight import without healing or deadline reset",
    "[server-region-process]") {
    ProcessFixture f{false, "function on_combat(k,t,a,b,d,s) s[1]=s[1]+1 if k==1 then return "
                            "true,25,10 end return true,11,10 end"};
    f.move();
    f.move();
    f.tick();
    f.tick();
    for (int hit = 0; hit < 3; ++hit) {
        while (f.image().physics.completed_tick < process_actor(f.image(), f.b).ready_tick)
            f.tick();
        CHECK(f.combat(true, CombatActionKind::strike).status == RegionRouteStatus::admitted);
        const auto struck = f.tick();
        CHECK(process_actor(struck, f.a).health == 100 - 25 * (hit + 1));
    }
    while (f.image().physics.completed_tick + 1 < process_actor(f.image(), f.b).ready_tick)
        f.tick();
    f.combat(false, CombatActionKind::projectile);
    f.tick();
    f.combat(true, CombatActionKind::strike);
    const auto cut = f.tick();
    REQUIRE(process_actor(cut, f.a).health == 0);
    REQUIRE(process_actor(cut, f.a).projectile);
    const auto deadline = process_actor(cut, f.a).respawn_tick;
    const auto oldlife = process_actor(cut, f.a).target;
    const auto dead_entity =
        std::find_if(cut.entities.begin(), cut.entities.end(),
                     [&](const auto& row) { return row.source.identity == EntityIdentity{f.a}; });
    REQUIRE(dead_entity != cut.entities.end());
    const auto dead_pose = dead_entity->transform;
    CHECK(dead_pose.translation.x > .5);
    CHECK(process_actor(cut, f.a).movement.last_input == 2);
    take(f.gateway->prepare_handoff(id<RegionInstanceId>(827), f.live(), f.base.now));
    f.commit();
    take(f.gateway->activate(f.base.now));
    const auto installed = f.image();
    CHECK(process_actor(installed, f.a).health == 0);
    CHECK(process_actor(installed, f.a).respawn_tick == deadline);
    CHECK(process_actor(installed, f.a).ready_tick == process_actor(cut, f.a).ready_tick);
    const auto installed_entity =
        std::find_if(installed.entities.begin(), installed.entities.end(),
                     [&](const auto& row) { return row.source.identity == EntityIdentity{f.a}; });
    REQUIRE(installed_entity != installed.entities.end());
    CHECK(installed_entity->transform == dead_pose);
    const auto ownership = take(f.base.authority->region_ownership(f.base.session.character));
    REQUIRE(ownership);
    REQUIRE(ownership->last_transfer);
    CHECK(ownership->last_transfer->accepted_input == 2);
    CHECK(ownership->last_transfer->applied_input == 2);
    CHECK(process_actor(installed, f.b).outcomes == process_actor(cut, f.b).outcomes);
    CHECK(process_actor(installed, f.b).ready_tick == process_actor(cut, f.b).ready_tick);
    const auto cancelled = f.tick();
    REQUIRE_FALSE(process_actor(cancelled, f.a).projectile);
    REQUIRE(process_actor(cancelled, f.a).outcomes.size() == 1);
    CHECK(process_actor(cancelled, f.a).outcomes.front().outcome ==
          CombatOutcome::actor_unavailable);
    CHECK(process_actor(cancelled, f.b).health == 100);
    while (f.image().physics.completed_tick + 1 < deadline) {
        const auto tick = f.tick();
        CHECK(process_actor(tick, f.a).health == 0);
    }
    const auto alive = f.tick();
    CHECK(alive.physics.completed_tick == deadline);
    CHECK(process_actor(alive, f.a).health == 100);
    CHECK(process_actor(alive, f.a).target != oldlife);
    CHECK(process_actor(alive, f.a).movement.stream != process_actor(cut, f.a).movement.stream);
    CHECK(alive.rules.state == cut.rules.state);
    f.finish();
}

TEST_CASE("The gateway never retains a released player's TLS transport between native ticks",
          "[server-region-process]") {
    ProcessFixture f;
    const std::array<TlsTransport*, 1> only_source{f.base.pair.server.get()};
    f.base.now += 50ms;
    const auto detached = take(f.gateway->step(only_source, f.base.now));
    CHECK(process_actor(detached, f.b).detached);
    CHECK_FALSE(process_actor(detached, f.b).body);
    f.base.peer->server.reset();
    f.base.peer->client.reset();
    f.base.now += 50ms;
    const auto next = take(f.gateway->step(only_source, f.base.now));
    CHECK(process_actor(next, f.b).detached);
    CHECK(process_actor(next, f.b).health == 100);
    CHECK(process_mover(next, f.a).applied == 0);
    CHECK(take(f.gateway->stats()).phase == RegionGatewayPhase::active);
    f.finish();
}
