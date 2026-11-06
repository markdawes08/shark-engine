#include "shark/server/regions.hpp"

#include "execution_cut_encoding.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>

namespace shark::server {
namespace {
struct Failure {
    Error error;
};
[[noreturn]] void fail(const char* message, ErrorCode code = ErrorCode::conflict) {
    throw Failure{Error{code, message, {}}};
}
template <class T> T require_value(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
void require_value(Result<void> result) {
    if (!result)
        throw Failure{std::move(result).error()};
}
template <class T, class F> Result<T> boundary(const char* operation, F&& function) {
    try {
        if constexpr (std::is_void_v<T>) {
            function();
            return {};
        } else
            return function();
    } catch (Failure& failure) {
        failure.error.context.operation = operation;
        return std::move(failure.error);
    } catch (...) {
        return Error{
            ErrorCode::execution_failed, "Co-located region operation failed", {operation}};
    }
}
std::uint64_t fresh_scope() {
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<std::uint64_t>::max())
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            return value;
    fail("Co-located region scope exhausted");
}
bool contains(const RegionDefinition& region, const math::Vec3& position) noexcept {
    return position.x >= region.min_x && position.x < region.max_x && position.z >= region.min_z &&
           position.z < region.max_z;
}
bool adjacent(const RegionDefinition& a, const RegionDefinition& b) noexcept {
    const bool vertical = (a.max_x == b.min_x || b.max_x == a.min_x) &&
                          std::max(a.min_z, b.min_z) < std::min(a.max_z, b.max_z);
    const bool horizontal = (a.max_z == b.min_z || b.max_z == a.min_z) &&
                            std::max(a.min_x, b.min_x) < std::min(a.max_x, b.max_x);
    return vertical || horizontal;
}
bool same_session(const net::SessionSnapshot& a, const net::SessionSnapshot& b) noexcept {
    return a.session == b.session && a.account == b.account && a.character == b.character &&
           a.ownership_epoch == b.ownership_epoch && b.ready && b.active;
}
using CutEncoder = detail::CutEncoder;
void blob(CutEncoder& out, std::span<const std::byte> bytes) {
    out.value(static_cast<std::uint64_t>(bytes.size()));
    out.bytes.insert(out.bytes.end(), bytes.begin(), bytes.end());
}
void text(CutEncoder& out, const std::string& value) {
    blob(out, std::as_bytes(std::span{value.data(), value.size()}));
}
void encode(CutEncoder& out, const storage::RegionWriter& value) {
    out.value(value.store);
    out.value(value.character);
    out.value(value.entity);
    out.value(value.region);
    out.value(value.instance);
    out.value(value.epoch);
}
void encode(CutEncoder& out, const storage::RegionTransferRequest& value) {
    out.value(value.transfer);
    encode(out, value.source);
    out.value(value.destination);
    out.value(value.destination_instance);
    out.value(value.cut_tick);
    out.value(value.accepted_input);
    out.value(value.applied_input);
    out.value(value.command_sequence);
    blob(out, value.cut_digest.bytes);
}
template <class T, class F>
void optional(CutEncoder& out, const std::optional<T>& value, F&& write) {
    out.value(value.has_value());
    if (value)
        write(*value);
}
void encode(CutEncoder& out, const storage::RegionOwnership& value) {
    encode(out, value.writer);
    optional(out, value.last_transfer, [&](const auto& transfer) { encode(out, transfer); });
}
void encode(CutEncoder& out, const storage::RegionTransferResult& value) {
    encode(out, value.request);
    encode(out, value.writer);
    out.value(value.replayed);
}
void encode(CutEncoder& out, const storage::EconomyState& value) {
    out.value(value.currency);
    out.value(value.item_units);
}
void encode(CutEncoder& out, const storage::EconomyRecord& value) {
    out.value(value.account);
    out.value(value.character);
    out.value(value.revision);
    encode(out, value.state);
}
void encode(CutEncoder& out, const storage::TransferRequest& value) {
    out.value(value.operation.store);
    out.value(value.operation.actor);
    out.value(value.operation.sequence);
    out.value(value.source);
    out.value(value.target);
    out.value(value.expected_source_revision);
    out.value(value.expected_target_revision);
    encode(out, value.quantity);
}
void encode(CutEncoder& out, const storage::TransferResult& value) {
    encode(out, value.receipt.request);
    out.value(value.receipt.status);
    optional(out, value.receipt.source, [&](const auto& row) { encode(out, row); });
    optional(out, value.receipt.target, [&](const auto& row) { encode(out, row); });
    out.value(value.replayed);
}
void encode(CutEncoder& out, const storage::CharacterRecord& value) {
    out.value(value.account);
    out.value(value.character);
    out.value(value.entity);
    out.value(value.revision);
    text(out, value.state.name.value);
    out.value(value.state.position);
}
void encode(CutEncoder& out, const Error& value) {
    out.value(value.code);
    text(out, value.message);
    text(out, value.context.operation);
    text(out, value.context.request_id);
    text(out, value.context.path);
    text(out, value.context.entity_id);
    text(out, value.context.asset_id);
}
void encode(CutEncoder& out, const storage::RuntimeTicket& value) {
    out.value(value.owner);
    out.value(value.sequence);
}
void encode(CutEncoder& out, const storage::RuntimePendingCut& value) {
    encode(out, value.ticket);
    out.value(value.session);
    out.value(value.entity);
    out.value(value.deadline);
    optional(out, value.transfer, [&](const auto& request) { encode(out, request); });
    optional(out, value.writer, [&](const auto& writer) { encode(out, writer); });
    optional(out, value.region_transfer, [&](const auto& request) { encode(out, request); });
    out.value(value.control);
    out.value(value.timed_out);
    optional(out, value.outcome, [&](const auto& outcome) {
        out.value(outcome.has_value());
        if (outcome) {
            out.value(static_cast<std::uint64_t>(outcome.value().index()));
            std::visit([&](const auto& result) { encode(out, result); }, outcome.value());
        } else
            encode(out, outcome.error());
    });
    optional(out, value.save, [&](const auto& save) {
        out.value(save.expected_revision);
        text(out, save.state.name.value);
        out.value(save.state.position);
    });
}
void encode(CutEncoder& out, const RegionGrant& value) {
    out.value(value.scope);
    out.value(value.entity);
    out.value(value.region);
    out.value(value.generation);
}
ContentDigest fingerprint(RegionCut& cut, const RegionHandoff& handoff) {
    CutEncoder out;
    out.value(std::uint32_t{3});
    out.value(handoff.id);
    encode(out, handoff.source);
    out.value(handoff.destination);
    out.value(handoff.destination_instance);
    out.value(cut.scope);
    out.value(cut.next_grant);
    out.value(cut.transitions);
    out.value(cut.clock);
    out.value(cut.execution);
    out.value(cut.frame.origin);
    out.value(cut.frame.generation);
    out.value(static_cast<std::uint64_t>(cut.cells.size()));
    for (const auto& cell : cut.cells) {
        out.value(cell.key.world);
        out.value(cell.key.cell.x);
        out.value(cell.key.cell.y);
        out.value(cell.key.cell.z);
        out.value(cell.resident_generation);
        out.value(cell.collision);
        out.value(cell.proxy);
    }
    blob(out, cut.movement.bytes);
    blob(out, cut.combat.bytes);
    out.value(static_cast<std::uint64_t>(cut.npcs.size()));
    for (const auto& npc : cut.npcs) {
        encode(out, npc.grant);
        out.value(npc.entity);
        out.value(npc.body);
        out.value(npc.native);
        out.value(npc.velocity);
        out.value(npc.pending);
    }
    out.value(cut.sessions);
    out.value(static_cast<std::uint64_t>(cut.runtime.size()));
    for (const auto& runtime : cut.runtime)
        encode(out, runtime);
    out.value(static_cast<std::uint64_t>(cut.routes.size()));
    for (const auto& route : cut.routes) {
        encode(out, route.ticket);
        out.value(route.entity);
        out.value(route.character);
    }
    out.value(static_cast<std::uint64_t>(cut.writers.size()));
    for (const auto& writer : cut.writers)
        encode(out, writer);
    out.value(static_cast<std::uint64_t>(cut.players.size()));
    for (const auto& actor : cut.players) {
        encode(out, actor.grant);
        out.value(actor.entity);
        out.value(actor.character);
        out.value(actor.binding.stream);
        out.value(actor.binding.target);
        out.value(actor.binding.movement);
        out.value(actor.session);
        out.value(actor.connection);
        out.value(actor.attached);
        optional(out, actor.writer, [&](const auto& writer) { encode(out, writer); });
    }
    if (out.bytes.size() > region_handoff_maximum_cut_bytes)
        fail("Regional cut exceeds its bounded encoded reservation");
    cut.encoded_bytes = out.bytes.size();
    return require_value(sha256(out.bytes));
}
bool terminal(RegionHandoffPhase phase) noexcept {
    return phase == RegionHandoffPhase::committed || phase == RegionHandoffPhase::aborted;
}
void validate(std::span<const RegionDefinition> regions, const net::InterestLimits& interest) {
    if (regions.size() < 2 || regions.size() > region_world_maximum_regions)
        fail("Co-located world requires two to eight regions", ErrorCode::invalid_configuration);
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const auto& region = regions[i];
        for (const auto value : {region.min_x, region.max_x, region.min_z, region.max_z})
            if (!std::isfinite(value) || std::abs(value) > physics_local_position_limit)
                fail("Region bounds must fit the shared native frame",
                     ErrorCode::invalid_configuration);
        if (region.min_x >= region.max_x || region.min_z >= region.max_z ||
            !std::isfinite(region.interest_overlap) ||
            region.interest_overlap < interest.exit_radius || region.interest_overlap > 8192)
            fail("Region bounds or interest overlap are invalid", ErrorCode::invalid_configuration);
        for (std::size_t j = 0; j < i; ++j) {
            const auto& other = regions[j];
            if (region.id == other.id ||
                (std::max(region.min_x, other.min_x) < std::min(region.max_x, other.max_x) &&
                 std::max(region.min_z, other.min_z) < std::min(region.max_z, other.max_z)))
                fail("Region identities and ownership interiors must not overlap",
                     ErrorCode::invalid_configuration);
        }
    }
    std::array<bool, region_world_maximum_regions> reached{};
    reached[0] = true;
    for (std::size_t pass = 0; pass < regions.size(); ++pass)
        for (std::size_t i = 0; i < regions.size(); ++i)
            if (reached[i])
                for (std::size_t j = 0; j < regions.size(); ++j)
                    if (adjacent(regions[i], regions[j]))
                        reached[j] = true;
    for (std::size_t i = 0; i < regions.size(); ++i)
        if (!reached[i])
            fail("Region ownership rectangles must be edge-connected",
                 ErrorCode::invalid_configuration);
}
} // namespace

