#include "shark/server/gameplay_access.hpp"

namespace shark::server {
LocalGameplayAccess::LocalGameplayAccess(storage::CharacterAuthority& authority,
                                         std::span<net::TlsTransport* const> transports) noexcept
    : authority_(authority), transports_(transports) {}
std::uint64_t LocalGameplayAccess::connection_id(net::TlsTransport& transport) {
    const auto peer = transport.authenticated_peer();
    return peer ? peer.value().connection_id : 0;
}
net::TlsTransport* LocalGameplayAccess::transport(std::uint64_t connection) const {
    if (connection != 0)
        for (auto* candidate : transports_)
            if (candidate && connection_id(*candidate) == connection)
                return candidate;
    return nullptr;
}
Result<net::SessionSnapshot> LocalGameplayAccess::authorize(std::uint64_t connection,
                                                            const Scene& scene, EntityHandle entity,
                                                            MonotonicTime now) {
    if (auto* peer = transport(connection))
        return authority_.authorize_entity(*peer, scene, entity, now);
    return Error{
        ErrorCode::conflict, "Gameplay connection is unavailable", {"server.gameplay.authorize"}};
}
Result<net::SessionSnapshot> LocalGameplayAccess::admit(std::uint64_t connection,
                                                        const net::SessionEnvelope& envelope,
                                                        MonotonicTime now) {
    if (auto* peer = transport(connection))
        return authority_.apply(*peer, net::HeartbeatRequest{envelope}, now);
    return Error{
        ErrorCode::conflict, "Gameplay connection is unavailable", {"server.gameplay.admit"}};
}
Result<void>
validate_gameplay_transfer_bindings(const Scene& scene, const PhysicsWorld& physics,
                                    std::span<const GameplayTransferBinding> bindings) {
    const auto invalid = [] {
        return Error{ErrorCode::conflict,
                     "Invalid or duplicate gameplay relocation map",
                     {"server.gameplay.transfer.map"}};
    };
    if (bindings.size() > physics_maximum_bodies)
        return invalid();
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        const auto& map = bindings[i];
        const auto* source = std::get_if<RegionSceneScope>(&map.source.scope);
        const auto* destination = std::get_if<RegionSceneScope>(&map.destination.scope);
        if (!source || !destination || source->world != destination->world ||
            !std::holds_alternative<WorldEntityId>(map.source.identity) ||
            map.source.identity != map.destination.identity ||
            map.source_body.has_value() != map.destination_body.has_value())
            return invalid();
        if (auto resolved = scene.resolve(map.destination); !resolved)
            return resolved.error();
        if (map.destination_body) {
            const auto body = physics.body_state(scene, *map.destination_body);
            if (!body)
                return body.error();
            if (body.value().entity != map.destination)
                return invalid();
        }
        for (std::size_t j = 0; j < i; ++j)
            if (bindings[j].source.identity == map.source.identity ||
                bindings[j].destination == map.destination ||
                (map.source_body && bindings[j].source_body == map.source_body) ||
                (map.destination_body && bindings[j].destination_body == map.destination_body))
                return invalid();
    }
    return {};
}
} // namespace shark::server
