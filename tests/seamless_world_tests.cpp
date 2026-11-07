#include "seamless_world_content.hpp"
#include "server_region_process_fixture.hpp"

#include <iomanip>

namespace {
struct VerifiedRecipient {
    std::unique_ptr<MovementPredictor> prediction;
    std::unique_ptr<CombatClient> combat;
    std::unique_ptr<FaultLink> observations;
    std::vector<CombatActionResult> effects;
    std::size_t stale_observations{};
    explicit VerifiedRecipient(ProcessFixture& fixture) {
        const auto initial =
            take(fixture.gateway->combat_state(*fixture.base.pair.server, fixture.base.now));
        REQUIRE(initial.movement);
        prediction = take(MovementPredictor::create(fixture.base.session, *initial.movement));
        combat = take(CombatClient::create(fixture.base.session, initial));
        FaultLinkConfig config;
        config.delay = 100ms;
        config.jitter = 50ms;
        config.reorder_delay = 75ms;
        config.loss_per_10000 = 1000;
        config.duplication_per_10000 = 3000;
        config.reorder_per_10000 = 4000;
        observations = take(FaultLink::create(config));
    }
    RegionRouteReceipt move(ProcessFixture& fixture, std::int16_t x) {
        const auto prepared = take(
            prediction->prepare(envelope(fixture.base.session, fixture.base.next_command), x, 0));
        CHECK(prepared.input().input_sequence == fixture.input_a);
        CHECK(take(prediction->send(*fixture.base.pair.client, prepared)) ==
              PredictionSendResult::admitted);
        const auto frame = fixture.base.pair.receive(true, fixture.base.now);
        REQUIRE(frame.kind == MessageKind::movement_input);
        const auto result = take(fixture.gateway->submit(*fixture.base.pair.server,
                                                         take(decode_movement_input(frame.payload)),
                                                         fixture.base.now));
        ++fixture.base.next_command;
        ++fixture.input_a;
        return result;
    }
    void deliver(MonotonicTime now) {
        for (const auto& packet : take(observations->drain(now, 64))) {
            const auto reply = take(decode_combat_reply(packet.payload));
            const auto before = take(combat->snapshot());
            const auto applied = combat->apply(reply);
            if (!applied) {
                const auto* state = std::get_if<CombatState>(&reply);
                REQUIRE(state);
                CHECK(state->completed_tick < before.state.completed_tick);
                CHECK(applied.error().context.operation == "net.combat.client.tick");
                CHECK(take(combat->snapshot()).state == before.state);
                ++stale_observations;
                continue;
            }
            const auto& update = applied.value();
            CHECK(update.status != CombatClientApplyResult::resync_required);
            if (update.status == CombatClientApplyResult::duplicate ||
                update.status == CombatClientApplyResult::rejected)
                CHECK(update.effects.empty());
            effects.insert(effects.end(), update.effects.begin(), update.effects.end());
            const auto current = take(combat->snapshot());
            if (current.state.movement) {
                CHECK(current.state.movement->position.x >= 1020);
                CHECK(current.state.movement->position.x <= 1028);
            }
        }
    }
    void record(ProcessFixture& fixture) {
        const auto state =
            take(fixture.gateway->combat_state(*fixture.base.pair.server, fixture.base.now));
        take(fixture.base.pair.server->send(MessageKind::combat_state,
                                            take(encode_combat_reply(CombatReply{state}))));
        const auto verified = fixture.base.pair.receive(false, fixture.base.now);
        REQUIRE(verified.kind == MessageKind::combat_state);
        const auto reply = take(decode_combat_reply(verified.payload));
        const auto* complete = std::get_if<CombatState>(&reply);
        REQUIRE(complete);
        REQUIRE(complete->movement);
        // Movement corrections remain reliable; only already verified combat
        // observations enter the semantic fault link. No TLS bytes are dropped.
        CHECK(take(prediction->apply(MovementReply{*complete->movement})) !=
              PredictionCorrectionResult::reconciliation_required);
        const auto queued =
            take(observations->send(fixture.base.now, verified.payload, state.completed_tick));
        CHECK(queued.outcome != FaultSendOutcome::backpressure);
        deliver(fixture.base.now);
    }
};
struct ConnectedMetrics {
    std::uint64_t rss_peak{};
    std::size_t rss_samples{};
    std::size_t logical_resident_peak{};
    std::size_t logical_pending_peak{};
    MonotonicTime quiescence_peak{};
    void sample(ProcessFixture& fixture, bool logical = true) {
        const auto stats = take(fixture.gateway->stats());
        quiescence_peak = std::max(quiescence_peak, stats.quiescence);
        for (const auto pid : {stats.source_pid, stats.destination_pid}) {
            if (pid == 0)
                continue;
            rss_peak = std::max(rss_peak, seamless_content::child_rss(pid));
            ++rss_samples;
        }
        if (!logical)
            return;
        const auto cells = take(fixture.gateway->cell_usage(fixture.base.now));
        CHECK(cells.cells.usage.resident_cells <= 2);
        CHECK(cells.cells.usage.pending_bytes <=
              fixture.image().residency_limits.maximum_pending_bytes);
        logical_resident_peak =
            std::max(logical_resident_peak, cells.cells.usage.peak_resident_bytes);
        logical_pending_peak = std::max(logical_pending_peak, cells.cells.usage.peak_pending_bytes);
    }
};
bool terminal(WorldCellJobState state) {
    return state == WorldCellJobState::published || state == WorldCellJobState::failed ||
           state == WorldCellJobState::cancelled;
}
WorldCellJobStatus finish_job(ProcessFixture& fixture, RegionCellTicket ticket) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        // A retained loader job does not freeze native gameplay. Publication is
        // explicit owner work, even when the private reader has completed.
        fixture.tick();
        take(fixture.gateway->drain_cells(fixture.base.now));
        const auto usage = take(fixture.gateway->cell_usage(fixture.base.now));
        CHECK(usage.instance == ticket.instance);
        const auto found = std::find_if(usage.cells.jobs.begin(), usage.cells.jobs.end(),
                                        [&](const auto& row) { return row.id == ticket.job; });
        REQUIRE(found != usage.cells.jobs.end());
        if (terminal(found->state)) {
            const auto original = *found;
            const auto taken = take(fixture.gateway->take_cell(ticket, fixture.base.now));
            CHECK(taken.id == original.id);
            CHECK(taken.state == original.state);
            CHECK(taken.resident_generation == original.resident_generation);
            return taken;
        }
        std::this_thread::yield();
    }
}
math::Vec3 publication_position(const RegionProcessImage& image, WorldEntityId entity,
                                WorldLocalFrame publication) {
    const auto local = process_body(image, entity).native.pose.translation;
    const auto world = take(world_position_from_local(
        image.frame.origin, {local.x, local.y, local.z}, world_local_coordinate_limit));
    const auto relative =
        take(world_position_relative_to(world, publication.origin, world_local_coordinate_limit));
    return {relative[0], relative[1], relative[2]};
}
std::vector<math::Vec3> connected_world(std::int64_t base_cell) {
    CAPTURE(base_cell);
    const auto world = id<WorldId>(1);
    seamless_content::AuthoredWorld content{fs::absolute(fs::path{SHARK_MOVEMENT_SCRATCH}), world,
                                            base_cell};
    ProcessFixtureOptions options;
    options.frame = WorldLocalFrame{{world, {base_cell, 0, 0}, {0, 0, 0}}, 1};
    options.regions = {{id<RegionId>(101), 1020, 1023.5, 3, 7, 110},
                       {id<RegionId>(102), 1023.5, 1028, 3, 7, 110}};
    options.player_a = math::Vec3{1022, .5, 4.5};
    options.player_b = math::Vec3{1022, .5, 6.3};
    options.npc = math::Vec3{1020.75, .5, 4.5};
    options.residency_limits.maximum_resident_cells = 2;
    options.residency_limits.maximum_pending_bytes =
        world_cell_maximum_payload_bytes + content.assets->retained_bytes();
    for (unsigned index = 0; index < content.cells.size(); ++index) {
        const auto& cell = content.cells[index];
        options.cells.push_back(
            {cell.key, cell.revision, id<WorldEntityId>(810 + index),
             index == 0 ? cell.payload : std::vector<std::byte>{},
             index == 0 ? RegionCellState::active : RegionCellState::unloaded,
             RegionCellFileSource{cell.path, take(sha256(cell.payload)), cell.payload.size()}});
    }
    ProcessFixture fixture{false, nullptr, &options};
    const auto publication = *options.frame;
    const auto initial = fixture.image();
    const auto original_session = fixture.base.session;
    const auto original_stream = process_mover(initial, fixture.a).stream;
    const auto original_target = process_actor(initial, fixture.a).target;
    const auto npc = id<WorldEntityId>(40);
    ConnectedMetrics metrics;
    metrics.sample(fixture);
    const auto baseline = fixture.interest();
    auto replicas = take(ReplicaClient::create(fixture.base.session));
    take(replicas->apply(baseline));
    const auto replica_ids = take(replicas->snapshot());
    VerifiedRecipient recipient{fixture};
    const auto move = [&](std::int16_t x = 1000) { return recipient.move(fixture, x); };
    const auto tick = [&] {
        auto image = fixture.tick();
        recipient.record(fixture);
        return image;
    };
    std::vector<math::Vec3> observations;
    const auto observe = [&] {
        const auto image = fixture.image();
        const auto position = publication_position(image, fixture.a, publication);
        observations.push_back(position);
        CHECK(process_mover(image, fixture.a).stream == original_stream);
        CHECK(process_actor(image, fixture.a).target == original_target);
        CHECK(take(fixture.gateway->publication_frame()) == publication);
        const auto view =
            take(fixture.gateway->combat_state(*fixture.base.pair.server, fixture.base.now));
        REQUIRE(view.movement);
        CHECK(view.movement->position == position);
        CHECK(view.session == original_session.session);
        CHECK(view.ownership_epoch == original_session.ownership_epoch);
    };

    // The logical authority boundary is 0.5 m before the content seam. A cold
    // next cell consumes admitted input as a horizontal stop, without inventing
    // collision/nav readiness or disconnecting either actual player session.
    for (unsigned step = 0; step < 10; ++step) {
        CHECK(move().status == RegionRouteStatus::admitted);
        tick();
    }
    const auto stopped = fixture.image();
    const auto stop_position = publication_position(stopped, fixture.a, publication);
    CHECK(stop_position.x > 1023.0);
    CHECK(stop_position.x < 1023.5);
    CHECK(process_mover(stopped, fixture.a).accepted == 10);
    CHECK(process_mover(stopped, fixture.a).applied == 10);
    CHECK(process_mover(stopped, fixture.a).velocity.linear.x == 0);
    observe();

    const auto target_key = content.cells[1].key;
    // Real filesystem failure and corrupt bytes use the same cold descriptor;
    // neither can replace or detach the still-active first terrain generation.
    REQUIRE(fs::remove(content.cells[1].path));
    const auto missing = take(fixture.gateway->prefetch_cell(target_key, fixture.base.now));
    const auto missing_result = finish_job(fixture, missing);
    CHECK(missing_result.state == WorldCellJobState::failed);
    REQUIRE(missing_result.error);
    content.restore_cell(1);
    {
        std::ofstream corrupt{content.cells[1].path, std::ios::binary | std::ios::app};
        corrupt.put('!');
        corrupt.close();
        REQUIRE(corrupt);
    }
    const auto corrupt = take(fixture.gateway->prefetch_cell(target_key, fixture.base.now));
    const auto corrupt_result = finish_job(fixture, corrupt);
    CHECK(corrupt_result.state == WorldCellJobState::failed);
    REQUIRE(corrupt_result.error);
    content.restore_cell(1);
    const auto cancelled = take(fixture.gateway->prefetch_cell(target_key, fixture.base.now));
    take(fixture.gateway->cancel_cell(cancelled, fixture.base.now));
    const auto cancelled_result = finish_job(fixture, cancelled);
    CHECK(cancelled_result.state == WorldCellJobState::cancelled);
    CHECK(cancelled_result.cancellation_requested);
    CHECK(take(fixture.gateway->cell_usage(fixture.base.now)).cells.usage.resident_cells == 1);
    CHECK(take(fixture.gateway->cell_usage(fixture.base.now)).cells.active.size() == 1);

    // An unused third authored cell creates actual residency pressure. A valid
    // target load fails atomically, then succeeds after explicit inactive eviction.
    const auto unused =
        take(fixture.gateway->prefetch_cell(content.cells[2].key, fixture.base.now));
    // One complete bounded reader reservation fits. A second distinct request
    // rejects before IO/publication while the first receipt remains retained.
    CHECK_FALSE(fixture.gateway->prefetch_cell(target_key, fixture.base.now));
    CHECK(finish_job(fixture, unused).state == WorldCellJobState::published);
    const auto full = take(fixture.gateway->prefetch_cell(target_key, fixture.base.now));
    const auto full_result = finish_job(fixture, full);
    CHECK(full_result.state == WorldCellJobState::failed);
    REQUIRE(full_result.error);
    CHECK(full_result.error->code == ErrorCode::conflict);
    metrics.sample(fixture);
    take(fixture.gateway->evict_cell(content.cells[2].key, fixture.base.now));
    const auto retry = take(fixture.gateway->prefetch_cell(target_key, fixture.base.now));
    CHECK(finish_job(fixture, retry).state == WorldCellJobState::published);
    take(fixture.gateway->activate_cell(target_key, fixture.base.now));
    CHECK(take(fixture.gateway->cell_usage(fixture.base.now)).cells.active.size() == 2);
    take(fixture.gateway->deactivate_cell(target_key, fixture.base.now));
    CHECK(move().status == RegionRouteStatus::admitted);
    const auto inactive = tick();
    CHECK(publication_position(inactive, fixture.a, publication).x == stop_position.x);
    take(fixture.gateway->activate_cell(target_key, fixture.base.now));
    for (unsigned step = 0; step < 7; ++step) {
        CHECK(move().status == RegionRouteStatus::admitted);
        tick();
    }
    CHECK(publication_position(fixture.image(), fixture.a, publication).x > 1024.5);
    observe();
    take(fixture.gateway->set_pursuit(npc, 1, fixture.a, original_target, 4.0, 1.05,
                                      fixture.base.now));
    const auto actor = process_actor(fixture.image(), fixture.a);
    const auto target = process_actor(fixture.image(), fixture.b);
    const CombatAction shot{envelope(fixture.base.session, fixture.base.next_command++),
                            actor.stream, fixture.action_a++, target.target,
                            CombatActionKind::projectile};
    take(fixture.base.pair.client->send(MessageKind::combat_action,
                                        take(encode_combat_action(shot))));
    const auto incoming = fixture.base.pair.receive(true, fixture.base.now);
    REQUIRE(incoming.kind == MessageKind::combat_action);
    CHECK(take(fixture.gateway->submit(*fixture.base.pair.server,
                                       take(decode_combat_action(incoming.payload)),
                                       fixture.base.now))
              .status == RegionRouteStatus::admitted);
    tick();
    REQUIRE(process_actor(fixture.image(), fixture.a).projectile);

    // Both actual region workers coexist only during a frozen complete-island
    // handoff. The destination uses a real +1024 m frame shift, while session,
    // input/life identities and the client's publication frame remain stable.
    const auto before_shift = fixture.image();
    const auto source_pid = take(fixture.gateway->stats()).source_pid;
    auto shifted = publication;
    ++shifted.origin.cell.x;
    ++shifted.generation;
    take(fixture.gateway->prepare_handoff(id<RegionInstanceId>(901), fixture.live(),
                                          fixture.base.now, shifted));
    const auto prepared = take(fixture.gateway->stats());
    CHECK(prepared.source_pid == source_pid);
    CHECK(prepared.destination_pid != 0);
    CHECK(prepared.destination_pid != source_pid);
    // RSS is observed while both processes are owned, separately from charged
    // cell bytes. Per-process sampling is deliberately a functional budget.
    metrics.sample(fixture, false);
    fixture.commit();
    take(fixture.gateway->activate(fixture.base.now));
    const auto after_shift = fixture.image();
    metrics.sample(fixture);
    CHECK(after_shift.frame == shifted);
    for (const auto entity : {fixture.a, fixture.b, npc})
        CHECK(publication_position(after_shift, entity, publication) ==
              publication_position(before_shift, entity, publication));
    CHECK(take(fixture.gateway->stats()).source_pid != source_pid);
    for (const auto& writer : after_shift.writers) {
        CHECK(writer.epoch == 2);
        CHECK(writer.instance == id<RegionInstanceId>(901));
    }
    observe();
    recipient.record(fixture);
    const auto saved = fixture.image();
    const TransferRequest economic{{initial.writers.front().store, fixture.base.session.account, 1},
                                   fixture.base.session.character,
                                   fixture.base.peer_session->character,
                                   1,
                                   1,
                                   {7, 2}};
    const auto ticket = take(fixture.gateway->submit_transfer(
        *fixture.base.pair.server, envelope(fixture.base.session, fixture.base.next_command++),
        economic, fixture.base.now + 10s, fixture.base.now));
    shark::server::detail::set_region_control_fault(
        shark::server::detail::RegionControlPoint::step,
        shark::server::detail::RegionControlFault::drop_reply);
    fixture.base.now += 50ms;
    CHECK_FALSE(fixture.gateway->step(fixture.live(), fixture.base.now));
    fixture.recover(903);
    metrics.sample(fixture);
    CHECK(fixture.image().physics.completed_tick == saved.physics.completed_tick);
    CHECK(fixture.image().rules.evaluations == saved.rules.evaluations);
    CHECK(fixture.image().npcs.front().pursuit == saved.npcs.front().pursuit);
    CHECK_FALSE(fixture.gateway->take_cell(retry, fixture.base.now));
    take(recipient.observations->partition_until(fixture.base.now, fixture.base.now + 150ms));
    for (unsigned step = 0; step < 10; ++step)
        tick();
    CHECK(process_actor(fixture.image(), fixture.b).health == 89);
    CHECK(fixture.image().rules.state[0] == 1);
    CHECK_FALSE(process_actor(fixture.image(), fixture.a).projectile);

    // Pursuit uses actual checked native movement and current authored tile
    // geometry. It must cross both the logical edge and the streamed cell seam.
    for (unsigned step = 0; step < 50; ++step) {
        const auto image = tick();
        REQUIRE(image.npcs.front().pursuit);
        if (publication_position(image, npc, publication).x > 1024.0)
            break;
    }
    const auto pursuing = fixture.image();
    REQUIRE(pursuing.npcs.front().pursuit);
    const auto pursuit_pose = publication_position(pursuing, npc, publication);
    INFO("NPC pursuit pose=" << std::setprecision(17) << pursuit_pose.x << ',' << pursuit_pose.y
                             << ',' << pursuit_pose.z << " status="
                             << static_cast<unsigned>(pursuing.npcs.front().pursuit->status));
    CHECK(pursuing.npcs.front().pursuit->committed_steps > 0);
    CHECK(pursuing.npcs.front().pursuit->committed_distance > 3.25);
    CHECK(publication_position(pursuing, npc, publication).x > 1024.0);
    take(fixture.gateway->cancel_pursuit(npc, 1, fixture.base.now));

    // Return through the same authored seam and transfer the whole island back.
    // No source import/save, session reset or durable character replacement runs.
    for (unsigned step = 0; step < 20; ++step) {
        CHECK(move(-1000).status == RegionRouteStatus::admitted);
        tick();
        if (publication_position(fixture.image(), fixture.a, publication).x < 1023.0)
            break;
    }
    CHECK(publication_position(fixture.image(), fixture.a, publication).x < 1023.5);
    const auto return_cut = fixture.image();
    take(fixture.gateway->prepare_handoff(id<RegionInstanceId>(902), fixture.live(),
                                          fixture.base.now));
    fixture.commit();
    take(fixture.gateway->activate(fixture.base.now));
    const auto final = fixture.image();
    metrics.sample(fixture);
    for (const auto entity : {fixture.a, fixture.b, npc})
        CHECK(publication_position(final, entity, publication) ==
              publication_position(return_cut, entity, publication));
    for (const auto& writer : final.writers) {
        CHECK(writer.epoch == 4);
        CHECK(writer.instance == id<RegionInstanceId>(902));
    }
    observe();
    recipient.record(fixture);
    recipient.deliver(fixture.base.now + 1s);
    REQUIRE(recipient.effects.size() == 1);
    CHECK(recipient.effects.front().action_sequence == shot.action_sequence);
    CHECK(recipient.effects.front().outcome == CombatOutcome::hit);
    const auto faults = take(recipient.observations->stats());
    CHECK(faults.loss_drops > 0);
    CHECK(faults.duplicated_messages > 0);
    CHECK(faults.reordered_messages > 0);
    CHECK(faults.partition_drops > 0);
    CHECK(faults.backpressure == 0);
    fixture.settled();
    const auto completed =
        take(fixture.gateway->take_transfer(*fixture.base.pair.server, ticket, fixture.base.now));
    CHECK(completed.ticket == ticket);
    CHECK(completed.transfer == economic);
    CHECK(completed.writer == saved.writers.front());
    REQUIRE(completed.outcome);
    const auto economic_result = take(*completed.outcome);
    const auto& receipt = std::get<TransferResult>(economic_result).receipt;
    REQUIRE(receipt.source);
    REQUIRE(receipt.target);
    CHECK(receipt.source->state == EconomyState{93, 18});
    CHECK(receipt.target->state == EconomyState{107, 22});
    const auto delta = fixture.interest();
    CHECK(delta.stream == baseline.stream);
    for (const auto& change : delta.changes)
        CHECK(change.kind == ReplicationChange::update);
    take(replicas->apply(delta));
    const auto final_ids = take(replicas->snapshot());
    REQUIRE(final_ids.entities.size() == replica_ids.entities.size());
    for (std::size_t index = 0; index < final_ids.entities.size(); ++index)
        CHECK(final_ids.entities[index].id == replica_ids.entities[index].id);
    metrics.sample(fixture);
    CHECK(metrics.rss_samples >= 4);
    CHECK(metrics.logical_pending_peak > 0);
    std::cout << "connected-world cell=" << base_cell << " rss_peak_bytes=" << metrics.rss_peak
              << " rss_samples=" << metrics.rss_samples
              << " logical_resident_peak_bytes=" << metrics.logical_resident_peak
              << " logical_pending_peak_bytes=" << metrics.logical_pending_peak
              << " semantic_loss=" << faults.loss_drops
              << " semantic_duplicate=" << faults.duplicated_messages
              << " semantic_reorder=" << faults.reordered_messages
              << " semantic_partition=" << faults.partition_drops
              << " stale_observations=" << recipient.stale_observations
              << " quiescence_peak_ns=" << metrics.quiescence_peak.count()
              << " quiescence_over_budget="
              << (metrics.quiescence_peak > region_handoff_quiescence_budget) << '\n';
    content.verify_source();
    fixture.finish();
    content.complete = true;
    return observations;
}
} // namespace

