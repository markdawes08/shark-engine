#pragma once

#include "shark/physics.hpp"
#include "shark/storage/characters.hpp"

namespace shark::server {

// Closed trusted-host boundary. Gateway implementations use actual player TLS;
// executor implementations accept only authenticated parent-channel receipts.
// Copied SessionSnapshots and client-provided values are never credentials.
class GameplayAccess {
  public:
    virtual ~GameplayAccess() = default;
    [[nodiscard]] virtual Result<net::SessionSnapshot> authorize(std::uint64_t connection,
                                                                 const Scene& scene,
                                                                 EntityHandle entity,
                                                                 MonotonicTime now) = 0;
    [[nodiscard]] virtual Result<net::SessionSnapshot>
    admit(std::uint64_t connection, const net::SessionEnvelope& envelope, MonotonicTime now) = 0;
};

class LocalGameplayAccess final : public GameplayAccess {
  public:
    LocalGameplayAccess(storage::CharacterAuthority& authority,
                        std::span<net::TlsTransport* const> transports) noexcept;
    [[nodiscard]] static std::uint64_t connection_id(net::TlsTransport& transport);
    [[nodiscard]] Result<net::SessionSnapshot> authorize(std::uint64_t connection,
                                                         const Scene& scene, EntityHandle entity,
                                                         MonotonicTime now) override;
    [[nodiscard]] Result<net::SessionSnapshot> admit(std::uint64_t connection,
                                                     const net::SessionEnvelope& envelope,
                                                     MonotonicTime now) override;

  private:
    [[nodiscard]] net::TlsTransport* transport(std::uint64_t connection) const;
    storage::CharacterAuthority& authority_;
    std::span<net::TlsTransport* const> transports_;
};

// Exact source references are lookup keys, never destination authority. The
// closed installer validates fresh destination references/bodies independently.
struct GameplayTransferBinding {
    EntityReference source;
    EntityReference destination;
    std::optional<PhysicsBodyHandle> source_body;
    std::optional<PhysicsBodyHandle> destination_body;
};
[[nodiscard]] Result<void>
validate_gameplay_transfer_bindings(const Scene& scene, const PhysicsWorld& physics,
                                    std::span<const GameplayTransferBinding> bindings);

} // namespace shark::server
