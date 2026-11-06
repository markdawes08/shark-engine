#include "shark/server/region_image.hpp"

#include "execution_cut_encoding.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace shark::server {
namespace {
struct InvalidImage {
    Error error;
};
[[noreturn]] void invalid(const char* message) {
    throw InvalidImage{
        {ErrorCode::invalid_argument, message, {.operation = "server.region-image"}}};
}
template <class T> T checked(Result<T> value) {
    if (!value)
        throw InvalidImage{std::move(value).error()};
    return std::move(value).value();
}
void checked(Result<void> value) {
    if (!value)
        throw InvalidImage{std::move(value).error()};
}
using Writer = detail::CutEncoder;
void cap(const Writer& out, std::size_t extra = 0) {
    if (out.bytes.size() > region_process_image_maximum_bytes ||
        extra > region_process_image_maximum_bytes - out.bytes.size())
        invalid("Region image exceeds sixteen MiB");
}
void blob(Writer& out, std::span<const std::byte> value) {
    cap(out, 8);
    out.value(static_cast<std::uint64_t>(value.size()));
    cap(out, value.size());
    out.bytes.insert(out.bytes.end(), value.begin(), value.end());
}
void text(Writer& out, const std::string& value) {
    blob(out, std::as_bytes(std::span{value.data(), value.size()}));
}
template <class T, class F> void optional(Writer& out, const std::optional<T>& value, F encode) {
    out.value(value.has_value());
    if (value)
        encode(*value);
}
template <class T, class F> void list(Writer& out, const std::vector<T>& values, F encode) {
    out.value(static_cast<std::uint64_t>(values.size()));
    for (const auto& value : values) {
        encode(value);
        cap(out);
    }
}
void write(Writer& out, const Camera& x) {
    out.value(x.projection);
    out.value(x.vertical_fov_radians);
    out.value(x.orthographic_height);
    out.value(x.near_clip);
    out.value(x.far_clip);
}
void write(Writer& out, const MeshRenderer& x) {
    out.value(x.asset);
    out.value(x.mesh_index);
    out.value(x.visible);
}
void write(Writer& out, const PhysicsWorldConfig& x) {
    out.value(x.tick_hz);
    out.value(x.collision_substeps);
    out.value(x.max_bodies);
    out.value(x.max_body_pairs);
    out.value(x.max_contact_constraints);
    out.value(static_cast<std::uint64_t>(x.temporary_bytes));
    out.value(x.gravity);
    out.value(x.max_contact_pairs);
    out.value(x.max_queued_events);
}
void write(Writer& out, const PhysicsStaticMesh& x) {
    out.value(x.vertices);
    list(out, x.triangles, [&](const auto& triangle) {
        for (const auto index : triangle)
            out.value(index);
    });
}
void write(Writer& out, const PhysicsContactParticipant& x) {
    out.value(x.body);
    out.value(x.entity);
    out.value(x.is_trigger);
}
void write(Writer& out, const PhysicsTransferState& x) {
    out.value(x.world);
    out.value(x.revision);
    out.value(SceneScope{x.scope});
    write(out, x.config);
    out.value(x.completed_tick);
    out.value(x.next_contact_sequence);
    out.value(x.discarded_events);
    list(out, x.bodies, [&](const auto& body) {
        out.value(body.source);
        out.value(body.native);
        out.value(body.settings);
        out.value(body.source_pose);
        out.value(body.previous_pose);
        out.value(body.velocity);
        out.value(body.pending_force);
        out.value(body.target);
        optional(out, body.mesh, [&](const auto& mesh) { write(out, mesh); });
    });
    list(out, x.active_pairs, [&](const auto& pair) {
        write(out, pair.first);
        write(out, pair.second);
    });
}
void write(Writer& out, const CombatRulesPortableState& x) {
    text(out, x.source.path);
    text(out, x.source.text);
    out.value(static_cast<std::uint64_t>(x.config.memory_bytes));
    out.value(static_cast<std::uint64_t>(x.config.instruction_limit));
    for (const auto slot : x.state)
        out.value(slot);
    out.value(x.evaluations);
}
void write(Writer& out, const storage::RegionWriter& x) {
    out.value(x.store);
    out.value(x.character);
    out.value(x.entity);
    out.value(x.region);
    out.value(x.instance);
    out.value(x.epoch);
}

struct Reader {
    std::span<const std::byte> bytes;
    std::size_t offset{0};
    std::size_t geometry_vertices{0};
    std::size_t geometry_triangles{0};
    std::size_t remaining() const {
        return bytes.size() - offset;
    }
    std::uint64_t integer(unsigned width) {
        if (width > remaining())
            invalid("Region image is truncated");
        std::uint64_t value = 0;
        for (unsigned index = 0; index < width; ++index)
            value = (value << 8) | std::to_integer<unsigned char>(bytes[offset++]);
        return value;
    }
    std::uint64_t u64() {
        return integer(8);
    }
    std::uint32_t u32() {
        return static_cast<std::uint32_t>(integer(4));
    }
    std::uint16_t u16() {
        return static_cast<std::uint16_t>(integer(2));
    }
    std::int64_t i64() {
        return std::bit_cast<std::int64_t>(u64());
    }
    std::int16_t i16() {
        return std::bit_cast<std::int16_t>(u16());
    }
    bool boolean() {
        const auto value = integer(1);
        if (value > 1)
            invalid("Region image boolean is not zero or one");
        return value != 0;
    }
    double real() {
        const auto value = std::bit_cast<double>(u64());
        if (!std::isfinite(value))
            invalid("Region image contains a nonfinite number");
        return value;
    }
    template <class E> E enumeration(std::uint32_t low, std::uint32_t high) {
        const auto value = u32();
        if (value < low || value > high)
            invalid("Region image enum is unknown");
        return static_cast<E>(value);
    }
    template <class T> T id() {
        if (remaining() < 16)
            invalid("Region image UUID is truncated");
        constexpr char hex[] = "0123456789abcdef";
        std::string value;
        value.reserve(36);
        for (unsigned index = 0; index < 16; ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10)
                value.push_back('-');
            const auto byte = std::to_integer<unsigned char>(bytes[offset++]);
            value.push_back(hex[byte >> 4]);
            value.push_back(hex[byte & 15]);
        }
        return T{checked(StableId::parse(value))};
    }
    std::size_t count(std::size_t maximum) {
        const auto value = u64();
        if (value > maximum || value > remaining())
            invalid("Region image count exceeds its bounds");
        return static_cast<std::size_t>(value);
    }
    std::vector<std::byte> blob(std::size_t maximum) {
        const auto size = count(maximum);
        const auto value = bytes.subspan(offset, size);
        offset += size;
        return {value.begin(), value.end()};
    }
    std::string text(std::size_t maximum) {
        const auto size = count(maximum);
        const auto value = bytes.subspan(offset, size);
        offset += size;
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    }
    template <class F> auto optional(F decode) -> std::optional<decltype(decode())> {
        if (!boolean())
            return std::nullopt;
        return decode();
    }
    template <class F> auto list(std::size_t maximum, F decode) -> std::vector<decltype(decode())> {
        const auto size = count(maximum);
        std::vector<decltype(decode())> values;
        values.reserve(size);
        for (std::size_t index = 0; index < size; ++index)
            values.push_back(decode());
        return values;
    }
    math::Vec3 vector() {
        return {real(), real(), real()};
    }
    Transform transform() {
        const auto translation = vector();
        const auto x = real(), y = real(), z = real(), w = real();
        return {translation, math::Quat{w, x, y, z}, vector()};
    }
    WorldPosition position() {
        return {id<WorldId>(), {i64(), i64(), i64()}, {real(), real(), real()}};
    }
    SceneRevision revision() {
        return {u64(), u64()};
    }
    EntityHandle entity_handle() {
        return {u64(), u32(), u64()};
    }
    PhysicsBodyHandle body_handle() {
        return {u64(), u32(), u64()};
    }
    AuthorityStamp authority() {
        return {id<RegionId>(), id<RegionInstanceId>(), u64()};
    }
    SceneScope scope() {
        switch (integer(1)) {
        case 0:
            return AuthoredSceneScope{id<SceneId>()};
        case 1:
            return RegionSceneScope{id<WorldId>(), authority()};
        case 2:
            return ReplicaSceneScope{id<WorldId>(), id<ReplicaSessionId>()};
        default:
            invalid("Region image Scene scope is unknown");
        }
    }
    EntityReference entity() {
        const auto handle = entity_handle();
        const auto kind = integer(1);
        if (kind > 1)
            invalid("Region image entity identity is unknown");
        const EntityIdentity identity = kind == 0 ? EntityIdentity{id<AuthoredEntityId>()}
                                                  : EntityIdentity{id<WorldEntityId>()};
        return {handle, identity, scope(), optional([&] { return authority(); })};
    }
    RigidBody rigid_body() {
        return {enumeration<BodyMotion>(0, 2),
                enumeration<ColliderShape>(0, 2),
                vector(),
                real(),
                real(),
                real(),
                real(),
                real(),
                u32(),
                u32(),
                boolean()};
    }
    PhysicsBodyVelocity velocity() {
        return {vector(), vector()};
    }
    PhysicsBodyState body_state() {
        return {entity(),
                optional([&] { return rigid_body(); }),
                transform(),
                vector(),
                enumeration<BodyMotion>(0, 2),
                real(),
                real(),
                real(),
                enumeration<PhysicsBodyGeometry>(0, 1),
                u32(),
                u32(),
                static_cast<std::size_t>(u64()),
                boolean()};
    }
    PhysicsContactParticipant participant() {
        return {body_handle(), entity(), boolean()};
    }
    PhysicsWorldConfig config() {
        return {u32(),    u32(), u32(), u32(), u32(), static_cast<std::size_t>(u64()),
                vector(), u32(), u32()};
    }
    PhysicsStaticMesh mesh() {
        auto vertices = list(physics_static_mesh_maximum_vertices, [&] { return vector(); });
        geometry_vertices += vertices.size();
        if (geometry_vertices > physics_transfer_maximum_geometry_vertices)
            invalid("Region image geometry vertex total exceeds its bound");
        auto triangles = list(physics_static_mesh_maximum_triangles,
                              [&] { return std::array<std::uint32_t, 3>{u32(), u32(), u32()}; });
        geometry_triangles += triangles.size();
        if (geometry_triangles > physics_static_mesh_maximum_world_triangles)
            invalid("Region image geometry triangle total exceeds its bound");
        return {std::move(vertices), std::move(triangles)};
    }
    PhysicsTransferState physics() {
        const auto world = u64();
        const auto rev = revision();
        const auto value_scope = scope();
        const auto* region = std::get_if<RegionSceneScope>(&value_scope);
        if (!region)
            invalid("Region native image requires a Region Scene");
        const auto settings = config();
        const auto tick = u64(), next = u64(), discarded = u64();
        auto bodies = list(physics_maximum_bodies, [&] {
            return PhysicsTransferBody{body_handle(),
                                       body_state(),
                                       rigid_body(),
                                       transform(),
                                       transform(),
                                       velocity(),
                                       vector(),
                                       optional([&] { return transform(); }),
                                       optional([&] { return mesh(); })};
        });
        auto pairs = list(16384, [&] { return PhysicsTransferPair{participant(), participant()}; });
        return {world, rev,       *region,           settings,        tick,
                next,  discarded, std::move(bodies), std::move(pairs)};
    }
    net::SessionSnapshot session() {
        return {id<net::SessionId>(),
                id<net::AccountId>(),
                id<net::CharacterId>(),
                u64(),
                u64(),
                boolean(),
                boolean()};
    }
    net::SessionEnvelope envelope() {
        return {id<net::SessionId>(), u64(), u64(), id<net::CharacterId>()};
    }
    net::MovementInput input() {
        return {envelope(), id<net::MovementStreamId>(), u64(), i16(), i16()};
    }
    net::MovementState movement_state() {
        return {id<net::SessionId>(),
                u64(),
                id<net::MovementStreamId>(),
                u64(),
                u64(),
                vector(),
                vector()};
    }
    MovementDomain domain() {
        return {real(), real(), real(), real()};
    }
    ExecutionPermit permit() {
        return {u64(), u64()};
    }
    MonotonicTime time() {
        const auto value = i64();
        if (value < 0)
            invalid("Region image clock fence is negative");
        return MonotonicTime{value};
    }
    MovementCut movement() {
        if (u32() != 1)
            invalid("Region image movement section is unknown");
        return {revision(),
                scope(),
                u64(),
                position(),
                optional([&] { return u64(); }),
                list(8, [&] { return domain(); }),
                u64(),
                time(),
                optional([&] { return permit(); }),
                list(movement_maximum_characters,
                     [&] {
                         return MovementActorCut{
                             u64(),
                             session(),
                             id<net::MovementStreamId>(),
                             entity(),
                             body_handle(),
                             body_state(),
                             velocity(),
                             u64(),
                             u64(),
                             optional([&] { return domain(); }),
                             list(movement_pending_inputs, [&] { return input(); })};
                     }),
                {}};
    }
    net::CombatAction action() {
        return {envelope(), id<net::CombatStreamId>(), u64(), id<net::CombatTargetId>(),
                enumeration<CombatActionKind>(1, 3)};
    }
    net::CombatActionResult outcome() {
        return {u64(), id<net::CombatTargetId>(), enumeration<CombatActionKind>(1, 3),
                enumeration<CombatOutcome>(1, 8), u16()};
    }
    CombatRuleDecision decision() {
        return {boolean(), u16(), u32()};
    }
    ProjectileCut projectile() {
        return {action(),      decision(),    id<net::CombatTargetId>(),
                body_handle(), body_handle(), entity(),
                vector(),      vector(),      u32(),
                u64()};
    }
    CombatCut combat() {
        if (u32() != 2)
            invalid("Region image combat section is unknown");
        return {revision(),
                scope(),
                u64(),
                u64(),
                time(),
                u64(),
                boolean(),
                optional([&] { return u64(); }),
                {u64(), boolean()},
                optional([&] { return permit(); }),
                list(movement_maximum_characters,
                     [&] {
                         return CombatActorCut{
                             u64(),
                             session(),
                             id<net::CombatStreamId>(),
                             id<net::CombatTargetId>(),
                             entity(),
                             optional([&] { return body_handle(); }),
                             optional([&] { return body_state(); }),
                             optional([&] { return velocity(); }),
                             rigid_body(),
                             transform(),
                             movement_state(),
                             u16(),
                             u64(),
                             u64(),
                             u64(),
                             u64(),
                             optional([&] { return action(); }),
                             list(net::combat_maximum_outcomes, [&] { return outcome(); }),
                             boolean(),
                             boolean(),
                             transform(),
                             optional([&] { return projectile(); })};
                     }),
                {}};
    }
    Camera camera() {
        return {enumeration<CameraProjection>(0, 1), real(), real(), real(), real()};
    }
    MeshRenderer renderer() {
        return {id<AssetId>(), u32(), boolean()};
    }
    CombatRulesPortableState rules() {
        CombatRulesPortableState result{
            {text(script_maximum_source_path_bytes), text(script_runtime_maximum_source_bytes)},
            {static_cast<std::size_t>(u64()), static_cast<std::size_t>(u64())},
            {},
            0};
        for (auto& slot : result.state)
            slot = i64();
        result.evaluations = u64();
        return result;
    }
    storage::RegionWriter writer() {
        return {id<storage::StoreId>(), id<net::CharacterId>(), id<WorldEntityId>(),
                id<RegionId>(),         id<RegionInstanceId>(), u64()};
    }
};

