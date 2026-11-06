// Reuses the retained real TLS/Store/native regional fixture in this TU.
namespace {
RegionHandoff finish_handoff(RegionalFixture& f, RegionTransferId id) {
    const auto logical_start = f.base.now;
    const auto wall_start = std::chrono::steady_clock::now();
    for (;;) {
        const auto elapsed = std::chrono::steady_clock::now() - wall_start;
        REQUIRE(elapsed < 5s);
        const auto now = logical_start + std::chrono::duration_cast<MonotonicTime>(elapsed);
        auto result = take(f.world->poll_handoff(id, *f.base.authority, now));
        if (result.phase != RegionHandoffPhase::committing) {
            f.base.now = now;
            return result;
        }
        std::this_thread::yield();
    }
}
const MovementActorCut& mover(const RegionCut& cut, EntityHandle entity) {
    const auto found = std::find_if(cut.movement.actors.begin(), cut.movement.actors.end(),
                                    [&](const auto& row) { return row.entity.handle == entity; });
    REQUIRE(found != cut.movement.actors.end());
    return *found;
}
const CombatActorCut& fighter(const RegionCut& cut, EntityHandle entity) {
    const auto found = std::find_if(cut.combat.actors.begin(), cut.combat.actors.end(),
                                    [&](const auto& row) { return row.entity.handle == entity; });
    REQUIRE(found != cut.combat.actors.end());
    return *found;
}
} // namespace

