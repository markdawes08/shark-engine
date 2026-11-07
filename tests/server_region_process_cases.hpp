#include "server_region_process_fixture.hpp"

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

TEST_CASE("A lost combat commit reply replays one original action without another admission",
          "[server-region-process]") {
    ProcessFixture f;
    const auto initial = f.image();
    const auto& actor = process_actor(initial, f.a);
    const auto& target = process_actor(initial, f.b);
    const CombatAction original{envelope(f.base.session, f.base.next_command), actor.stream,
                                f.action_a, target.target, CombatActionKind::strike};
    shark::server::detail::set_region_control_fault(
        shark::server::detail::RegionControlPoint::combat_commit,
        shark::server::detail::RegionControlFault::drop_reply);
    CHECK(f.combat(false, CombatActionKind::strike).status == RegionRouteStatus::uncertain);
    const auto admitted = take(f.base.authority->authorize_entity(
        *f.base.pair.server, *f.base.scene, f.base.entity, f.base.now));
    CHECK(admitted.last_sequence == original.authority.sequence);
    CHECK(process_actor(f.image(), f.a).accepted == 0);
    CHECK(f.image().rules.evaluations == 0);
    f.recover(828);
    const auto restored = f.image();
    REQUIRE(process_actor(restored, f.a).pending);
    CHECK(*process_actor(restored, f.a).pending == original);
    CHECK(process_actor(restored, f.a).accepted == original.action_sequence);
    CHECK(process_actor(restored, f.a).outcomes.empty());
    CHECK(restored.rules.state[0] == 0);
    CHECK(restored.rules.evaluations == 0);
    CHECK(take(f.base.authority->authorize_entity(*f.base.pair.server, *f.base.scene, f.base.entity,
                                                  f.base.now)) == admitted);
    const auto resolved = f.tick();
    REQUIRE(process_actor(resolved, f.a).outcomes.size() == 1);
    CHECK(process_actor(resolved, f.a).outcomes.front() ==
          CombatActionResult{original.action_sequence, original.target, CombatActionKind::strike,
                             CombatOutcome::hit, 11});
    CHECK(process_actor(resolved, f.b).health == 89);
    CHECK(resolved.rules.state[0] == 1);
    CHECK(resolved.rules.evaluations == 1);
    CHECK_FALSE(f.gateway->submit(*f.base.pair.server, original, f.base.now));
    const auto repeated = f.tick();
    CHECK(process_actor(repeated, f.a).outcomes == process_actor(resolved, f.a).outcomes);
    CHECK(process_actor(repeated, f.b).health == 89);
    CHECK(repeated.rules.state[0] == 1);
    CHECK(repeated.rules.evaluations == 1);
    CHECK(take(f.base.authority->authorize_entity(*f.base.pair.server, *f.base.scene, f.base.entity,
                                                  f.base.now)) == admitted);
    f.finish();
}

TEST_CASE("A lost completed step restores the acknowledged cut and commits one outcome and effect",
          "[server-region-process]") {
    ProcessFixture f;
    const auto initial = f.image();
    const TransferRequest operation{{initial.writers.front().store, f.base.session.account, 1},
                                    f.base.session.character,
                                    f.base.peer_session->character,
                                    1,
                                    1,
                                    {7, 2}};
    const auto economic = take(f.gateway->submit_transfer(
        *f.base.pair.server, envelope(f.base.session, f.base.next_command++), operation,
        f.base.now + 10s, f.base.now));
    CHECK(f.combat(false, CombatActionKind::strike).status == RegionRouteStatus::admitted);
    const auto acknowledged = f.image();
    REQUIRE(process_actor(acknowledged, f.a).pending);
    const auto original = *process_actor(acknowledged, f.a).pending;
    const auto admitted = take(f.base.authority->authorize_entity(
        *f.base.pair.server, *f.base.scene, f.base.entity, f.base.now));
    shark::server::detail::set_region_control_fault(
        shark::server::detail::RegionControlPoint::step,
        shark::server::detail::RegionControlFault::drop_reply);
    f.base.now += 50ms;
    CHECK_FALSE(f.gateway->step(f.live(), f.base.now));
    const auto retained = f.image();
    CHECK(retained.physics.completed_tick == acknowledged.physics.completed_tick);
    CHECK(process_actor(retained, f.a).pending == process_actor(acknowledged, f.a).pending);
    CHECK(process_actor(retained, f.b).health == 100);
    CHECK(retained.rules.state[0] == 0);
    CHECK(retained.rules.evaluations == 0);
    f.recover(829);
    const auto restored = f.image();
    CHECK(restored.physics.completed_tick == acknowledged.physics.completed_tick);
    CHECK(process_actor(restored, f.a).pending == process_actor(acknowledged, f.a).pending);
    CHECK(process_actor(restored, f.a).outcomes.empty());
    CHECK(restored.rules.state[0] == 0);
    CHECK(restored.rules.evaluations == 0);
    CHECK(take(f.base.authority->authorize_entity(*f.base.pair.server, *f.base.scene, f.base.entity,
                                                  f.base.now)) == admitted);
    const auto resolved = f.tick();
    CHECK(resolved.physics.completed_tick == acknowledged.physics.completed_tick + 1);
    REQUIRE(process_actor(resolved, f.a).outcomes.size() == 1);
    CHECK(process_actor(resolved, f.a).outcomes.front() ==
          CombatActionResult{original.action_sequence, original.target, CombatActionKind::strike,
                             CombatOutcome::hit, 11});
    CHECK(process_actor(resolved, f.b).health == 89);
    CHECK(resolved.rules.state[0] == 1);
    CHECK(resolved.rules.evaluations == 1);
    const auto repeated = f.tick();
    CHECK(process_actor(repeated, f.a).outcomes == process_actor(resolved, f.a).outcomes);
    CHECK(process_actor(repeated, f.b).health == 89);
    CHECK(repeated.rules.state[0] == 1);
    CHECK(repeated.rules.evaluations == 1);
    CHECK(take(f.base.authority->authorize_entity(*f.base.pair.server, *f.base.scene, f.base.entity,
                                                  f.base.now)) == admitted);
    f.settled();
    const auto completion =
        take(f.gateway->take_transfer(*f.base.pair.server, economic, f.base.now));
    CHECK(completion.ticket == economic);
    CHECK(completion.transfer == operation);
    CHECK(completion.writer == initial.writers.front());
    REQUIRE(completion.outcome);
    const auto value = take(*completion.outcome);
    REQUIRE(std::get<TransferResult>(value).receipt.source);
    REQUIRE(std::get<TransferResult>(value).receipt.target);
    CHECK(std::get<TransferResult>(value).receipt.source->state == EconomyState{93, 18});
    CHECK(std::get<TransferResult>(value).receipt.target->state == EconomyState{107, 22});
    const auto source_balance = take(f.base.authority->economy(
        *f.base.pair.server, envelope(f.base.session, f.base.next_command++), f.base.now));
    const auto target_balance = take(f.base.authority->economy(
        *f.base.peer->server, envelope(*f.base.peer_session, f.command_b++), f.base.now));
    CHECK(source_balance.state == EconomyState{93, 18});
    CHECK(target_balance.state == EconomyState{107, 22});
    CHECK(source_balance.revision == 2);
    CHECK(target_balance.revision == 2);
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