TEST_CASE("Authored near and far seamless worlds stream through actual region workers",
          "[seamless-world]") {
    const auto near_world = connected_world(0);
    const auto far_world = connected_world(1'000'000'000);
    // Both workloads run identical local native geometry at very different
    // integer global cells; no enormous global floating-point coordinate forms.
    CHECK(far_world == near_world);
}

TEST_CASE("Pursuit checks actual target presence before the same native tick", "[seamless-world]") {
    ProcessFixture fixture;
    const auto initial = fixture.image();
    const auto npc = id<WorldEntityId>(40);
    take(fixture.gateway->set_pursuit(npc, 1, fixture.a, process_actor(initial, fixture.a).target,
                                      4.0, 1.05, fixture.base.now));
    const std::array<TlsTransport*, 1> only_b{fixture.base.peer->server.get()};
    fixture.base.now += 50ms;
    const auto detached = take(fixture.gateway->step(only_b, fixture.base.now));
    REQUIRE(detached.npcs.front().pursuit);
    CHECK(detached.npcs.front().pursuit->status == RegionPursuitStatus::target_unavailable);
    CHECK(detached.npcs.front().pursuit->committed_steps == 0);
    CHECK(process_body(detached, npc).velocity.linear.x == 0);
    CHECK(process_body(detached, npc).velocity.linear.z == 0);
    CHECK(process_actor(detached, fixture.a).detached);
    fixture.finish();
}