TEST_CASE("Final regional cut retains original queues flights Lua state and durable identity",
          "[server-transfer]") {
    RegionalFixture f{true, true, true, R"(
calls=0
function on_combat(kind,tick,actor,target,distance)
  calls=calls+1
  if kind==1 or kind==3 then return true,10+calls,10 end
  return true,0,5
end)"};
    const auto original = f.grant();
    const auto writer = take(f.world->current_writer(original));
    const auto npc = take(f.world->bind_npc(f.base.npc_entity));
    const auto baseline = f.interest();
    auto replicas = take(ReplicaClient::create(f.base.session));
    take(replicas->apply(baseline));
    const auto replica_before = take(replicas->snapshot());
    const auto handoff_id = id<RegionTransferId>(601);
    const auto prepared =
        take(f.world->prepare_handoff(handoff_id, original, f.regions[1].id,
                                      id<RegionInstanceId>(105), *f.base.authority, f.base.now));
    CHECK(prepared.phase == RegionHandoffPhase::prepared);
    CHECK_FALSE(prepared.cut);
    const TransferRequest operation{{f.durable->id, f.base.record.account, 1},
                                    f.base.record.character,
                                    f.base.peer_session->character,
                                    1,
                                    1,
                                    {7, 2}};
    const auto economic = take(f.world->submit_transfer(
        original, *f.base.authority, *f.base.pair.server,
        envelope(f.base.session, f.base.next_command++), operation, f.base.now + 10s, f.base.now));
    f.move();
    f.move();
    take(f.world->submit_combat(original, *f.base.authority, *f.base.pair.server,
                                {envelope(f.base.session, f.base.next_command++),
                                 f.a->gameplay.stream, 1, f.b->gameplay.target,
                                 CombatActionKind::projectile},
                                f.base.now));
    take(
        f.world->submit_combat(f.grant(true), *f.base.authority, *f.base.peer->server,
                               {envelope(*f.base.peer_session, f.command_b++), f.b->gameplay.stream,
                                1, f.a->gameplay.target, CombatActionKind::projectile},
                               f.base.now));
    CHECK(f.tick().gameplay.projectile_launches == 2);
    CHECK(f.tick().transitions.empty()); // Position alone cannot change a durable writer.
    f.move();
    f.move();
    take(f.world->submit_npc_velocity(npc, {2, 0, 0}));
    const std::array<TlsTransport*, 2> transports{f.base.pair.server.get(),
                                                  f.base.peer->server.get()};
    const auto cut =
        take(f.world->cut_handoff(handoff_id, *f.base.authority, transports, f.base.now));
    REQUIRE(cut.phase == RegionHandoffPhase::cut);
    REQUIRE(cut.cut);
    REQUIRE(cut.request);
    CHECK(cut.request->source == writer);
    CHECK(cut.request->cut_tick == 2);
    CHECK(cut.request->accepted_input == 4);
    CHECK(cut.request->applied_input == 2);
    CHECK(cut.request->command_sequence == f.base.next_command - 1);
    CHECK(cut.request->cut_digest == cut.cut->digest);
    const auto& movement = mover(*cut.cut, f.base.entity);
    CHECK(movement.accepted == 4);
    CHECK(movement.applied == 2);
    REQUIRE(movement.pending.size() == 2);
    CHECK(movement.pending[0].input_sequence == 3);
    CHECK(movement.pending[1].input_sequence == 4);
    const auto& combat = fighter(*cut.cut, f.base.entity);
    REQUIRE(combat.projectile);
    CHECK(combat.projectile->request.action_sequence == 1);
    CHECK(combat.projectile->decision.damage == 11);
    CHECK(combat.projectile->remaining_ticks == 19);
    CHECK(combat.health == 100);
    CHECK(combat.ready_tick == 11);
    REQUIRE(fighter(*cut.cut, f.base.peer_entity).projectile);
    CHECK(fighter(*cut.cut, f.base.peer_entity).projectile->request.target == f.a->gameplay.target);
    CHECK(fighter(*cut.cut, f.base.peer_entity).projectile->decision.damage == 12);
    CHECK(cut.cut->combat.rules.evaluations == 2);
    REQUIRE(cut.cut->npcs.size() == 1);
    REQUIRE(cut.cut->npcs.front().pending);
    CHECK(cut.cut->npcs.front().pending->x == 2);
    REQUIRE(cut.cut->runtime.size() == 1);
    CHECK(cut.cut->runtime.front().ticket == economic);
    CHECK(cut.cut->runtime.front().transfer == operation);
    CHECK(cut.cut->runtime.front().writer == writer);
    const auto native_before = take(f.base.physics->body_state(*f.base.scene, f.base.body));
    const auto npc_before = take(f.base.physics->body_state(*f.base.scene, f.base.npc_body));
    CHECK_FALSE(f.world->step(*f.base.authority, transports, f.base.now));
    CHECK_FALSE(f.world->submit_movement(
        original, *f.base.authority, *f.base.pair.server,
        {envelope(f.base.session, f.base.next_command), f.a->gameplay.movement.stream, 5, 1000, 0},
        f.base.now));
    CHECK_FALSE(f.world->submit_npc_velocity(npc, {}));
    CHECK(take(f.base.physics->stats()).completed_ticks == 2);
    CHECK(take(f.base.physics->body_state(*f.base.scene, f.base.body)).pose == native_before.pose);
    CHECK(take(f.world->cut_handoff(handoff_id, *f.base.authority, transports, f.base.now)).cut ==
          cut.cut);
    CHECK(take(f.world->begin_commit(handoff_id, *f.base.authority, f.base.now + 1s, f.base.now))
              .phase == RegionHandoffPhase::committing);
    CHECK_FALSE(f.world->abort_handoff(handoff_id, *f.base.authority, f.base.now));
    const auto committed = finish_handoff(f, handoff_id);
    REQUIRE(committed.phase == RegionHandoffPhase::committed);
    REQUIRE(committed.grant);
    CHECK_FALSE(committed.budget_exceeded);
    const auto current = *committed.grant;
    const auto next = take(f.world->current_writer(current));
    CHECK(next.epoch == writer.epoch + 1);
    CHECK(next.region == f.regions[1].id);
    CHECK(take(f.world->poll_handoff(handoff_id, *f.base.authority, f.base.now)).grant == current);
    CHECK(take(f.base.physics->stats()).completed_ticks == 2);
    CHECK(take(f.base.physics->body_state(*f.base.scene, f.base.body)).pose == native_before.pose);
    CHECK(take(f.base.physics->find_body(*f.base.scene, native_before.entity)) == f.base.body);
    CHECK(f.state().stream == f.a->gameplay.stream);
    CHECK(f.state().ready_tick == 11);
    CHECK(f.state().outcomes.empty());
    CHECK_FALSE(f.world->take_transfer(original, *f.base.authority, *f.base.pair.server, economic,
                                       f.base.now));
    f.settled();
    const auto outcome = take(f.world->take_transfer(current, *f.base.authority,
                                                     *f.base.pair.server, economic, f.base.now));
    REQUIRE(outcome.outcome);
    REQUIRE(*outcome.outcome);
    CHECK(outcome.transfer == operation);
    CHECK(outcome.writer == writer);
    CHECK(std::get<TransferResult>(outcome.outcome->value()).receipt.source->state ==
          EconomyState{93, 18});
    const auto resumed = f.tick();
    CHECK(resumed.gameplay.movement.deliveries.front().state.last_input == 3);
    const auto npc_after = take(f.base.physics->body_state(*f.base.scene, f.base.npc_body));
    CHECK(npc_after.pose.translation.x - npc_before.pose.translation.x ==
          Catch::Approx(.1).margin(.002));
    std::size_t impacts = resumed.gameplay.projectile_impacts;
    for (int tick = 0; tick < 8; ++tick) {
        const auto report = f.tick();
        impacts += report.gameplay.projectile_impacts;
        CHECK(report.gameplay.projectile_launches == 0);
    }
    CHECK(impacts == 2);
    CHECK(f.state(true).health == 89);
    CHECK(f.state().health == 88);
    take(f.world->submit_combat(current, *f.base.authority, *f.base.pair.server,
                                {envelope(f.base.session, f.base.next_command++),
                                 f.a->gameplay.stream, 2, f.b->gameplay.target,
                                 CombatActionKind::strike},
                                f.base.now));
    const auto second = f.tick();
    REQUIRE(second.gameplay.outcomes.size() == 1);
    CHECK(second.gameplay.outcomes.front().outcome.amount == 13); // Same mutable Lua VM.
    CHECK(f.state(true).health == 76);
    const auto delta = f.interest();
    CHECK(delta.stream == baseline.stream);
    for (const auto& change : delta.changes)
        CHECK(change.kind == ReplicationChange::update);
    take(replicas->apply(delta));
    const auto replica_after = take(replicas->snapshot());
    REQUIRE(replica_after.entities.size() == replica_before.entities.size());
    for (std::size_t i = 0; i < replica_after.entities.size(); ++i)
        CHECK(replica_after.entities[i].id == replica_before.entities[i].id);
    f.base.complete = true;
}