struct RegionWorld::Impl {
    struct Actor {
        RegionGrant grant;
        EntityReference entity;
        net::CharacterId character;
        CombatBinding binding;
        net::SessionSnapshot session;
        std::uint64_t connection;
        bool attached;
        std::optional<storage::RegionWriter> writer{};
    };
    struct PendingTransfer {
        storage::RuntimeTicket ticket;
        WorldEntityId entity;
        net::CharacterId character;
    };
    struct Npc {
        RegionGrant grant;
        EntityReference entity;
        PhysicsBodyHandle body;
        RigidBody settings;
        std::optional<math::Vec3> pending{};
    };
    Scene& scene;
    PhysicsWorld& physics;
    const WorldTraversal& traversal;
    WorldLocalFrame frame;
    std::vector<RegionDefinition> regions;
    std::vector<MovementDomain> domains;
    net::InterestLimits interest_limits;
    std::unique_ptr<CharacterMovement> movement;
    std::unique_ptr<CharacterCombat> combat;
    std::unique_ptr<net::InterestServer> interest;
    std::shared_ptr<ExecutionFence> fence;
    ExecutionPermit permit;
    bool persistent;
    storage::CharacterAuthority* authority_owner{nullptr};
    const std::thread::id owner{std::this_thread::get_id()};
    const std::uint64_t scope{fresh_scope()};
    std::uint64_t next_grant{1};
    std::uint64_t transitions{0};
    MonotonicTime last_time{};
    bool faulted{false};
    std::vector<Actor> actors;
    std::vector<Npc> npcs;
    std::array<std::optional<PendingTransfer>, storage::maximum_runtime_pending> pending{};
    std::vector<RegionHandoff> handoffs;