void validate_shape(const RegionProcessImage& image) {
    if (image.regions.size() < 2 || image.regions.size() > region_world_maximum_regions ||
        image.entities.size() > physics_maximum_bodies ||
        image.physics.bodies.size() > physics_maximum_bodies ||
        image.physics.active_pairs.size() > 16384 || image.movement.domains.size() > 8 ||
        image.movement.actors.size() > movement_maximum_characters ||
        image.combat.actors.size() > movement_maximum_characters ||
        image.cells.size() > world_cell_maximum_pins ||
        image.assets.size() > prepared_assets_maximum_count ||
        image.npcs.size() > region_world_maximum_npcs ||
        image.peers.size() > movement_maximum_characters ||
        image.writers.size() > movement_maximum_characters)
        invalid("Region image structural count exceeds its bound");
    checked(validate_world_position(image.frame.origin));
    if (image.frame.generation == 0 || image.movement.origin != image.frame.origin ||
        image.physics.completed_tick != image.movement.completed_tick ||
        image.physics.completed_tick != image.combat.completed_tick ||
        image.physics.world != image.movement.physics_world ||
        image.physics.world != image.combat.physics_world ||
        image.rules.evaluations != image.combat.rules.evaluations || image.combat.rules.faulted ||
        image.combat.script_instance)
        invalid("Region image frame, tick, native owner or portable rules are inconsistent");
    if (image.rules.source.path.empty() ||
        image.rules.source.path.size() > script_maximum_source_path_bytes ||
        image.rules.source.text.size() > script_runtime_maximum_source_bytes ||
        image.rules.config.memory_bytes > combat_rules_maximum_memory_bytes ||
        image.rules.config.instruction_limit > combat_rules_maximum_instructions)
        invalid("Region image portable source/configuration exceeds its bounds");
    std::size_t vertices = 0, triangles = 0;
    for (const auto& body : image.physics.bodies)
        if (body.mesh) {
            if (body.mesh->vertices.size() > physics_static_mesh_maximum_vertices ||
                body.mesh->triangles.size() > physics_static_mesh_maximum_triangles)
                invalid("Region image mesh count exceeds its bound");
            vertices += body.mesh->vertices.size();
            triangles += body.mesh->triangles.size();
        }
    if (vertices > physics_transfer_maximum_geometry_vertices ||
        triangles > physics_static_mesh_maximum_world_triangles)
        invalid("Region image geometry totals exceed their bounds");
    for (const auto& actor : image.movement.actors) {
        if (actor.pending.size() > movement_pending_inputs)
            invalid("Region image movement FIFO exceeds its bound");
        for (const auto& input : actor.pending)
            checked(net::validate_movement_input(input));
    }
    for (const auto& actor : image.combat.actors) {
        if (actor.outcomes.size() > net::combat_maximum_outcomes)
            invalid("Region image combat history exceeds its bound");
        if (actor.pending)
            checked(net::validate_combat_action(*actor.pending));
        if (actor.projectile)
            checked(net::validate_combat_action(actor.projectile->request));
    }
    for (std::size_t index = 0; index < image.npcs.size(); ++index) {
        const auto& npc = image.npcs[index];
        if (npc.pending && (!std::isfinite(npc.pending->x) || !std::isfinite(npc.pending->y) ||
                            !std::isfinite(npc.pending->z) || npc.pending->y != 0 ||
                            glm::length(*npc.pending) > movement_speed))
            invalid("Region image NPC intent exceeds its horizontal movement profile");
        for (std::size_t previous = 0; previous < index; ++previous)
            if (image.npcs[previous].source.identity == npc.source.identity ||
                image.npcs[previous].source.handle == npc.source.handle ||
                image.npcs[previous].body == npc.body)
                invalid("Region image contains a duplicate NPC source or body");
    }
    for (const auto& entity : image.entities) {
        checked(validate_name(entity.name));
        checked(canonicalize_transform(entity.transform));
        if (entity.camera)
            checked(validate_camera(*entity.camera));
        if (entity.mesh_renderer)
            checked(validate_mesh_renderer(*entity.mesh_renderer));
        if (entity.rigid_body)
            checked(validate_rigid_body(*entity.rigid_body));
    }
    for (const auto& cell : image.cells)
        if (cell.payload.size() > world_cell_maximum_payload_bytes)
            invalid("Region image cell payload exceeds its bound");
    for (const auto& asset : image.assets)
        if (asset.payload.size() > prepared_mesh_maximum_payload_bytes)
            invalid("Region image asset payload exceeds its bound");
}
} // namespace