TEST_CASE("A known unsubmitted cut aborts without losing accepted input or writer identity",
          "[server-transfer]") {
    RegionalFixture f{true, false, true};
    const auto original = f.grant();
    const auto writer = take(f.world->current_writer(original));
    const auto transfer = id<RegionTransferId>(602);
    take(f.world->prepare_handoff(transfer, original, f.regions[1].id, id<RegionInstanceId>(105),
                                  *f.base.authority, f.base.now));
    f.move();
    f.move();
    f.tick();
    f.tick();
    f.move();
    const std::array<TlsTransport*, 2> transports{f.base.pair.server.get(),
                                                  f.base.peer->server.get()};
    const auto cut =
        take(f.world->cut_handoff(transfer, *f.base.authority, transports, f.base.now));
    CHECK(mover(*cut.cut, f.base.entity).pending.size() == 1);
    CHECK(take(f.world->abort_handoff(transfer, *f.base.authority, f.base.now)).phase ==
          RegionHandoffPhase::aborted);
    CHECK(take(f.world->current_writer(original)) == writer);
    CHECK(f.tick().gameplay.movement.deliveries.front().state.last_input == 3);
    CHECK_FALSE(f.world->begin_commit(transfer, *f.base.authority, f.base.now + 1s, f.base.now));
    CHECK(take(f.base.authority->region_ownership(f.base.session.character))->writer == writer);
    f.base.complete = true;
}