    Impl(Scene& s, PhysicsWorld& p, const WorldTraversal& t, WorldLocalFrame f,
         std::vector<RegionDefinition> configured, net::InterestLimits limits,
         std::unique_ptr<CharacterMovement> moves, std::unique_ptr<CharacterCombat> fights,
         std::unique_ptr<net::InterestServer> publisher, std::shared_ptr<ExecutionFence> execution,
         ExecutionPermit captured, bool durable)
        : scene(s), physics(p), traversal(t), frame(f), regions(std::move(configured)),
          interest_limits(limits), movement(std::move(moves)), combat(std::move(fights)),
          interest(std::move(publisher)), fence(std::move(execution)), permit(captured),
          persistent(durable) {
        actors.reserve(movement_maximum_characters);
        npcs.reserve(region_world_maximum_npcs);
        handoffs.reserve(8);
        domains.reserve(regions.size());
        for (const auto& region : regions)
            domains.push_back({region.min_x, region.max_x, region.min_z, region.max_z});
    }
    void check(bool allow_frozen = false) const {
        if (owner != std::this_thread::get_id())
            fail("Region world requires its creating thread");
        if (faulted)
            fail("Region world has partial effects; reconcile and replace it");
        require_value(fence->check(permit, allow_frozen));
        require_value(traversal.check_context(scene, physics));
    }
    void owner_authority(storage::CharacterAuthority& supplied) {
        if (!persistent)
            return;
        if (authority_owner && authority_owner != &supplied)
            fail("Region world cannot replace its retained character authority");
        authority_owner = &supplied;
    }
    void control_clock(storage::CharacterAuthority& supplied, MonotonicTime now) {
        check(true);
        owner_authority(supplied);
        if (now < MonotonicTime::zero() || now < last_time)
            fail("Region world clock cannot regress");
        last_time = now;
    }
    const RegionDefinition& definition(RegionId id) const {
        const auto found = std::find_if(regions.begin(), regions.end(),
                                        [&](const auto& region) { return region.id == id; });
        if (found == regions.end())
            fail("Region identity is outside this simulation island");
        return *found;
    }
    MovementDomain corridor(RegionId id) const {
        const auto& region = definition(id);
        return {std::max(-physics_local_position_limit, region.min_x - region.interest_overlap),
                std::min(physics_local_position_limit, region.max_x + region.interest_overlap),
                std::max(-physics_local_position_limit, region.min_z - region.interest_overlap),
                std::min(physics_local_position_limit, region.max_z + region.interest_overlap)};
    }
    void writer(const Actor& actor, storage::CharacterAuthority& supplied) const {
        if (!persistent)
            return;
        if (authority_owner != &supplied || !actor.writer)
            fail("Persistent regional writer belongs to another authority");
        const auto current = require_value(supplied.region_ownership(actor.character));
        if (!current || current->writer != *actor.writer)
            fail("Persistent regional writer is stale");
    }
    RegionHandoff& handoff(storage::RegionTransferId id) {
        const auto found = std::find_if(handoffs.begin(), handoffs.end(),
                                        [&](const auto& record) { return record.id == id; });
        if (found == handoffs.end())
            fail("Regional handoff is not retained");
        return *found;
    }
    void budget(RegionHandoff& record, MonotonicTime now) {
        if (record.cut_at && !record.settled_at &&
            now - *record.cut_at > region_handoff_quiescence_budget) {
            record.budget_exceeded = true;
            if (record.phase != RegionHandoffPhase::rejected)
                record.phase = RegionHandoffPhase::reconciliation_required;
        }
    }
    void cut_readiness(const RegionCut& cut, const Actor& actor) const {
        if (traversal.frame() != cut.frame)
            fail("Regional handoff traversal frame changed after cut");
        const auto cells = require_value(traversal.cells());
        if (cells.size() != cut.cells.size() ||
            !std::equal(cells.begin(), cells.end(), cut.cells.begin(),
                        [](const auto& current, const auto& captured) {
                            return current.key == captured.key &&
                                   current.resident_generation == captured.resident_generation &&
                                   current.collision == captured.collision &&
                                   current.proxy == captured.proxy;
                        }))
            fail("Regional handoff traversal attachments changed after cut");
        const auto captured =
            std::find_if(cut.combat.actors.begin(), cut.combat.actors.end(),
                         [&](const auto& value) { return value.entity == actor.entity; });
        if (captured == cut.combat.actors.end() ||
            captured->settings.shape != ColliderShape::sphere)
            fail("Regional handoff requires its retained character footprint");
        const auto state = require_value(scene.entity_state(actor.entity.handle));
        if (state.reference != actor.entity)
            fail("Regional handoff character generation changed");
        const math::Vec3 local{state.world_matrix[3]};
        const auto position =
            require_value(translate_world_position(cut.frame.origin, {local.x, local.y, local.z}));
        // The retained primitive settings also cover dead lives without a body.
        if (!require_value(traversal.allows_motion(position, position, captured->settings.radius)))
            fail("Regional handoff character footprint is not traversal-ready");
    }
    void clock(MonotonicTime now) {
        check();
        if (now < MonotonicTime::zero() || now < last_time)
            fail("Region world clock cannot regress");
        last_time = now;
    }
    const RegionDefinition& locate(const math::Vec3& position) const {
        const auto found = std::find_if(regions.begin(), regions.end(), [&](const auto& region) {
            return contains(region, position);
        });
        if (found == regions.end())
            fail("Committed actor is outside the configured region union");
        return *found;
    }
    Actor& lookup(const RegionGrant& grant) {
        if (grant.scope != scope || grant.generation == 0)
            fail("Region grant is stale or foreign");
        const auto found = std::find_if(actors.begin(), actors.end(),
                                        [&](const auto& actor) { return actor.grant == grant; });
        if (found == actors.end())
            fail("Region grant is stale or foreign");
        return *found;
    }
    const Actor& lookup(const RegionGrant& grant) const {
        return const_cast<Impl*>(this)->lookup(grant);
    }
    Npc& lookup_npc(const RegionGrant& grant) {
        if (grant.scope != scope || grant.generation == 0)
            fail("NPC region grant is stale or foreign");
        const auto found = std::find_if(npcs.begin(), npcs.end(),
                                        [&](const auto& npc) { return npc.grant == grant; });
        if (found == npcs.end())
            fail("NPC region grant is stale or foreign");
        return *found;
    }
    PhysicsBodyState npc_body(const Npc& npc) const {
        require_value(scene.resolve(npc.entity));
        const auto body = require_value(physics.body_state(scene, npc.body));
        if (body.entity != npc.entity || body.geometry != PhysicsBodyGeometry::primitive ||
            !body.settings || *body.settings != npc.settings ||
            body.motion != BodyMotion::dynamic_body)
            fail("NPC body identity, generation or primitive profile changed");
        return body;
    }
    static double npc_radius(const PhysicsBodyState& body) {
        const auto radius = body.settings && body.settings->shape == ColliderShape::sphere
                                ? body.collider_half_extents.x
                                : glm::length(body.collider_half_extents);
        if (!std::isfinite(radius) || radius <= 0 || radius > physics_maximum_dimension)
            fail("NPC primitive exceeds the bounded traversal footprint");
        return radius;
    }
    net::SessionSnapshot authorize(const Actor& actor, storage::CharacterAuthority& authority,
                                   net::TlsTransport& transport, MonotonicTime now,
                                   bool require_original_session = true) const {
        writer(actor, authority);
        auto session =
            require_value(authority.authorize_entity(transport, scene, actor.entity.handle, now));
        if (session.character != actor.character ||
            (require_original_session && !same_session(actor.session, session)))
            fail("Region grant belongs to another character");
        require_value(scene.resolve(actor.entity));
        return session;
    }
    RegionGrant issue(WorldEntityId entity, RegionId region) {
        if (next_grant == std::numeric_limits<std::uint64_t>::max())
            fail("Region grant sequence exhausted");
        return {scope, entity, region, next_grant++};
    }
    std::vector<EntityReference> active() const {
        return require_value(combat->active_entities(scene, physics));
    }
    bool live(const Actor& actor, std::span<const EntityReference> active_entities) const {
        return std::find(active_entities.begin(), active_entities.end(), actor.entity) !=
               active_entities.end();
    }
    std::vector<net::InterestEntry> catalog(const Actor& observer) const {
        const auto active_entities = active();
        std::vector<net::InterestEntry> result;
        result.reserve(actors.size() + npcs.size());
        for (const auto& actor : actors) {
            net::InterestEntry row{actor.entity.handle,
                                   net::InterestVisibility::public_pose,
                                   actor.character,
                                   0,
                                   true,
                                   actor.character == observer.character};
            row.present = live(actor, active_entities);
            result.push_back(row);
        }
        for (const auto& npc : npcs) {
            static_cast<void>(npc_body(npc));
            result.push_back({npc.entity.handle, net::InterestVisibility::public_pose, std::nullopt,
                              0, true, false, true});
        }
        return result;
    }
};

