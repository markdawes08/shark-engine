#include "shark/server/region_gateway.hpp"
#include "region_rpc_codec.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <thread>

namespace shark::server {
namespace {
using namespace rpc_detail;
std::mutex gateway_lease_mutex;
struct GatewayLease {
    storage::StoreId store;
    const storage::CharacterAuthority* authority;
};
std::vector<GatewayLease> gateway_leases;
WorldEntityId identity(const RegionImageEntity& value) {
    const auto* id = std::get_if<WorldEntityId>(&value.source.identity);
    if (!id)
        fail("Process image contains a non-world entity");
    return *id;
}
RegionId region_at(const RegionProcessImage& image, WorldEntityId entity) {
    const auto found = std::find_if(image.entities.begin(), image.entities.end(),
                                    [&](const auto& row) { return identity(row) == entity; });
    if (found == image.entities.end())
        fail("Writer entity is missing from the island");
    const auto& point = found->transform.translation;
    for (const auto& region : image.regions)
        if (point.x >= region.min_x && point.x < region.max_x && point.z >= region.min_z &&
            point.z < region.max_z)
            return region.id;
    fail("Writer pose is outside configured regional coverage");
}
} // namespace

struct RegionGateway::Impl {
    storage::CharacterAuthority& authority;
    const storage::StoreMetadata metadata;
    std::filesystem::path executable;
    std::unique_ptr<RegionJournal> journal;
    std::unique_ptr<RegionProcess> source, destination;
    std::unique_ptr<Scene> publication;
    std::unique_ptr<net::InterestServer> interest;
    RegionProcessImage last;
    std::optional<RegionProcessImage> staged;
    std::vector<storage::RegionTransferRequest> requests;
    std::optional<storage::RuntimeTicket> commit_ticket;
    std::vector<RegionPeer> bindings;
    std::span<net::TlsTransport* const> transports{};
    struct LiveScope {
        Impl& impl;
        LiveScope(Impl& value, std::span<net::TlsTransport* const> live) : impl(value) {
            if (!impl.transports.empty() || live.size() > movement_maximum_characters)
                fail("Gateway live transport borrow is reentrant or oversized");
            for (std::size_t i = 0; i < live.size(); ++i) {
                if (!live[i])
                    fail("Null live transport");
                for (std::size_t j = 0; j < i; ++j)
                    if (live[i] == live[j])
                        fail("Duplicate live transport");
            }
            impl.transports = live;
        }
        ~LiveScope() {
            impl.transports = {};
        }
        LiveScope(const LiveScope&) = delete;
        LiveScope& operator=(const LiveScope&) = delete;
    };
    RegionGatewayPhase phase{RegionGatewayPhase::active};
    std::thread::id owner{std::this_thread::get_id()};
    MonotonicTime clock{};
    std::optional<std::chrono::steady_clock::time_point> frozen_at;
    MonotonicTime last_quiescence{};
    using Input = std::variant<net::MovementInput, net::CombatAction>;
    struct Route {
        std::uint64_t ticket, ordinal, connection;
        Input input;
        std::vector<RegionPeer> before, after;
    };
    std::vector<Route> buffered, admitted;
    std::uint64_t next_ticket{0};
    bool publication_valid{true};
    bool ownership_uncertain{false};
    struct DurableRoute {
        storage::RuntimeTicket ticket;
        WorldEntityId entity;
    };
    std::vector<DurableRoute> durable;
    bool leased{false};