Result<std::vector<std::byte>> encode_region_process_image(const RegionProcessImage& image) {
    try {
        validate_shape(image);
        Writer out;
        out.value(std::uint32_t{0x53485249}); // SHRI
        out.value(region_process_image_version);
        out.value(image.frame.origin);
        out.value(image.frame.generation);
        list(out, image.regions, [&](const auto& region) {
            out.value(region.id);
            out.value(region.min_x);
            out.value(region.max_x);
            out.value(region.min_z);
            out.value(region.max_z);
            out.value(region.interest_overlap);
        });
        list(out, image.entities, [&](const auto& entity) {
            out.value(entity.source);
            text(out, entity.name.value);
            out.value(entity.transform);
            out.value(entity.parent);
            optional(out, entity.camera, [&](const auto& value) { write(out, value); });
            optional(out, entity.mesh_renderer, [&](const auto& value) { write(out, value); });
            out.value(entity.rigid_body);
        });
        write(out, image.physics);
        out.value(image.movement);
        out.value(image.combat);
        write(out, image.rules);
        list(out, image.cells, [&](const auto& cell) {
            out.value(cell.key.world);
            out.value(cell.key.cell.x);
            out.value(cell.key.cell.y);
            out.value(cell.key.cell.z);
            out.value(cell.content_revision);
            out.value(cell.proxy);
            blob(out, cell.payload);
        });
        list(out, image.assets, [&](const auto& asset) {
            out.value(asset.asset);
            blob(out, asset.payload);
        });
        list(out, image.npcs, [&](const auto& npc) {
            out.value(npc.source);
            out.value(npc.body);
            out.value(npc.pending);
        });
        list(out, image.peers, [&](const auto& peer) {
            out.value(peer.connection);
            out.value(peer.session);
            out.value(peer.entity);
        });
        list(out, image.writers, [&](const auto& writer) { write(out, writer); });
        out.value(image.route_watermark);
        out.value(image.executor_instance);
        cap(out);
        return std::move(out.bytes);
    } catch (const InvalidImage& failure) {
        return failure.error;
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot encode bounded region image",
                     {.operation = "server.region-image.encode"}};
    }
}