TEST_CASE("Regional handoff identities reject changed retries and bound retained receipt capacity",
          "[server-transfer]") {
    RegionalFixture f{true, false, true};
    const auto grant = f.grant();
    for (unsigned n = 0; n < 8; ++n) {
        const auto transfer = id<RegionTransferId>(610 + n);
        const auto prepared = take(f.world->prepare_handoff(transfer, grant, f.regions[1].id,
                                                            id<RegionInstanceId>(105),
                                                            *f.base.authority, f.base.now));
        CHECK(
            take(f.world->prepare_handoff(transfer, grant, f.regions[1].id,
                                          id<RegionInstanceId>(105), *f.base.authority, f.base.now))
                .phase == prepared.phase);
        CHECK_FALSE(f.world->prepare_handoff(transfer, grant, f.regions[1].id,
                                             id<RegionInstanceId>(106), *f.base.authority,
                                             f.base.now));
        CHECK_FALSE(f.world->prepare_handoff(id<RegionTransferId>(699), grant, f.regions[1].id,
                                             id<RegionInstanceId>(105), *f.base.authority,
                                             f.base.now));
        take(f.world->abort_handoff(transfer, *f.base.authority, f.base.now));
    }
    CHECK_FALSE(f.world->prepare_handoff(id<RegionTransferId>(699), grant, f.regions[1].id,
                                         id<RegionInstanceId>(105), *f.base.authority, f.base.now));
    take(f.world->release_handoff(id<RegionTransferId>(610)));
    CHECK_FALSE(f.world->handoff(id<RegionTransferId>(610)));
    take(f.world->prepare_handoff(id<RegionTransferId>(699), grant, f.regions[1].id,
                                  id<RegionInstanceId>(105), *f.base.authority, f.base.now));
    CHECK_FALSE(f.world->release_handoff(id<RegionTransferId>(699)));
    f.base.complete = true;
}

TEST_CASE("Rotated execution permits fence stale lower owners before native mutation",
          "[server-transfer]") {
    Fixture f{false, false, character_body(), true};
    auto fence = take(ExecutionFence::create());
    const auto original = take(fence->permit());
    f.movement =
        take(CharacterMovement::create(*f.scene, *f.physics, f.origin, std::nullopt, {}, fence));
    auto combat = take(CharacterCombat::create(
        *f.scene, *f.physics, take(CombatRules::create({"scripts/combat.lua", combat_source})),
        fence));
    const auto first = take(combat->bind(*f.authority, *f.pair.server, *f.movement, *f.scene,
                                         *f.physics, f.entity, f.now));
    const auto second = take(combat->bind(*f.authority, *f.peer->server, *f.movement, *f.scene,
                                          *f.physics, f.peer_entity, f.now));
    f.initial = first.movement;
    f.submit();
    take(fence->freeze(original));
    const auto cut = take(f.movement->capture_cut(*f.scene, *f.physics));
    REQUIRE(cut.actors.size() == 2);
    CHECK(cut.actors.front().pending.size() == 1);
    const std::array<TlsTransport*, 1> transports{f.pair.server.get()};
    CHECK_FALSE(f.movement->step(*f.authority, *f.scene, *f.physics, transports, 50ms));
    CHECK_FALSE(
        f.movement->submit(*f.authority, *f.pair.server, *f.scene, *f.physics, f.input(), f.now));
    const CombatAction action{envelope(f.session, f.next_command), first.stream, 1, second.target,
                              CombatActionKind::strike};
    CHECK_FALSE(combat->submit(*f.authority, *f.pair.server, *f.scene, *f.physics, action, f.now));
    CHECK_FALSE(combat->step(*f.authority, *f.movement, *f.scene, *f.physics, transports, 50ms));
    const auto next = take(fence->rotate(original));
    take(fence->resume(next));
    CHECK_FALSE(f.movement->step(*f.authority, *f.scene, *f.physics, transports, 50ms));
    CHECK_FALSE(f.movement->release(*f.scene, *f.physics, f.initial.stream));
    CHECK_FALSE(f.movement->capture_cut(*f.scene, *f.physics));
    CHECK_FALSE(combat->submit(*f.authority, *f.pair.server, *f.scene, *f.physics, action, f.now));
    CHECK_FALSE(combat->step(*f.authority, *f.movement, *f.scene, *f.physics, transports, 50ms));
    CHECK(take(f.authority->authorize_entity(*f.pair.server, *f.scene, f.entity, f.now))
              .last_sequence == 2);
    CHECK(take(f.physics->stats()).completed_ticks == 0);
    CHECK(take(f.physics->body_state(*f.scene, f.body)).pose == cut.actors.front().native.pose);
    CHECK(take(f.movement->stats()).pending_inputs == 1);
    CHECK_FALSE(fence->check(original));
    CHECK(take(fence->permit()) == next);
    f.complete = true;
}