    Impl(storage::CharacterAuthority& auth, storage::StoreMetadata store_metadata,
         std::filesystem::path server, RegionProcessImage image)
        : authority(auth), metadata(store_metadata), executable(std::move(server)),
          last(std::move(image)), bindings(last.peers) {
        buffered.reserve(region_gateway_maximum_routes);
        admitted.reserve(region_gateway_maximum_routes);
        durable.reserve(storage::maximum_runtime_pending);
    }
    ~Impl() {
        const bool source_stopped = !source || static_cast<bool>(source->retire(clock)) ||
                                    static_cast<bool>(source->terminate());
        const bool destination_stopped = !destination ||
                                         static_cast<bool>(destination->retire(clock)) ||
                                         static_cast<bool>(destination->terminate());
        source.reset();
        destination.reset();
        // Failed fencing never makes a replacement gateway eligible implicitly.
        if (leased && source_stopped && destination_stopped) {
            const std::lock_guard lock{gateway_lease_mutex};
            std::erase_if(gateway_leases,
                          [&](const auto& lease) { return lease.authority == &authority; });
        }
    }
    void acquire_lease() {
        const std::lock_guard lock{gateway_lease_mutex};
        const auto store = metadata.id;
        if (gateway_leases.size() >= 64 ||
            std::any_of(gateway_leases.begin(), gateway_leases.end(),
                        [&](const auto& lease) { return lease.store == store; }))
            fail("Character authority already has a regional gateway owner");
        gateway_leases.push_back({store, &authority});
        leased = true;
    }
    void check(MonotonicTime now) {
        if (phase == RegionGatewayPhase::closed)
            fail("Gateway is closed");
        if (owner != std::this_thread::get_id() || now < clock || now.count() < 0)
            fail("Gateway requires its creating thread and monotonic clock");
        clock = now;
    }
    void observe() const {
        if (owner != std::this_thread::get_id())
            fail("Gateway requires its creating thread");
    }
    void start_quiescence() {
        if (!frozen_at)
            frozen_at = std::chrono::steady_clock::now();
    }
    MonotonicTime quiescence() const {
        return frozen_at ? std::chrono::duration_cast<MonotonicTime>(
                               std::chrono::steady_clock::now() - *frozen_at)
                         : last_quiescence;
    }
    const storage::RegionWriter& writer(WorldEntityId entity) const {
        const auto found = std::find_if(last.writers.begin(), last.writers.end(),
                                        [&](const auto& row) { return row.entity == entity; });
        if (found == last.writers.end())
            fail("No retained writer for this entity");
        return *found;
    }
    void current_writers(const RegionProcessImage& image) const {
        if (image.writers.empty() || image.writers.size() != bindings.size())
            fail("Incomplete island writer set");
        for (const auto& expected : image.writers) {
            const auto actual = need(authority.region_ownership(expected.character));
            if (!actual || actual->writer != expected)
                fail("Island has a stale durable writer");
        }
    }
    void create_publication() {
        if (last.regions.empty())
            fail("Island has no regional coverage");
        publication = need(Scene::create(RegionSceneScope{
            last.frame.origin.world,
            {last.regions.front().id, RegionInstanceId{need(generate_stable_id())}, 1}}));
        for (const auto& row : last.entities) {
            EntitySeed seed{identity(row)};
            seed.name = row.name;
            seed.transform = row.transform;
            seed.camera = row.camera;
            seed.mesh_renderer = row.mesh_renderer;
            seed.rigid_body = row.rigid_body;
            need(publication->create_entity(std::move(seed)));
        }
        for (const auto& row : last.entities)
            if (row.parent)
                need(publication->reparent(need(publication->find(identity(row))),
                                           need(publication->find(*row.parent)),
                                           ReparentMode::keep_local));
    }
    void validate_image(const RegionProcessImage& image, RegionInstanceId expected,
                        bool advancing) const {
        if (image.executor_instance != expected || image.frame != last.frame ||
            image.entities.size() != last.entities.size() || image.writers != last.writers ||
            image.physics.completed_tick != image.movement.completed_tick ||
            image.physics.completed_tick != image.combat.completed_tick ||
            image.physics.completed_tick != last.physics.completed_tick + (advancing ? 1U : 0U) ||
            image.route_watermark < last.route_watermark)
            fail("Worker image does not match its admitted incarnation/cut");
        for (const auto& row : last.entities) {
            const auto found =
                std::find_if(image.entities.begin(), image.entities.end(),
                             [&](const auto& item) { return identity(row) == identity(item); });
            if (found == image.entities.end() || found->parent != row.parent ||
                found->name != row.name || found->camera != row.camera ||
                found->mesh_renderer != row.mesh_renderer)
                fail("Worker changed fixed island topology or source components");
            if (found->rigid_body != row.rigid_body) {
                const auto old_actor = std::find_if(
                    last.combat.actors.begin(), last.combat.actors.end(), [&](const auto& actor) {
                        return actor.entity.identity == row.source.identity;
                    });
                const auto actor = std::find_if(
                    image.combat.actors.begin(), image.combat.actors.end(),
                    [&](const auto& item) { return item.entity.identity == row.source.identity; });
                if (old_actor == last.combat.actors.end() || actor == image.combat.actors.end() ||
                    actor->settings != old_actor->settings ||
                    found->rigid_body !=
                        (actor->body ? std::optional<RigidBody>{actor->settings} : std::nullopt))
                    fail("Worker changed a body outside its retained combat lifecycle");
                const auto native =
                    std::find_if(image.physics.bodies.begin(), image.physics.bodies.end(),
                                 [&](const auto& body) {
                                     return body.native.entity.identity == row.source.identity;
                                 });
                if ((actor->body &&
                     (native == image.physics.bodies.end() || native->source != *actor->body)) ||
                    (!actor->body && native != image.physics.bodies.end()))
                    fail("Combat body transition disagrees with the complete native image");
            }
        }
    }
    void publish_poses(const RegionProcessImage& image) {
        publication_valid = false;
        for (const auto& row : image.entities) {
            need(publication->set_transform(need(publication->find(identity(row))), row.transform));
            need(publication->set_rigid_body(need(publication->find(identity(row))),
                                             row.rigid_body));
        }
        publication_valid = true;
    }
    std::vector<RegionPeer> peers(MonotonicTime now) {
        std::vector<RegionPeer> result;
        result.reserve(bindings.size());
        for (auto& binding : bindings) {
            for (auto* transport : transports) {
                if (!transport ||
                    LocalGameplayAccess::connection_id(*transport) != binding.connection)
                    continue;
                const auto handle = need(publication->find(binding.entity));
                const auto current =
                    authority.authorize_entity(*transport, *publication, handle, now);
                if (current && current.value().session == binding.session.session &&
                    current.value().ownership_epoch == binding.session.ownership_epoch) {
                    binding.session = current.value();
                    result.push_back({binding.connection, current.value(), binding.entity});
                }
            }
        }
        return result;
    }
    const RegionPeer& peer(std::span<const RegionPeer> values, std::uint64_t connection) const {
        const auto found = std::find_if(values.begin(), values.end(), [&](const auto& row) {
            return row.connection == connection;
        });
        if (found == values.end())
            fail("No actual ready player transport/session for this route");
        return *found;
    }
    net::TlsTransport& transport(std::uint64_t connection) const {
        for (auto* value : transports)
            if (value && LocalGameplayAccess::connection_id(*value) == connection)
                return *value;
        fail("Original routed transport is unavailable");
    }
    void checkpoint(const RegionProcessImage& image) {
        const auto existing = need(journal->read(RegionJournalSlot::checkpoint));
        RegionJournalRecord record{metadata.id,
                                   metadata.world,
                                   image.executor_instance,
                                   image.physics.completed_tick,
                                   image.route_watermark,
                                   image.writers,
                                   {},
                                   need(encode_region_process_image(image))};
        const auto receipt = need(journal->publish(
            RegionJournalSlot::checkpoint, record,
            existing ? std::optional<ContentDigest>{existing->digest} : std::nullopt));
        if (!receipt.published || receipt.durability == FileDurability::directory_flush_failed)
            fail("Checkpoint publication needs reconciliation");
        // Publication state changes only after the complete file receipt.
        publish_poses(image);
        last = image;
    }
    std::vector<net::InterestEntry> catalog(net::CharacterId recipient) const {
        if (!publication_valid)
            fail("Recipient publication needs a complete checkpoint refresh");
        std::vector<net::InterestEntry> result;
        for (const auto& binding : bindings) {
            const auto actor = std::find_if(
                last.combat.actors.begin(), last.combat.actors.end(), [&](const auto& row) {
                    return row.session.character == binding.session.character;
                });
            const bool present = actor != last.combat.actors.end() && !actor->detached &&
                                 !actor->fenced && actor->health != 0;
            result.push_back({need(publication->find(binding.entity)),
                              net::InterestVisibility::public_pose, binding.session.character, 1,
                              true, binding.session.character == recipient, present});
        }
        for (const auto& npc : last.npcs)
            result.push_back({need(publication->find(npc.source.identity)),
                              net::InterestVisibility::public_pose,
                              {},
                              0,
                              true,
                              false,
                              true});
        return result;
    }
    Result<RegionRouteReceipt> route(Route route, MonotonicTime now) {
        return boundary<RegionRouteReceipt>([&] {
            if (phase != RegionGatewayPhase::active || !source || !admitted.empty())
                fail("Gateway is frozen or has an unresolved admitted route");
            current_writers(last);
            route.before = peers(now);
            const auto caller_connection = peer(route.before, route.connection).connection;
            if (last.route_watermark == std::numeric_limits<std::uint64_t>::max())
                fail("Gateway route ordinal exhausted");
            route.ordinal = last.route_watermark + 1;
            route.after = route.before;
            // All allocations and immutable input capture precede admission.
            const auto prepared = std::visit(
                [&](const auto& input) {
                    return source->prepare(route.ordinal, route.connection, input, route.before,
                                           now);
                },
                route.input);
            if (!prepared) {
                if (!source->channel_healthy())
                    phase = RegionGatewayPhase::recovery_required;
                throw Failure{prepared.error()};
            }
            admitted.push_back(std::move(route));
            auto& retained = admitted.back();
            const auto envelope =
                std::visit([](const auto& value) { return value.authority; }, retained.input);
            auto admission = authority.apply(transport(retained.connection),
                                             net::HeartbeatRequest{envelope}, now);
            if (!admission) {
                const auto error = admission.error();
                admitted.pop_back();
                const auto cancelled = source->cancel(now);
                if (!cancelled)
                    phase = RegionGatewayPhase::recovery_required;
                throw Failure{error};
            }
            for (auto& value : retained.after)
                if (value.connection == caller_connection)
                    value.session = admission.value();
            for (auto& binding : bindings)
                if (binding.connection == caller_connection)
                    binding.session = admission.value();
            const auto receipt_ticket = retained.ticket;
            auto result = std::visit(
                [&](const auto& input) {
                    return source->commit(retained.ordinal, retained.connection, input,
                                          retained.after, now);
                },
                retained.input);
            if (result) {
                try {
                    validate_image(result.value(), last.executor_instance, false);
                    if (result.value().route_watermark != retained.ordinal)
                        fail("Input reply omitted its exact route watermark");
                    checkpoint(result.value());
                    admitted.clear();
                    return RegionRouteReceipt{receipt_ticket, RegionRouteStatus::admitted};
                } catch (const Failure& error) {
                    phase = RegionGatewayPhase::recovery_required;
                    return RegionRouteReceipt{receipt_ticket, RegionRouteStatus::uncertain,
                                              error.error};
                } catch (...) {
                    phase = RegionGatewayPhase::recovery_required;
                    throw;
                }
            }
            phase = RegionGatewayPhase::recovery_required;
            return RegionRouteReceipt{receipt_ticket, RegionRouteStatus::uncertain, result.error()};
        });
    }
    template <class T>
    RegionRouteReceipt submit(net::TlsTransport& connection, const T& input, MonotonicTime now) {
        check(now);
        const auto connection_id = LocalGameplayAccess::connection_id(connection);
        const auto current = peers(now);
        static_cast<void>(peer(current, connection_id));
        if constexpr (std::is_same_v<T, net::MovementInput>)
            need(net::validate_movement_input(input));
        else
            need(net::validate_combat_action(input));
        if (next_ticket == std::numeric_limits<std::uint64_t>::max())
            fail("Gateway receipt space exhausted");
        if (phase != RegionGatewayPhase::active) {
            if (buffered.size() == region_gateway_maximum_routes)
                fail("Post-cut input buffer is full");
            const auto ticket = ++next_ticket;
            buffered.push_back({ticket, 0, connection_id, input, {}, {}});
            return {ticket, RegionRouteStatus::buffered};
        }
        return need(route({++next_ticket, 0, connection_id, input, {}, {}}, now));
    }
    void stage_image(RegionProcessImage image, RegionInstanceId instance, MonotonicTime now) {
        if (instance == image.executor_instance ||
            std::any_of(image.writers.begin(), image.writers.end(),
                        [&](const auto& owner_row) { return owner_row.instance == instance; }))
            fail("Replacement requires a fresh executor incarnation");
        image.executor_instance = instance;
        const auto bytes = need(encode_region_process_image(image));
        const auto digest = need(sha256(bytes));
        std::vector<storage::RegionTransferRequest> batch;
        for (const auto& owner_row : image.writers) {
            const auto actor = std::find_if(
                image.movement.actors.begin(), image.movement.actors.end(),
                [&](const auto& row) { return row.session.character == owner_row.character; });
            const auto combat_actor = std::find_if(
                image.combat.actors.begin(), image.combat.actors.end(),
                [&](const auto& row) { return row.session.character == owner_row.character; });
            const auto retained_input =
                combat_actor == image.combat.actors.end() ? 0 : combat_actor->movement.last_input;
            const auto player =
                std::find_if(bindings.begin(), bindings.end(), [&](const auto& row) {
                    return row.session.character == owner_row.character;
                });
            if (player == bindings.end())
                fail("Transfer omits its retained player binding");
            const auto current = peers(now);
            const auto current_peer =
                std::find_if(current.begin(), current.end(),
                             [&](const auto& row) { return row.connection == player->connection; });
            const auto sequence = current_peer == current.end()
                                      ? player->session.last_sequence
                                      : current_peer->session.last_sequence;
            batch.push_back(
                {storage::RegionTransferId{need(generate_stable_id())}, owner_row,
                 region_at(image, owner_row.entity), instance, image.physics.completed_tick,
                 actor == image.movement.actors.end() ? retained_input : actor->accepted,
                 actor == image.movement.actors.end() ? retained_input : actor->applied, sequence,
                 digest});
        }
        destination = need(RegionProcess::launch(executable));
        need(destination->stage(image, now));
        const auto prior = need(journal->read(RegionJournalSlot::staged));
        const auto receipt = need(
            journal->publish(RegionJournalSlot::staged,
                             {metadata.id, metadata.world, instance, image.physics.completed_tick,
                              image.route_watermark, image.writers, batch, bytes},
                             prior ? std::optional<ContentDigest>{prior->digest} : std::nullopt));
        if (!receipt.published || receipt.durability == FileDurability::directory_flush_failed)
            fail("Staged transfer publication needs reconciliation");
        staged = std::move(image);
        requests = std::move(batch);
        phase = RegionGatewayPhase::prepared;
    }
};

RegionGateway::RegionGateway(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
RegionGateway::~RegionGateway() = default;
Result<std::unique_ptr<RegionGateway>>
RegionGateway::create(storage::CharacterAuthority& authority, const std::filesystem::path& server,
                      const std::filesystem::path& directory, const RegionProcessImage& initial,
                      std::span<net::TlsTransport* const> transports, MonotonicTime now) {
    return boundary<std::unique_ptr<RegionGateway>>([&] {
        if (initial.physics.completed_tick != 0 || initial.route_watermark != 0 ||
            transports.size() > movement_maximum_characters)
            fail("New gateway requires a released tick-zero bootstrap image");
        const auto metadata = need(authority.metadata());
        auto impl = std::make_unique<Impl>(authority, metadata, server, initial);
        impl->check(now);
        impl->acquire_lease();
        const Impl::LiveScope live{*impl, transports};
        impl->create_publication();
        impl->current_writers(initial);
        const auto current = impl->peers(now);
        if (current.size() != initial.peers.size())
            fail("Bootstrap peer was not actually authorized");
        for (const auto& writer : initial.writers)
            if (writer.instance != initial.executor_instance)
                fail("Bootstrap writer has a different instance");
        impl->journal = need(RegionJournal::open(directory, metadata.id, metadata.world));
        if (need(impl->journal->read(RegionJournalSlot::checkpoint)) ||
            need(impl->journal->read(RegionJournalSlot::staged)))
            fail("Gateway journal is already populated");
        impl->interest = need(net::InterestServer::create());
        impl->source = need(RegionProcess::launch(server));
        need(impl->source->stage(initial, now));
        need(impl->source->activate(initial, now));
        auto installed = need(impl->source->inspect(now));
        impl->validate_image(installed, initial.executor_instance, false);
        impl->checkpoint(installed);
        return std::unique_ptr<RegionGateway>{new RegionGateway{std::move(impl)}};
    });
}
Result<RegionRouteReceipt> RegionGateway::submit(net::TlsTransport& transport,
                                                 const net::MovementInput& input,
                                                 MonotonicTime now) {
    return boundary<RegionRouteReceipt>([&] {
        impl_->check(now);
        auto* pointer = &transport;
        const Impl::LiveScope live{*impl_, std::span{&pointer, 1}};
        return impl_->submit(transport, input, now);
    });
}
Result<RegionRouteReceipt> RegionGateway::submit(net::TlsTransport& transport,
                                                 const net::CombatAction& input,
                                                 MonotonicTime now) {
    return boundary<RegionRouteReceipt>([&] {
        impl_->check(now);
        auto* pointer = &transport;
        const Impl::LiveScope live{*impl_, std::span{&pointer, 1}};
        return impl_->submit(transport, input, now);
    });
}
Result<std::vector<RegionRouteReceipt>>
RegionGateway::drain_routes(std::span<net::TlsTransport* const> transports, MonotonicTime now) {
    return boundary<std::vector<RegionRouteReceipt>>([&] {
        impl_->check(now);
        const Impl::LiveScope live{*impl_, transports};
        if (impl_->phase != RegionGatewayPhase::active)
            fail("Routes remain buffered while frozen");
        std::vector<RegionRouteReceipt> result;
        result.reserve(impl_->buffered.size());
        while (!impl_->buffered.empty() && impl_->phase == RegionGatewayPhase::active) {
            auto route = std::move(impl_->buffered.front());
            impl_->buffered.erase(impl_->buffered.begin());
            const auto ticket = route.ticket;
            auto receipt = impl_->route(std::move(route), now);
            result.push_back(
                receipt ? std::move(receipt).value()
                        : RegionRouteReceipt{ticket, RegionRouteStatus::rejected, receipt.error()});
        }
        return result;
    });
}
Result<RegionProcessImage> RegionGateway::step(std::span<net::TlsTransport* const> transports,
                                               MonotonicTime now) {
    return boundary<RegionProcessImage>([&] {
        impl_->check(now);
        const Impl::LiveScope live{*impl_, transports};
        if (impl_->phase != RegionGatewayPhase::active || !impl_->buffered.empty() ||
            !impl_->admitted.empty())
            fail("Gateway cannot tick with frozen or unresolved routes");
        impl_->current_writers(impl_->last);
        auto result = impl_->source->step(impl_->peers(now), now);
        if (!result) {
            impl_->phase = RegionGatewayPhase::recovery_required;
            throw Failure{result.error()};
        }
        try {
            impl_->validate_image(result.value(), impl_->last.executor_instance, true);
            impl_->checkpoint(result.value());
        } catch (...) {
            impl_->phase = RegionGatewayPhase::recovery_required;
            throw;
        }
        return impl_->last;
    });
}
Result<void> RegionGateway::prepare_handoff(RegionInstanceId destination,
                                            std::span<net::TlsTransport* const> transports,
                                            MonotonicTime now) {
    return boundary<void>([&] {
        impl_->check(now);
        const Impl::LiveScope live{*impl_, transports};
        if (impl_->phase != RegionGatewayPhase::active || !impl_->admitted.empty() ||
            !impl_->buffered.empty())
            fail("Handoff requires a complete acknowledged tick/input boundary");
        impl_->current_writers(impl_->last);
        impl_->phase = RegionGatewayPhase::recovery_required;
        impl_->start_quiescence();
        auto cut = need(impl_->source->freeze(now));
        cut.peers = impl_->peers(now);
        impl_->validate_image(cut, impl_->last.executor_instance, false);
        impl_->checkpoint(cut);
        impl_->stage_image(cut, destination, now);
    });
}
Result<storage::RuntimeTicket> RegionGateway::begin_commit(MonotonicTime deadline,
                                                           MonotonicTime now) {
    return boundary<storage::RuntimeTicket>([&] {
        impl_->check(now);
        if (impl_->phase != RegionGatewayPhase::prepared || !impl_->staged || impl_->commit_ticket)
            fail("No prepared immutable process transfer");
        if (!impl_->destination || !need(impl_->destination->alive())) {
            impl_->phase = RegionGatewayPhase::recovery_required;
            fail("Staged destination exited before writer commit");
        }
        const auto ticket =
            need(impl_->authority.submit_region_commits(impl_->requests, deadline, now));
        impl_->commit_ticket = ticket;
        impl_->phase = RegionGatewayPhase::committing;
        return ticket;
    });
}
Result<RegionGatewayPhase> RegionGateway::poll_commit(MonotonicTime now) {
    return boundary<RegionGatewayPhase>([&] {
        impl_->check(now);
        if (impl_->phase != RegionGatewayPhase::committing || !impl_->commit_ticket)
            fail("No retained original process commit ticket");
        auto completion = need(impl_->authority.take_region(*impl_->commit_ticket, now));
        if (!completion.outcome)
            return impl_->phase;
        impl_->commit_ticket.reset();
        if (!*completion.outcome &&
            (completion.outcome->error().code == ErrorCode::io_failure ||
             completion.outcome->error().code == ErrorCode::execution_failed ||
             completion.outcome->error().code == ErrorCode::invalid_configuration)) {
            impl_->ownership_uncertain = true;
            impl_->phase = RegionGatewayPhase::recovery_required;
            throw Failure{completion.outcome->error()};
        }
        bool committed = true, unchanged = true;
        for (const auto& request : impl_->requests) {
            const auto actual = need(impl_->authority.region_ownership(request.source.character));
            committed = committed && actual && actual->last_transfer == request &&
                        actual->writer.instance == request.destination_instance &&
                        actual->writer.epoch == request.source.epoch + 1;
            unchanged = unchanged && actual && actual->writer == request.source;
        }
        if (committed)
            impl_->phase = RegionGatewayPhase::committed;
        else {
            impl_->phase = RegionGatewayPhase::recovery_required;
            if (!unchanged)
                fail("Ownership completion is uncertain; retained source cannot resume");
            if (!*completion.outcome)
                throw Failure{completion.outcome->error()};
            fail("Batch completed without the exact all-owner successor receipts");
        }
        return impl_->phase;
    });
}
Result<void> RegionGateway::activate(MonotonicTime now) {
    return boundary<void>([&] {
        impl_->check(now);
        if (impl_->phase != RegionGatewayPhase::committed || !impl_->staged || !impl_->destination)
            fail("Destination requires the exact settled writer batch");
        impl_->phase = RegionGatewayPhase::recovery_required;
        auto proof = *impl_->staged;
        proof.writers.clear();
        for (const auto& request : impl_->requests) {
            const auto actual = need(impl_->authority.region_ownership(request.source.character));
            if (!actual || actual->last_transfer != request)
                fail("Activation writer receipt changed");
            proof.writers.push_back(actual->writer);
        }
        need(impl_->destination->activate(proof, now));
        auto installed = need(impl_->destination->inspect(now));
        // Validate against the committed image, then retain native destination refs.
        auto previous = impl_->last;
        impl_->last.writers = proof.writers;
        try {
            impl_->validate_image(installed, proof.executor_instance, false);
        } catch (...) {
            impl_->last = std::move(previous);
            throw;
        }
        impl_->checkpoint(installed);
        if (impl_->source) {
            const auto retired = impl_->source->retire(now);
            if (!retired && impl_->source->channel_healthy())
                throw Failure{retired.error()};
            if (!retired)
                need(impl_->source->terminate());
        }
        impl_->source = std::move(impl_->destination);
        impl_->staged.reset();
        impl_->requests.clear();
        // Reapply only original admitted routes not covered by the durable image.
        for (const auto& route : impl_->admitted) {
            if (route.ordinal <= impl_->last.route_watermark)
                continue;
            std::visit(
                [&](const auto& input) {
                    need(impl_->source->prepare(route.ordinal, route.connection, input,
                                                route.before, now));
                    auto image = need(impl_->source->commit(route.ordinal, route.connection, input,
                                                            route.after, now));
                    impl_->validate_image(image, impl_->last.executor_instance, false);
                    impl_->checkpoint(image);
                },
                route.input);
        }
        impl_->admitted.clear();
        impl_->phase = RegionGatewayPhase::active;
        impl_->last_quiescence = impl_->quiescence();
        impl_->frozen_at.reset();
    });
}
Result<void> RegionGateway::prepare_recovery(RegionInstanceId replacement,
                                             std::span<net::TlsTransport* const> transports,
                                             MonotonicTime now) {
    return boundary<void>([&] {
        impl_->check(now);
        const Impl::LiveScope live{*impl_, transports};
        if (impl_->commit_ticket)
            fail("Original SQL completion must settle before process recovery");
        if (impl_->ownership_uncertain)
            fail("Storage uncertainty requires the existing durable-owner maintenance "
                 "reconciliation");
        impl_->phase = RegionGatewayPhase::recovery_required;
        impl_->start_quiescence();
        if (impl_->source)
            need(impl_->source->terminate());
        if (impl_->destination)
            need(impl_->destination->terminate());
        impl_->source.reset();
        impl_->destination.reset();
        auto image = impl_->last;
        const auto persisted = need(impl_->journal->read(RegionJournalSlot::checkpoint));
        if (!persisted)
            fail("Recovery checkpoint is missing");
        auto disk = need(decode_region_process_image(persisted->record.payload));
        if (disk.physics.completed_tick >= image.physics.completed_tick &&
            disk.route_watermark >= image.route_watermark)
            image = std::move(disk);
        if (impl_->staged && !impl_->requests.empty()) {
            bool transferred = true;
            for (const auto& request : impl_->requests) {
                const auto actual =
                    need(impl_->authority.region_ownership(request.source.character));
                transferred = transferred && actual && actual->last_transfer == request;
            }
            if (transferred && image.executor_instance != impl_->staged->executor_instance)
                image = *impl_->staged;
        }
        image.writers.clear();
        for (const auto& binding : impl_->bindings) {
            const auto actual = need(impl_->authority.region_ownership(binding.session.character));
            if (!actual || actual->writer.entity != binding.entity)
                fail("Recovery writer is missing or foreign");
            image.writers.push_back(actual->writer);
        }
        image.peers = impl_->peers(now);
        impl_->publish_poses(image);
        impl_->last = image;
        impl_->staged.reset();
        impl_->requests.clear();
        impl_->stage_image(std::move(image), replacement, now);
    });
}
Result<void> RegionGateway::terminate_executor(RegionProcessRole role) {
    return boundary<void>([&] {
        impl_->observe();
        auto& child = role == RegionProcessRole::source ? impl_->source : impl_->destination;
        if (!child)
            fail("Requested executor is absent");
        impl_->start_quiescence();
        need(child->terminate());
        if (impl_->phase == RegionGatewayPhase::active)
            impl_->phase = RegionGatewayPhase::recovery_required;
    });
}
Result<void> RegionGateway::close(MonotonicTime now) {
    return boundary<void>([&] {
        impl_->observe();
        if (impl_->phase == RegionGatewayPhase::closed)
            return;
        impl_->check(now);
        impl_->phase = RegionGatewayPhase::recovery_required;
        if (impl_->source) {
            if (impl_->source->channel_healthy())
                need(impl_->source->retire(now));
            else
                need(impl_->source->terminate());
            impl_->source.reset();
        }
        if (impl_->destination) {
            if (impl_->destination->channel_healthy())
                need(impl_->destination->retire(now));
            else
                need(impl_->destination->terminate());
            impl_->destination.reset();
        }
        need(impl_->publication->close());
        impl_->phase = RegionGatewayPhase::closed;
    });
}
Result<RegionGatewayStats> RegionGateway::stats() const {
    return boundary<RegionGatewayStats>([&] {
        impl_->observe();
        const auto elapsed = impl_->quiescence();
        return RegionGatewayStats{impl_->phase,
                                  impl_->last.physics.completed_tick,
                                  impl_->last.route_watermark,
                                  impl_->buffered.size(),
                                  impl_->admitted.size(),
                                  impl_->source ? impl_->source->process_id() : 0,
                                  impl_->destination ? impl_->destination->process_id() : 0,
                                  elapsed,
                                  elapsed > region_handoff_quiescence_budget};
    });
}
Result<RegionProcessImage> RegionGateway::snapshot() const {
    return boundary<RegionProcessImage>([&] {
        impl_->observe();
        return impl_->last;
    });
}
Result<net::InterestPublishResult> RegionGateway::publish_interest(net::TlsTransport& transport,
                                                                   MonotonicTime now) {
    return boundary<net::InterestPublishResult>([&] {
        impl_->check(now);
        auto* pointer = &transport;
        const Impl::LiveScope live{*impl_, std::span{&pointer, 1}};
        const auto current = impl_->peers(now);
        const auto recipient =
            impl_->peer(current, LocalGameplayAccess::connection_id(transport)).session.character;
        return need(impl_->authority.publish_interest(*impl_->interest, *impl_->publication,
                                                      impl_->catalog(recipient), transport, now));
    });
}
Result<net::ReplicationControlResult>
RegionGateway::control_interest(net::TlsTransport& transport,
                                const net::ReplicationControl& control, MonotonicTime now) {
    return boundary<net::ReplicationControlResult>([&] {
        impl_->check(now);
        auto* pointer = &transport;
        const Impl::LiveScope live{*impl_, std::span{&pointer, 1}};
        const auto current = impl_->peers(now);
        const auto recipient =
            impl_->peer(current, LocalGameplayAccess::connection_id(transport)).session.character;
        const auto result = need(
            impl_->authority.control_interest(*impl_->interest, *impl_->publication,
                                              impl_->catalog(recipient), transport, control, now));
        static_cast<void>(impl_->peers(now));
        return result;
    });
}
Result<storage::RuntimeTicket>
RegionGateway::submit_transfer(net::TlsTransport& transport, const net::SessionEnvelope& envelope,
                               const storage::TransferRequest& request, MonotonicTime deadline,
                               MonotonicTime now) {
    return boundary<storage::RuntimeTicket>([&] {
        impl_->check(now);
        auto* pointer = &transport;
        const Impl::LiveScope live{*impl_, std::span{&pointer, 1}};
        if (impl_->phase != RegionGatewayPhase::active)
            fail("New durable intents wait until process activation");
        const auto current = impl_->peers(now);
        const auto& caller = impl_->peer(current, LocalGameplayAccess::connection_id(transport));
        if (impl_->durable.size() == storage::maximum_runtime_pending)
            fail("Durable route buffer is full");
        const auto entity = need(impl_->publication->find(caller.entity));
        const auto ticket = need(
            impl_->authority.submit_transfer(transport, envelope, *impl_->publication, entity,
                                             request, deadline, now, impl_->writer(caller.entity)));
        impl_->durable.push_back({ticket, caller.entity});
        static_cast<void>(impl_->peers(now));
        return ticket;
    });
}
Result<storage::RuntimeCompletion> RegionGateway::take_transfer(net::TlsTransport& transport,
                                                                storage::RuntimeTicket ticket,
                                                                MonotonicTime now) {
    return boundary<storage::RuntimeCompletion>([&] {
        impl_->check(now);
        auto* pointer = &transport;
        const Impl::LiveScope live{*impl_, std::span{&pointer, 1}};
        const auto route = std::find_if(impl_->durable.begin(), impl_->durable.end(),
                                        [&](const auto& row) { return row.ticket == ticket; });
        if (route == impl_->durable.end())
            fail("Unknown original durable route");
        const auto peers = impl_->peers(now);
        const auto& caller = impl_->peer(peers, LocalGameplayAccess::connection_id(transport));
        if (caller.entity != route->entity)
            fail("Original durable route belongs to another live player");
        // During a committed/frozen cut use the actual cached successor, while
        // preserving the original request, ticket and admission writer receipt.
        const auto& retained = impl_->writer(route->entity);
        const auto current = need(impl_->authority.region_ownership(retained.character));
        if (!current || current->writer.entity != route->entity)
            fail("Durable completion writer is unavailable");
        auto result = need(
            impl_->authority.take(ticket, transport, *impl_->publication, now, current->writer));
        if (result.outcome)
            impl_->durable.erase(route);
        return result;
    });
}
} // namespace shark::server