Result<RegionProcessImage> decode_region_process_image(std::span<const std::byte> bytes) {
    if (bytes.size() > region_process_image_maximum_bytes)
        return Error{ErrorCode::invalid_argument,
                     "Region image exceeds sixteen MiB",
                     {.operation = "server.region-image.decode"}};
    try {
        Reader in{bytes};
        if (in.u32() != 0x53485249 || in.u32() != region_process_image_version)
            invalid("Region image magic/version is unsupported");
        RegionProcessImage image{
            {in.position(), in.u64()},
            in.list(region_world_maximum_regions,
                    [&] {
                        return RegionDefinition{in.id<RegionId>(), in.real(), in.real(),
                                                in.real(),         in.real(), in.real()};
                    }),
            in.list(physics_maximum_bodies,
                    [&] {
                        return RegionImageEntity{
                            in.entity(),
                            Name{in.text(maximum_name_bytes)},
                            in.transform(),
                            in.optional([&] { return in.id<WorldEntityId>(); }),
                            in.optional([&] { return in.camera(); }),
                            in.optional([&] { return in.renderer(); }),
                            in.optional([&] { return in.rigid_body(); })};
                    }),
            in.physics(),
            in.movement(),
            in.combat(),
            in.rules(),
            in.list(world_cell_maximum_pins,
                    [&] {
                        return RegionImageCell{{in.id<WorldId>(), {in.i64(), in.i64(), in.i64()}},
                                               in.u64(),
                                               in.id<WorldEntityId>(),
                                               in.blob(world_cell_maximum_payload_bytes)};
                    }),
            in.list(prepared_assets_maximum_count,
                    [&] {
                        return RegionImageAsset{in.id<AssetId>(),
                                                in.blob(prepared_mesh_maximum_payload_bytes)};
                    }),
            in.list(region_world_maximum_npcs,
                    [&] {
                        return RegionImageNpc{in.entity(), in.body_handle(),
                                              in.optional([&] { return in.vector(); })};
                    }),
            in.list(movement_maximum_characters,
                    [&] { return RegionPeer{in.u64(), in.session(), in.id<WorldEntityId>()}; }),
            in.list(movement_maximum_characters, [&] { return in.writer(); }),
            in.u64(),
            in.id<RegionInstanceId>()};
        if (in.remaining() != 0)
            invalid("Region image contains trailing bytes");
        validate_shape(image);
        Writer movement, combat;
        movement.value(image.movement);
        combat.value(image.combat);
        image.movement.bytes = std::move(movement.bytes);
        image.combat.bytes = std::move(combat.bytes);
        return image;
    } catch (const InvalidImage& failure) {
        return failure.error;
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot decode bounded region image",
                     {.operation = "server.region-image.decode"}};
    }
}

} // namespace shark::server