TEST_CASE("A lost regional completion stays frozen until its exact persisted cut is reconciled",
          "[server-transfer]") {
    RegionalFixture f{true, false, true};
    const auto original = f.grant();
    const auto transfer = id<RegionTransferId>(603);
    take(f.world->prepare_handoff(transfer, original, f.regions[1].id, id<RegionInstanceId>(105),
                                  *f.base.authority, f.base.now));
    f.move();
    f.move();
    f.tick();
    f.tick();
    const std::array<TlsTransport*, 2> transports{f.base.pair.server.get(),
                                                  f.base.peer->server.get()};
    const auto cut =
        take(f.world->cut_handoff(transfer, *f.base.authority, transports, f.base.now));
    const auto begun =
        take(f.world->begin_commit(transfer, *f.base.authority, f.base.now + 10s, f.base.now));
    REQUIRE(begun.ticket);
    f.settled();
    // Host consumed the actual result before forwarding it to the coordinator.
    // The published durable row is evidence, not a synthesized success reply.
    const auto withheld = take(f.base.authority->take_region(*begun.ticket, f.base.now));
    REQUIRE(withheld.outcome);
    REQUIRE(*withheld.outcome);
    f.base.now += region_handoff_quiescence_budget + 1ns;
    const auto unknown = take(f.world->poll_handoff(transfer, *f.base.authority, f.base.now));
    CHECK(unknown.phase == RegionHandoffPhase::reconciliation_required);
    CHECK(unknown.budget_exceeded);
    CHECK_FALSE(f.world->abort_handoff(transfer, *f.base.authority, f.base.now));
    CHECK_FALSE(f.world->step(*f.base.authority, transports, f.base.now));
    CHECK_FALSE(f.world->release_handoff(transfer));
    CHECK(take(f.base.physics->stats()).completed_ticks == 2);
    const auto reconciled =
        take(f.world->reconcile_handoff(transfer, *f.base.authority, f.base.now));
    REQUIRE(reconciled.phase == RegionHandoffPhase::committed);
    CHECK(reconciled.budget_exceeded);
    REQUIRE(reconciled.grant);
    CHECK(take(f.world->current_writer(*reconciled.grant)).epoch == cut.request->source.epoch + 1);
    CHECK(take(f.base.physics->stats()).completed_ticks == 2);
    CHECK(f.state().stream == f.a->gameplay.stream);
    CHECK_FALSE(f.world->submit_movement(
        original, *f.base.authority, *f.base.pair.server,
        {envelope(f.base.session, f.base.next_command), f.a->gameplay.movement.stream, 3, 0, 0},
        f.base.now));
    f.move(0, 0); // The rejected old-grant attempt did not consume this sequence.
    CHECK(f.tick().gameplay.movement.deliveries.front().state.last_input == 3);
    f.base.complete = true;
}