RegionWorld::RegionWorld(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
RegionWorld::~RegionWorld() = default;
Result<std::unique_ptr<RegionWorld>>
RegionWorld::create(Scene& scene, PhysicsWorld& physics, WorldLocalFrame frame,
                    const WorldTraversal& traversal, std::unique_ptr<CombatRules> rules,
                    std::span<const RegionDefinition> regions, net::InterestLimits interest,
                    bool persistent_ownership) {
    return boundary<std::unique_ptr<RegionWorld>>("server.regions.create", [&] {
        require_value(traversal.check_context(scene, physics));
        if (frame != traversal.frame())
            fail("Region world requires the traversal's exact local frame");
        validate(regions, interest);
        std::vector<RegionDefinition> configured(regions.begin(), regions.end());
        auto publisher = require_value(net::InterestServer::create(interest));
        auto fence = require_value(ExecutionFence::create());
        const auto permit = require_value(fence->permit());
        std::array<MovementDomain, region_world_maximum_regions> domains{};
        for (std::size_t i = 0; i < regions.size(); ++i)
            domains[i] = {regions[i].min_x, regions[i].max_x, regions[i].min_z, regions[i].max_z};
        auto movement = require_value(CharacterMovement::create(
            scene, physics, frame.origin, traversal.instance(),
            std::span<const MovementDomain>{domains}.first(regions.size()), fence));
        auto combat =
            require_value(CharacterCombat::create(scene, physics, std::move(rules), fence));
        return std::unique_ptr<RegionWorld>(new RegionWorld{std::make_unique<Impl>(
            scene, physics, traversal, frame, std::move(configured), interest, std::move(movement),
            std::move(combat), std::move(publisher), std::move(fence), permit,
            persistent_ownership)});
    });
}
Result<RegionBinding> RegionWorld::bind(storage::CharacterAuthority& authority,
                                        net::TlsTransport& transport, EntityHandle entity,
                                        MonotonicTime now) {
    return boundary<RegionBinding>("server.regions.bind", [&] {
        impl_->clock(now);
        impl_->owner_authority(authority);
        if (impl_->actors.size() == movement_maximum_characters)
            fail("Region actor inventory is full");
        const auto reference = require_value(impl_->scene.reference(entity));
        const auto* identity = std::get_if<WorldEntityId>(&reference.identity);
        if (!identity)
            fail("Region actors require durable world entity identities");
        if (std::any_of(impl_->actors.begin(), impl_->actors.end(),
                        [&](const auto& actor) { return actor.grant.entity == *identity; }))
            fail("Region actor is already retained; reconnect explicitly");
        if (std::any_of(impl_->npcs.begin(), impl_->npcs.end(),
                        [&](const auto& npc) { return npc.grant.entity == *identity; }))
            fail("Region entity is already retained as an NPC");
        const auto state = require_value(impl_->scene.entity_state(entity));
        const auto& region = impl_->locate(math::Vec3{state.world_matrix[3]});
        const auto session =
            require_value(authority.authorize_entity(transport, impl_->scene, entity, now));
        std::optional<storage::RegionWriter> writer;
        if (impl_->persistent) {
            const auto cached = require_value(authority.region_ownership(session.character));
            if (!cached || cached->writer.entity != *identity ||
                cached->writer.region != region.id || cached->writer.character != session.character)
                fail("Persistent actor requires its exact preclaimed regional writer");
            writer = cached->writer;
        }
        const auto peer = require_value(transport.authenticated_peer());
        const auto grant = impl_->issue(*identity, region.id);
        auto binding = require_value(impl_->combat->bind(
            authority, transport, *impl_->movement, impl_->scene, impl_->physics, entity, now));
        if (writer)
            require_value(impl_->movement->set_actor_domain(binding.movement.stream,
                                                            impl_->corridor(writer->region)));
        impl_->actors.push_back({grant, reference, session.character, binding, session,
                                 peer.connection_id, true, writer});
        return RegionBinding{grant, std::move(binding), writer};
    });
}
Result<RegionGrant> RegionWorld::current_grant(WorldEntityId entity) const {
    return boundary<RegionGrant>("server.regions.grant", [&] {
        impl_->check();
        const auto found =
            std::find_if(impl_->actors.begin(), impl_->actors.end(),
                         [&](const auto& actor) { return actor.grant.entity == entity; });
        if (found != impl_->actors.end())
            return found->grant;
        const auto npc =
            std::find_if(impl_->npcs.begin(), impl_->npcs.end(),
                         [&](const auto& value) { return value.grant.entity == entity; });
        if (npc == impl_->npcs.end())
            fail("Region actor is not retained");
        return npc->grant;
    });
}
Result<std::vector<RegionActor>> RegionWorld::actors() const {
    return boundary<std::vector<RegionActor>>("server.regions.actors", [&] {
        impl_->check();
        const auto active = impl_->active();
        std::vector<RegionActor> result;
        result.reserve(impl_->actors.size());
        for (const auto& actor : impl_->actors)
            result.push_back({actor.grant, actor.entity, actor.character, actor.binding.target,
                              impl_->live(actor, active)});
        return result;
    });
}
Result<RegionGrant> RegionWorld::bind_npc(EntityHandle entity) {
    return boundary<RegionGrant>("server.regions.npc.bind", [&] {
        impl_->check();
        if (impl_->npcs.size() == region_world_maximum_npcs)
            fail("Region NPC inventory is full");
        const auto state = require_value(impl_->scene.entity_state(entity));
        const auto* identity = std::get_if<WorldEntityId>(&state.reference.identity);
        if (!identity || state.parent || !state.rigid_body ||
            state.rigid_body->motion != BodyMotion::dynamic_body)
            fail("Region NPC requires a world-identified root dynamic primitive");
        if (std::any_of(impl_->actors.begin(), impl_->actors.end(),
                        [&](const auto& actor) { return actor.grant.entity == *identity; }) ||
            std::any_of(impl_->npcs.begin(), impl_->npcs.end(),
                        [&](const auto& npc) { return npc.grant.entity == *identity; }))
            fail("Region entity is already retained");
        const auto body = require_value(impl_->physics.find_body(impl_->scene, state.reference));
        const auto native = require_value(impl_->physics.body_state(impl_->scene, body));
        if (native.entity != state.reference || !native.settings ||
            native.geometry != PhysicsBodyGeometry::primitive ||
            native.motion != BodyMotion::dynamic_body)
            fail("Region NPC requires its exact live primitive body");
        const auto radius = Impl::npc_radius(native);
        const auto position = require_value(translate_world_position(
            impl_->frame.origin,
            {native.pose.translation.x, native.pose.translation.y, native.pose.translation.z}));
        if (!require_value(impl_->traversal.allows_motion(position, position, radius)))
            fail("Region NPC registration requires a ready content footprint");
        const auto& region = impl_->locate(native.pose.translation);
        const auto grant = impl_->issue(*identity, region.id);
        impl_->npcs.push_back({grant, state.reference, body, *native.settings, {}});
        return grant;
    });
}
Result<void> RegionWorld::submit_npc_velocity(const RegionGrant& grant,
                                              math::Vec3 horizontal_velocity) {
    return boundary<void>("server.regions.npc.velocity", [&] {
        impl_->check();
        auto& npc = impl_->lookup_npc(grant);
        static_cast<void>(impl_->npc_body(npc));
        if (npc.pending)
            fail("NPC already has a pending velocity intent for this tick");
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(horizontal_velocity[axis]) ||
                std::abs(horizontal_velocity[axis]) > movement_speed)
                fail("NPC velocity must be finite and bounded");
        if (horizontal_velocity.y != 0 || horizontal_velocity.x * horizontal_velocity.x +
                                                  horizontal_velocity.z * horizontal_velocity.z >
                                              movement_speed * movement_speed)
            fail("NPC intent is horizontal and cannot exceed movement speed");
        for (int axis = 0; axis < 3; ++axis)
            if (horizontal_velocity[axis] == 0)
                horizontal_velocity[axis] = 0;
        npc.pending = horizontal_velocity;
    });
}
Result<std::vector<RegionNpcActor>> RegionWorld::npcs() const {
    return boundary<std::vector<RegionNpcActor>>("server.regions.npcs", [&] {
        impl_->check();
        std::vector<RegionNpcActor> result;
        result.reserve(impl_->npcs.size());
        for (const auto& npc : impl_->npcs) {
            static_cast<void>(impl_->npc_body(npc));
            result.push_back({npc.grant, npc.entity, npc.body});
        }
        return result;
    });
}
Result<RegionId> RegionWorld::region_at(WorldPosition position) const {
    return boundary<RegionId>("server.regions.locate", [&] {
        impl_->check();
        const auto local = require_value(world_position_relative_to(position, impl_->frame.origin,
                                                                    physics_local_position_limit));
        return impl_->locate({local[0], local[1], local[2]}).id;
    });
}
Result<void> RegionWorld::submit_movement(const RegionGrant& grant,
                                          storage::CharacterAuthority& authority,
                                          net::TlsTransport& transport,
                                          const net::MovementInput& input, MonotonicTime now) {
    return boundary<void>("server.regions.movement", [&] {
        impl_->clock(now);
        const auto& actor = impl_->lookup(grant);
        static_cast<void>(impl_->authorize(actor, authority, transport, now));
        if (input.stream != actor.binding.movement.stream)
            fail("Movement stream does not match its regional actor");
        require_value(impl_->movement->submit(authority, transport, impl_->scene, impl_->physics,
                                              input, now));
    });
}
Result<void> RegionWorld::submit_combat(const RegionGrant& grant,
                                        storage::CharacterAuthority& authority,
                                        net::TlsTransport& transport,
                                        const net::CombatAction& action, MonotonicTime now) {
    return boundary<void>("server.regions.combat", [&] {
        impl_->clock(now);
        const auto& actor = impl_->lookup(grant);
        static_cast<void>(impl_->authorize(actor, authority, transport, now));
        if (action.stream != actor.binding.stream)
            fail("Combat stream does not match its regional actor");
        require_value(
            impl_->combat->submit(authority, transport, impl_->scene, impl_->physics, action, now));
    });
}
Result<RegionTick> RegionWorld::step(storage::CharacterAuthority& authority,
                                     std::span<net::TlsTransport* const> transports,
                                     MonotonicTime now, ScriptRuntime* scripts) {
    return boundary<RegionTick>("server.regions.step", [&] {
        impl_->clock(now);
        impl_->owner_authority(authority);
        for (const auto& actor : impl_->actors)
            impl_->writer(actor, authority);
        if (transports.size() > movement_maximum_characters ||
            std::any_of(transports.begin(), transports.end(),
                        [](const auto* transport) { return transport == nullptr; }))
            fail("Region tick requires a bounded live transport inventory");
        const auto maximum_transitions = impl_->actors.size() + impl_->npcs.size();
        if (impl_->next_grant > std::numeric_limits<std::uint64_t>::max() - maximum_transitions ||
            impl_->transitions > std::numeric_limits<std::uint64_t>::max() - maximum_transitions)
            fail("Region grant capacity cannot admit another tick");
        std::array<std::uint64_t, movement_maximum_characters> connections{};
        for (std::size_t i = 0; i < transports.size(); ++i) {
            const auto peer = transports[i]->authenticated_peer();
            if (!peer)
                continue;
            const auto previous = connections.begin() + static_cast<std::ptrdiff_t>(i);
            if (std::find(connections.begin(), previous, peer.value().connection_id) != previous)
                fail("Region tick cannot contain duplicate live transports");
            connections[i] = peer.value().connection_id;
        }
        std::array<bool, movement_maximum_characters> attached{};
        for (std::size_t i = 0; i < impl_->actors.size(); ++i) {
            const auto& actor = impl_->actors[i];
            for (auto* transport : transports) {
                if (!transport)
                    continue;
                const auto peer = transport->authenticated_peer();
                if (!peer || peer.value().connection_id != actor.connection)
                    continue;
                const auto session =
                    authority.authorize_entity(*transport, impl_->scene, actor.entity.handle, now);
                attached[i] = session && same_session(actor.session, session.value());
                break;
            }
        }
        RegionTick report{};
        report.transitions.reserve(maximum_transitions);
        report.gameplay.movement.completed_tick =
            require_value(impl_->movement->stats()).completed_ticks;
        struct NpcPlan {
            PhysicsBodyVelocity velocity{};
            math::Vec3 from{};
            double radius{0};
        };
        std::array<NpcPlan, region_world_maximum_npcs> plans{};
        // Every NPC candidate is checked before the first native velocity write.
        for (std::size_t i = 0; i < impl_->npcs.size(); ++i) {
            const auto& npc = impl_->npcs[i];
            const auto body = impl_->npc_body(npc);
            static_cast<void>(impl_->locate(body.pose.translation));
            auto& plan = plans[i];
            plan.from = body.pose.translation;
            plan.radius = Impl::npc_radius(body);
            plan.velocity = require_value(impl_->physics.velocity(impl_->scene, npc.body));
            const auto horizontal = npc.pending.value_or(math::Vec3{});
            plan.velocity.linear.x = horizontal.x;
            plan.velocity.linear.z = horizontal.z;
            plan.velocity.linear.y =
                std::clamp(plan.velocity.linear.y, -movement_maximum_vertical_speed,
                           movement_maximum_vertical_speed);
            const auto intended =
                plan.from + plan.velocity.linear / static_cast<double>(movement_tick_hz);
            const auto from = require_value(translate_world_position(
                impl_->frame.origin, {plan.from.x, plan.from.y, plan.from.z}));
            const auto to = require_value(translate_world_position(
                impl_->frame.origin, {intended.x, intended.y, intended.z}));
            const auto covered =
                require_value(movement_coverage(impl_->domains, plan.from, intended));
            const auto ready = require_value(impl_->traversal.allows_motion(from, to, plan.radius));
            if (!covered || !ready) {
                plan.velocity.linear.x = 0;
                plan.velocity.linear.z = 0;
            }
        }
        impl_->faulted = true;
        for (std::size_t i = 0; i < impl_->npcs.size(); ++i) {
            auto written =
                impl_->physics.set_velocity(impl_->scene, impl_->npcs[i].body, plans[i].velocity);
            if (!written) {
                report.error = std::move(written).error();
                return report;
            }
            ++report.npc_velocity_writes;
        }
        auto stepped =
            impl_->combat->step(authority, *impl_->movement, impl_->scene, impl_->physics,
                                transports, now, scripts, &impl_->traversal);
        if (!stepped) {
            const auto movement = impl_->movement->stats();
            const auto combat = impl_->combat->stats();
            impl_->faulted = report.npc_velocity_writes != 0 || !movement || !combat ||
                             movement.value().faulted || combat.value().faulted;
            if (report.npc_velocity_writes != 0) {
                if (movement) {
                    report.gameplay.movement_completed =
                        movement.value().completed_ticks > report.gameplay.movement.completed_tick;
                    report.gameplay.movement.completed_tick = movement.value().completed_ticks;
                }
                report.error = std::move(stepped).error();
                return report;
            }
            throw Failure{std::move(stepped).error()};
        }
        report.gameplay = std::move(stepped).value();
        if (report.gameplay.error) {
            report.error = report.gameplay.error;
            return report;
        }
        try {
            // Complete membership candidates precede grant publication. Native
            // state already committed; any failure below retains that report.
            std::array<std::optional<RegionId>, movement_maximum_characters> regions{};
            std::array<std::optional<RegionId>, region_world_maximum_npcs> npc_regions{};
            for (std::size_t i = 0; i < impl_->actors.size(); ++i) {
                const auto state =
                    require_value(impl_->scene.entity_state(impl_->actors[i].entity.handle));
                regions[i] = impl_->locate(math::Vec3{state.world_matrix[3]}).id;
                if (impl_->persistent) {
                    const auto domain = impl_->corridor(impl_->actors[i].grant.region);
                    const auto position = math::Vec3{state.world_matrix[3]};
                    if (!require_value(
                            movement_coverage(std::span{&domain, 1}, position, position)))
                        fail("Committed actor left its source ownership overlap");
                    regions[i] = impl_->actors[i].grant.region;
                }
                impl_->actors[i].attached = attached[i];
            }
            for (std::size_t i = 0; i < impl_->npcs.size(); ++i) {
                const auto body = impl_->npc_body(impl_->npcs[i]);
                npc_regions[i] = impl_->locate(body.pose.translation).id;
                const auto from = require_value(translate_world_position(
                    impl_->frame.origin, {plans[i].from.x, plans[i].from.y, plans[i].from.z}));
                const auto to = require_value(translate_world_position(
                    impl_->frame.origin,
                    {body.pose.translation.x, body.pose.translation.y, body.pose.translation.z}));
                if (!require_value(
                        movement_coverage(impl_->domains, plans[i].from, body.pose.translation)) ||
                    !require_value(impl_->traversal.allows_motion(from, to, plans[i].radius)))
                    fail("NPC native movement left the ready regional coverage");
            }
            for (auto& actor : impl_->actors) {
                for (const auto& rebound : report.gameplay.rebindings)
                    if (actor.binding.target == rebound.previous)
                        actor.binding = rebound.binding;
                if (impl_->persistent && actor.writer)
                    for (const auto& rebound : report.gameplay.rebindings)
                        if (actor.binding.target == rebound.binding.target)
                            require_value(impl_->movement->set_actor_domain(
                                actor.binding.movement.stream,
                                impl_->corridor(actor.writer->region)));
                for (const auto& delivery : report.gameplay.movement.deliveries)
                    if (actor.binding.movement.stream == delivery.state.stream)
                        actor.binding.movement = delivery.state;
            }
            for (std::size_t i = 0; i < impl_->actors.size(); ++i) {
                auto& actor = impl_->actors[i];
                if (actor.grant.region == *regions[i])
                    continue;
                const auto before = actor.grant;
                actor.grant = impl_->issue(before.entity, *regions[i]);
                report.transitions.push_back({before, actor.grant});
                ++impl_->transitions;
            }
            for (std::size_t i = 0; i < impl_->npcs.size(); ++i) {
                auto& npc = impl_->npcs[i];
                npc.pending.reset();
                if (npc.grant.region == *npc_regions[i])
                    continue;
                const auto before = npc.grant;
                npc.grant = impl_->issue(before.entity, *npc_regions[i]);
                report.transitions.push_back({before, npc.grant});
                ++impl_->transitions;
            }
        } catch (Failure& failure) {
            failure.error.context.operation = "server.regions.step";
            report.error = std::move(failure.error);
            return report;
        } catch (...) {
            report.error = Error{ErrorCode::execution_failed,
                                 "Region publication failed after gameplay",
                                 {"server.regions.step"}};
            return report;
        }
        impl_->faulted = false;
        return report;
    });
}
Result<net::CombatState> RegionWorld::view(const RegionGrant& grant,
                                           storage::CharacterAuthority& authority,
                                           net::TlsTransport& transport, MonotonicTime now) const {
    return boundary<net::CombatState>("server.regions.view", [&] {
        impl_->clock(now);
        const auto& observer = impl_->lookup(grant);
        static_cast<void>(impl_->authorize(observer, authority, transport, now));
        const auto state = require_value(impl_->scene.entity_state(observer.entity.handle));
        const math::Vec3 focus{state.world_matrix[3]};
        std::vector<net::CombatTargetId> visible;
        visible.reserve(impl_->actors.size());
        const auto radius = impl_->interest_limits.enter_radius;
        for (const auto& actor : impl_->actors) {
            if (actor.entity == observer.entity || !actor.attached)
                continue;
            const auto target = require_value(impl_->scene.entity_state(actor.entity.handle));
            const math::Vec3 delta = math::Vec3{target.world_matrix[3]} - focus;
            if (glm::dot(delta, delta) <= radius * radius)
                visible.push_back(actor.binding.target);
        }
        return require_value(
            impl_->combat->view(authority, transport, impl_->scene, impl_->physics, visible, now));
    });
}
Result<RegionReconnect> RegionWorld::reconnect(const RegionGrant& grant,
                                               storage::CharacterAuthority& authority,
                                               net::TlsTransport& transport, MonotonicTime now,
                                               ScriptRuntime* scripts) {
    return boundary<RegionReconnect>("server.regions.reconnect", [&] {
        impl_->clock(now);
        auto& actor = impl_->lookup(grant);
        if (impl_->next_grant == std::numeric_limits<std::uint64_t>::max())
            fail("Region grant sequence exhausted");
        const auto session = impl_->authorize(actor, authority, transport, now, false);
        const auto peer = require_value(transport.authenticated_peer());
        auto gameplay = require_value(
            impl_->combat->reconnect(authority, transport, *impl_->movement, impl_->scene,
                                     impl_->physics, actor.entity.handle, now, scripts));
        if (gameplay.error) {
            impl_->faulted = true;
            return RegionReconnect{actor.grant, std::move(gameplay)};
        }
        if (!gameplay.state) {
            impl_->faulted = true;
            fail("Reconnected actor has no owned state");
        }
        actor.binding.stream = gameplay.state->stream;
        actor.binding.target = gameplay.state->self;
        if (gameplay.state->movement)
            actor.binding.movement = *gameplay.state->movement;
        actor.session = session;
        actor.connection = peer.connection_id;
        actor.attached = true;
        actor.grant = impl_->issue(actor.grant.entity, actor.grant.region);
        if (actor.writer && gameplay.state->movement)
            require_value(impl_->movement->set_actor_domain(actor.binding.movement.stream,
                                                            impl_->corridor(actor.writer->region)));
        return RegionReconnect{actor.grant, std::move(gameplay)};
    });
}
Result<net::InterestPublishResult>
RegionWorld::publish_interest(const RegionGrant& grant, storage::CharacterAuthority& authority,
                              net::TlsTransport& transport, MonotonicTime now) {
    return boundary<net::InterestPublishResult>("server.regions.interest.publish", [&] {
        impl_->clock(now);
        const auto& actor = impl_->lookup(grant);
        static_cast<void>(impl_->authorize(actor, authority, transport, now));
        const auto catalog = impl_->catalog(actor);
        return require_value(
            authority.publish_interest(*impl_->interest, impl_->scene, catalog, transport, now));
    });
}
Result<net::ReplicationControlResult>
RegionWorld::control_interest(const RegionGrant& grant, storage::CharacterAuthority& authority,
                              net::TlsTransport& transport, const net::ReplicationControl& control,
                              MonotonicTime now) {
    return boundary<net::ReplicationControlResult>("server.regions.interest.control", [&] {
        impl_->clock(now);
        const auto& actor = impl_->lookup(grant);
        static_cast<void>(impl_->authorize(actor, authority, transport, now));
        const auto catalog = impl_->catalog(actor);
        return require_value(authority.control_interest(*impl_->interest, impl_->scene, catalog,
                                                        transport, control, now));
    });
}
Result<storage::RuntimeTicket>
RegionWorld::submit_transfer(const RegionGrant& grant, storage::CharacterAuthority& authority,
                             net::TlsTransport& transport, const net::SessionEnvelope& envelope,
                             const storage::TransferRequest& request, MonotonicTime deadline,
                             MonotonicTime now) {
    return boundary<storage::RuntimeTicket>("server.regions.transfer.submit", [&] {
        impl_->clock(now);
        const auto& actor = impl_->lookup(grant);
        static_cast<void>(impl_->authorize(actor, authority, transport, now));
        auto free = std::find_if(impl_->pending.begin(), impl_->pending.end(),
                                 [](const auto& pending) { return !pending; });
        if (free == impl_->pending.end())
            fail("Region durable completion inventory is full");
        const auto ticket = require_value(
            authority.submit_transfer(transport, envelope, impl_->scene, actor.entity.handle,
                                      request, deadline, now, actor.writer));
        *free = Impl::PendingTransfer{ticket, actor.grant.entity, actor.character};
        return ticket;
    });
}
Result<storage::RuntimeCompletion>
RegionWorld::take_transfer(const RegionGrant& grant, storage::CharacterAuthority& authority,
                           net::TlsTransport& transport, storage::RuntimeTicket ticket,
                           MonotonicTime now) {
    return boundary<storage::RuntimeCompletion>("server.regions.transfer.take", [&] {
        impl_->clock(now);
        const auto& actor = impl_->lookup(grant);
        static_cast<void>(impl_->authorize(actor, authority, transport, now));
        auto retained =
            std::find_if(impl_->pending.begin(), impl_->pending.end(), [&](const auto& pending) {
                return pending && pending->ticket == ticket &&
                       pending->entity == actor.grant.entity &&
                       pending->character == actor.character;
            });
        if (retained == impl_->pending.end())
            fail("Region durable completion belongs to another actor or is no longer retained");
        auto completion =
            require_value(authority.take(ticket, transport, impl_->scene, now, actor.writer));
        if (completion.outcome)
            retained->reset();
        return completion;
    });
}
Result<RegionWorldStats> RegionWorld::stats() const {
    return boundary<RegionWorldStats>("server.regions.stats", [&] {
        if (impl_->owner != std::this_thread::get_id())
            fail("Region world requires its creating thread");
        const auto movement = require_value(impl_->movement->stats());
        return RegionWorldStats{movement.completed_ticks, impl_->regions.size(),
                                impl_->actors.size(),     impl_->transitions,
                                impl_->faulted,           impl_->npcs.size()};
    });
}
Result<storage::RegionWriter> RegionWorld::current_writer(const RegionGrant& grant) const {
    return boundary<storage::RegionWriter>("server.regions.writer", [&] {
        impl_->check(true);
        const auto& actor = impl_->lookup(grant);
        if (!actor.writer)
            fail("Actor has no persistent regional writer");
        return *actor.writer;
    });
}
Result<RegionHandoff> RegionWorld::handoff(storage::RegionTransferId id) const {
    return boundary<RegionHandoff>("server.regions.handoff", [&] {
        if (impl_->owner != std::this_thread::get_id())
            fail("Regional handoff requires its creating thread");
        return impl_->handoff(id);
    });
}
Result<void> RegionWorld::release_handoff(storage::RegionTransferId id) {
    return boundary<void>("server.regions.handoff.release", [&] {
        impl_->check(true);
        const auto& record = impl_->handoff(id);
        if (!terminal(record.phase))
            fail("Unresolved regional handoff cannot be released");
        const auto found = std::find_if(impl_->handoffs.begin(), impl_->handoffs.end(),
                                        [&](const auto& retained) { return retained.id == id; });
        impl_->handoffs.erase(found);
    });
}
Result<RegionHandoff> RegionWorld::prepare_handoff(storage::RegionTransferId id,
                                                   const RegionGrant& source, RegionId destination,
                                                   RegionInstanceId destination_instance,
                                                   storage::CharacterAuthority& authority,
                                                   MonotonicTime now) {
    return boundary<RegionHandoff>("server.regions.handoff.prepare", [&] {
        impl_->control_clock(authority, now);
        if (!impl_->persistent)
            fail("Regional handoff requires persistent ownership mode");
        for (const auto& record : impl_->handoffs)
            if (record.id == id) {
                if (record.source != source || record.destination != destination ||
                    record.destination_instance != destination_instance)
                    fail("Regional transfer identity was reused with changed inputs");
                return record;
            }
        require_value(impl_->fence->check(impl_->permit));
        if (impl_->handoffs.size() == 8 ||
            std::any_of(impl_->handoffs.begin(), impl_->handoffs.end(),
                        [](const auto& record) { return !terminal(record.phase); }))
            fail("Regional handoff inventory is occupied or full");
        const auto& actor = impl_->lookup(source);
        impl_->writer(actor, authority);
        if (!actor.attached || destination == source.region ||
            !adjacent(impl_->definition(source.region), impl_->definition(destination)))
            fail("Handoff requires an attached actor and an adjacent destination");
        impl_->handoffs.push_back(
            {id, source, destination, destination_instance, RegionHandoffPhase::prepared});
        return impl_->handoffs.back();
    });
}
Result<RegionHandoff> RegionWorld::cut_handoff(storage::RegionTransferId id,
                                               storage::CharacterAuthority& authority,
                                               std::span<net::TlsTransport* const> transports,
                                               MonotonicTime now) {
    return boundary<RegionHandoff>("server.regions.handoff.cut", [&] {
        impl_->control_clock(authority, now);
        auto& record = impl_->handoff(id);
        if (record.phase != RegionHandoffPhase::prepared)
            return record;
        require_value(impl_->fence->check(impl_->permit));
        auto& source = impl_->lookup(record.source);
        for (const auto& actor : impl_->actors)
            impl_->writer(actor, authority);
        if (impl_->next_grant == std::numeric_limits<std::uint64_t>::max() ||
            impl_->transitions == std::numeric_limits<std::uint64_t>::max() ||
            impl_->permit.generation == std::numeric_limits<std::uint64_t>::max())
            fail("Regional handoff publication counters are exhausted");
        const auto pose = require_value(impl_->scene.entity_state(source.entity.handle));
        if (!contains(impl_->definition(record.destination), math::Vec3{pose.world_matrix[3]}))
            fail("Handoff destination does not contain the final committed pose");
        if (transports.size() > movement_maximum_characters ||
            std::any_of(transports.begin(), transports.end(), [](const auto* p) { return !p; }))
            fail("Handoff requires a bounded exact live transport inventory");
        std::array<std::uint64_t, movement_maximum_characters> connections{};
        for (std::size_t i = 0; i < transports.size(); ++i) {
            const auto peer = transports[i]->authenticated_peer();
            if (!peer)
                continue;
            const auto end = connections.begin() + static_cast<std::ptrdiff_t>(i);
            if (std::find(connections.begin(), end, peer.value().connection_id) != end)
                fail("Handoff transport inventory contains a duplicate connection");
            connections[i] = peer.value().connection_id;
        }
        std::vector<net::SessionSnapshot> sessions;
        sessions.reserve(impl_->actors.size());
        for (const auto& actor : impl_->actors) {
            if (!actor.attached)
                continue;
            bool found = false;
            for (auto* transport : transports) {
                const auto peer = transport->authenticated_peer();
                if (!peer || peer.value().connection_id != actor.connection)
                    continue;
                sessions.push_back(impl_->authorize(actor, authority, *transport, now));
                found = true;
                break;
            }
            if (!found)
                fail("Handoff cannot omit an attached actor's actual session");
        }
        if (sessions.empty())
            fail("Handoff requires its live actual source session");
        require_value(authority.poll(now));
        for (const auto& actor : impl_->actors)
            impl_->writer(actor, authority);
        require_value(impl_->fence->freeze(impl_->permit));
        try {
            auto cut = std::make_shared<RegionCut>(
                RegionCut{impl_->frame,
                          require_value(impl_->traversal.cells()),
                          require_value(impl_->movement->capture_cut(impl_->scene, impl_->physics)),
                          require_value(impl_->combat->capture_cut(impl_->scene, impl_->physics)),
                          {},
                          std::move(sessions),
                          require_value(authority.runtime_pending()),
                          {},
                          {},
                          {}});
            if (cut->movement.completed_tick != cut->combat.completed_tick)
                fail("Handoff gameplay cuts do not share a completed tick");
            if (std::any_of(cut->runtime.begin(), cut->runtime.end(),
                            [](const auto& pending) { return pending.control; }))
                fail("Handoff cannot capture concurrent ownership-control work");
            cut->npcs.reserve(impl_->npcs.size());
            for (const auto& npc : impl_->npcs)
                cut->npcs.push_back({npc.grant, npc.entity, npc.body, impl_->npc_body(npc),
                                     require_value(impl_->physics.velocity(impl_->scene, npc.body)),
                                     npc.pending});
            cut->routes.reserve(impl_->pending.size());
            for (const auto& pending : impl_->pending)
                if (pending)
                    cut->routes.push_back({pending->ticket, pending->entity, pending->character});
            cut->writers.reserve(impl_->actors.size());
            cut->players.reserve(impl_->actors.size());
            for (const auto& actor : impl_->actors) {
                cut->players.push_back({actor.grant, actor.entity, actor.character, actor.binding,
                                        actor.session, actor.connection, actor.attached,
                                        actor.writer});
                if (actor.writer)
                    cut->writers.push_back(*actor.writer);
            }
            cut->scope = impl_->scope;
            cut->next_grant = impl_->next_grant;
            cut->transitions = impl_->transitions;
            cut->clock = impl_->last_time;
            cut->execution = impl_->permit;
            impl_->cut_readiness(*cut, source);
            cut->digest = fingerprint(*cut, record);
            const auto moving =
                std::find_if(cut->movement.actors.begin(), cut->movement.actors.end(),
                             [&](const auto& actor) { return actor.entity == source.entity; });
            const auto live =
                std::find_if(cut->sessions.begin(), cut->sessions.end(), [&](const auto& session) {
                    return session.character == source.character;
                });
            if (live == cut->sessions.end())
                fail("Handoff source session is no longer attached");
            const auto applied = moving == cut->movement.actors.end()
                                     ? source.binding.movement.last_input
                                     : moving->applied;
            const auto accepted = moving == cut->movement.actors.end() ? applied : moving->accepted;
            record.request = storage::RegionTransferRequest{record.id,
                                                            *source.writer,
                                                            record.destination,
                                                            record.destination_instance,
                                                            cut->movement.completed_tick,
                                                            accepted,
                                                            applied,
                                                            live->last_sequence,
                                                            cut->digest};
            record.cut = std::move(cut);
            record.cut_at = now;
            record.phase = RegionHandoffPhase::cut;
            return record;
        } catch (...) {
            if (!impl_->fence->resume(impl_->permit))
                impl_->faulted = true;
            throw;
        }
    });
}
Result<RegionHandoff> RegionWorld::begin_commit(storage::RegionTransferId id,
                                                storage::CharacterAuthority& authority,
                                                MonotonicTime deadline, MonotonicTime now) {
    return boundary<RegionHandoff>("server.regions.handoff.commit", [&] {
        impl_->control_clock(authority, now);
        auto& record = impl_->handoff(id);
        impl_->budget(record, now);
        if (record.phase == RegionHandoffPhase::aborted)
            fail("Aborted regional handoff cannot begin commit");
        if (record.ticket || record.phase == RegionHandoffPhase::committed ||
            record.phase == RegionHandoffPhase::rejected)
            return record;
        if (!record.cut || !record.request ||
            (record.phase != RegionHandoffPhase::cut &&
             record.phase != RegionHandoffPhase::reconciliation_required))
            fail("Regional commit requires its original frozen cut");
        impl_->cut_readiness(*record.cut, impl_->lookup(record.source));
        const auto moves =
            require_value(impl_->movement->capture_cut(impl_->scene, impl_->physics));
        const auto combat = require_value(impl_->combat->capture_cut(impl_->scene, impl_->physics));
        if (moves.bytes != record.cut->movement.bytes || combat.bytes != record.cut->combat.bytes)
            fail("Regional cut changed before commit; source must remain frozen");
        record.ticket =
            require_value(authority.submit_region_commit(*record.request, deadline, now));
        record.phase = record.budget_exceeded ? RegionHandoffPhase::reconciliation_required
                                              : RegionHandoffPhase::committing;
        return record;
    });
}
Result<void> RegionWorld::activate_handoff(RegionHandoff& record,
                                           const storage::RegionOwnership& ownership) {
    return boundary<void>("server.regions.handoff.activate", [&] {
        if (!record.request || !record.cut || !ownership.last_transfer ||
            *ownership.last_transfer != *record.request)
            fail("Regional activation requires the exact persisted original cut");
        const auto& request = *record.request;
        const auto& writer = ownership.writer;
        if (writer.store != request.source.store || writer.character != request.source.character ||
            writer.entity != request.source.entity || writer.region != request.destination ||
            writer.instance != request.destination_instance ||
            request.source.epoch == std::numeric_limits<std::uint64_t>::max() ||
            writer.epoch != request.source.epoch + 1)
            fail("Regional commit returned a different destination writer");
        auto& actor = impl_->lookup(record.source);
        impl_->cut_readiness(*record.cut, actor);
        const auto native = require_value(impl_->physics.stats());
        const auto scene = require_value(impl_->scene.stats());
        const auto& moves = record.cut->movement;
        if (native.completed_ticks != moves.completed_tick || native.faulted ||
            scene.instance != moves.revision.scene_instance ||
            scene.revision != moves.revision.revision || scene.pending_mutations != 0)
            fail("Regional cut changed after persistence; explicit reconciliation is required");
        // Snapshot staging preceded durable commit. Readiness checks above may
        // reject activation while retaining the committed cut frozen. These
        // operations rotate authority without rebinding or resetting gameplay.
        const auto permit = require_value(impl_->fence->rotate(impl_->permit));
        impl_->permit = permit;
        if (auto adopted = impl_->movement->adopt_execution(permit); !adopted) {
            impl_->faulted = true;
            throw Failure{std::move(adopted).error()};
        }
        if (auto adopted = impl_->combat->adopt_execution(permit); !adopted) {
            impl_->faulted = true;
            throw Failure{std::move(adopted).error()};
        }
        actor.writer = writer;
        actor.grant = impl_->issue(actor.grant.entity, writer.region);
        require_value(impl_->fence->resume(permit));
        const auto moving =
            std::find_if(moves.actors.begin(), moves.actors.end(),
                         [&](const auto& moved) { return moved.entity == actor.entity; });
        if (moving != moves.actors.end()) {
            auto changed =
                impl_->movement->set_actor_domain(moving->stream, impl_->corridor(writer.region));
            if (!changed) {
                static_cast<void>(impl_->fence->freeze(permit));
                impl_->faulted = true;
                throw Failure{std::move(changed).error()};
            }
        }
        ++impl_->transitions;
        record.grant = actor.grant;
        record.phase = RegionHandoffPhase::committed;
        record.error.reset();
        record.settled_at = impl_->last_time;
        record.cut.reset(); // Terminal receipts retain identity, not another full island snapshot.
    });
}
Result<RegionHandoff> RegionWorld::poll_handoff(storage::RegionTransferId id,
                                                storage::CharacterAuthority& authority,
                                                MonotonicTime now) {
    return boundary<RegionHandoff>("server.regions.handoff.poll", [&] {
        impl_->control_clock(authority, now);
        auto& record = impl_->handoff(id);
        impl_->budget(record, now);
        if (terminal(record.phase) || record.phase == RegionHandoffPhase::rejected)
            return record;
        if (!record.ticket)
            return record;
        auto polled = authority.poll(now);
        if (!polled) {
            record.error = std::move(polled).error();
            record.phase = RegionHandoffPhase::reconciliation_required;
            return record;
        }
        auto taken = authority.take_region(*record.ticket, now);
        if (!taken) {
            record.error = std::move(taken).error();
            record.phase = RegionHandoffPhase::reconciliation_required;
            return record;
        }
        auto completion = std::move(taken).value();
        if (!completion.outcome) {
            if (completion.delivery == storage::RuntimeDelivery::timed_out ||
                completion.delivery == storage::RuntimeDelivery::reconciliation_required)
                record.phase = RegionHandoffPhase::reconciliation_required;
            return record;
        }
        if (!*completion.outcome) {
            record.error = completion.outcome->error();
            const auto code = record.error->code;
            const bool known =
                completion.delivery != storage::RuntimeDelivery::reconciliation_required &&
                (code == ErrorCode::conflict || code == ErrorCode::invalid_argument ||
                 code == ErrorCode::invalid_configuration || code == ErrorCode::invalid_id);
            record.phase =
                known ? RegionHandoffPhase::rejected : RegionHandoffPhase::reconciliation_required;
            return record;
        }
        const auto* result =
            std::get_if<storage::RegionTransferResult>(&completion.outcome->value());
        if (!result || !record.request || result->request != *record.request) {
            record.phase = RegionHandoffPhase::reconciliation_required;
            record.error =
                Error{ErrorCode::conflict, "Regional worker completion changed its cut", {}};
            return record;
        }
        const auto ownership = require_value(authority.region_ownership(result->writer.character));
        if (!ownership)
            fail("Committed regional ownership was not published to the cache");
        auto activated = activate_handoff(record, *ownership);
        if (!activated) {
            record.error = std::move(activated).error();
            record.phase = RegionHandoffPhase::reconciliation_required;
        }
        return record;
    });
}
Result<RegionHandoff> RegionWorld::reconcile_handoff(storage::RegionTransferId id,
                                                     storage::CharacterAuthority& authority,
                                                     MonotonicTime now) {
    return boundary<RegionHandoff>("server.regions.handoff.reconcile", [&] {
        impl_->control_clock(authority, now);
        auto& record = impl_->handoff(id);
        if (terminal(record.phase))
            return record;
        impl_->budget(record, now);
        if (record.phase == RegionHandoffPhase::rejected)
            return record;
        if (!record.request)
            fail("Regional reconciliation requires a retained cut");
        const auto ownership =
            require_value(authority.region_ownership(record.request->source.character));
        if (!ownership || !ownership->last_transfer ||
            *ownership->last_transfer != *record.request) {
            record.phase = RegionHandoffPhase::reconciliation_required;
            return record;
        }
        auto activated = activate_handoff(record, *ownership);
        if (!activated) {
            record.error = std::move(activated).error();
            record.phase = RegionHandoffPhase::reconciliation_required;
        }
        return record;
    });
}
Result<RegionHandoff> RegionWorld::abort_handoff(storage::RegionTransferId id,
                                                 storage::CharacterAuthority& authority,
                                                 MonotonicTime now) {
    return boundary<RegionHandoff>("server.regions.handoff.abort", [&] {
        impl_->control_clock(authority, now);
        auto& record = impl_->handoff(id);
        impl_->budget(record, now);
        if (record.phase == RegionHandoffPhase::aborted)
            return record;
        if (record.phase == RegionHandoffPhase::committed ||
            (record.ticket && record.phase != RegionHandoffPhase::rejected))
            fail("Submitted or committed regional handoff cannot be timeout-aborted");
        if (record.cut) {
            const auto& actor = impl_->lookup(record.source);
            impl_->writer(actor, authority);
            require_value(impl_->fence->resume(impl_->permit));
        }
        record.phase = RegionHandoffPhase::aborted;
        record.settled_at = now;
        record.cut.reset();
        return record;
    });
}
} // namespace shark::server