TEST_CASE("A final dead-life cut preserves respawn deadline and original combat history",
          "[server-transfer]") {
    RegionalFixture f{true, false, true};
    const auto original = f.grant();
    f.move();
    f.move();
    f.tick();
    f.tick();
    const auto transfer = id<RegionTransferId>(604);
    take(f.world->prepare_handoff(transfer, original, f.regions[1].id, id<RegionInstanceId>(105),
                                  *f.base.authority, f.base.now));
    for (std::uint64_t hit = 0; hit < 4; ++hit) {
        while (take(f.world->stats()).completed_ticks < 2 + hit * 10)
            f.tick();
        take(f.world->submit_combat(f.grant(true), *f.base.authority, *f.base.peer->server,
                                    {envelope(*f.base.peer_session, f.command_b++),
                                     f.b->gameplay.stream, hit + 1, f.a->gameplay.target,
                                     CombatActionKind::strike},
                                    f.base.now));
        const auto report = f.tick();
        REQUIRE(report.gameplay.outcomes.size() == 1);
        CHECK(report.gameplay.outcomes.front().outcome.amount == 25);
    }
    CHECK(f.state().health == 0);
    const auto deadline = f.state().respawn_tick;
    const auto old_target = f.state().self;
    const std::array<TlsTransport*, 2> transports{f.base.pair.server.get(),
                                                  f.base.peer->server.get()};
    const auto cut =
        take(f.world->cut_handoff(transfer, *f.base.authority, transports, f.base.now));
    REQUIRE(cut.cut);
    const auto& dead = fighter(*cut.cut, f.base.entity);
    CHECK(dead.health == 0);
    CHECK(dead.respawn_tick == deadline);
    CHECK_FALSE(dead.body);
    const auto& attacker = fighter(*cut.cut, f.base.peer_entity);
    REQUIRE(attacker.outcomes.size() == 4);
    CHECK(attacker.outcomes.front().action_sequence == 1);
    CHECK(attacker.outcomes.back().action_sequence == 4);
    take(f.world->begin_commit(transfer, *f.base.authority, f.base.now + 1s, f.base.now));
    REQUIRE(finish_handoff(f, transfer).phase == RegionHandoffPhase::committed);
    CHECK(f.state().health == 0);
    CHECK(f.state().respawn_tick == deadline);
    CHECK(f.state().self == old_target);
    CHECK(f.state(true).outcomes == attacker.outcomes);
    while (take(f.world->stats()).completed_ticks + 1 < deadline) {
        CHECK(f.tick().gameplay.respawns == 0);
        CHECK(f.state().health == 0);
    }
    CHECK(f.tick().gameplay.respawns == 1);
    CHECK(f.state().health == 100);
    CHECK(f.state().self != old_target);
    CHECK(f.state().movement->stream != f.a->gameplay.movement.stream);
    CHECK(take(f.world->current_writer(f.grant())).region == f.regions[1].id);
    f.base.complete = true;
}

TEST_CASE("Regional cuts reject missing destination readiness and later content invalidation",
          "[server-transfer]") {
    for (const bool after_cut : {false, true}) {
        RegionalFixture f{true, false, true};
        const auto original = f.grant();
        const auto writer = take(f.world->current_writer(original));
        const auto transfer = id<RegionTransferId>(605);
        take(f.world->prepare_handoff(transfer, original, f.regions[1].id,
                                      id<RegionInstanceId>(105), *f.base.authority, f.base.now));
        f.move();
        f.move();
        f.tick();
        f.tick();
        const std::array<TlsTransport*, 2> transports{f.base.pair.server.get(),
                                                      f.base.peer->server.get()};
        if (after_cut)
            take(f.world->cut_handoff(transfer, *f.base.authority, transports, f.base.now));
        const auto cells = take(f.traversal->cells());
        REQUIRE(cells.size() == 2);
        const auto neighbor = std::find_if(cells.begin(), cells.end(), [&](const auto& cell) {
            return cell.key.cell.x == f.base.origin.cell.x + 1;
        });
        REQUIRE(neighbor != cells.end());
        take(f.traversal->deactivate(neighbor->key, neighbor->resident_generation));
        if (after_cut)
            CHECK_FALSE(
                f.world->begin_commit(transfer, *f.base.authority, f.base.now + 1s, f.base.now));
        else
            CHECK_FALSE(f.world->cut_handoff(transfer, *f.base.authority, transports, f.base.now));
        CHECK(take(f.base.authority->runtime_stats()).retained == 0);
        CHECK(take(f.base.authority->region_ownership(f.base.session.character))->writer == writer);
        CHECK(take(f.base.physics->stats()).completed_ticks == 2);
        take(f.world->abort_handoff(transfer, *f.base.authority, f.base.now));
        f.base.complete = true;
    }
}
