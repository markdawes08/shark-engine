#pragma once

#include "shark/server/region_journal.hpp"
#include "shark/server/region_process.hpp"

namespace shark::server {
inline constexpr std::size_t region_gateway_maximum_routes = 8;
enum class RegionGatewayPhase {
    active,
    prepared,
    committing,
    committed,
    recovery_required,
    closed
};
enum class RegionRouteStatus { buffered, admitted, rejected, uncertain };
struct RegionRouteReceipt {
    std::uint64_t ticket;
    RegionRouteStatus status;
    std::optional<Error> error{};
};
struct RegionGatewayStats {
    RegionGatewayPhase phase;
    std::uint64_t tick;
    std::uint64_t route_watermark;
    std::size_t buffered;
    std::size_t uncertain;
    std::uint64_t source_pid;
    std::uint64_t destination_pid;
    MonotonicTime quiescence{};
    bool budget_exceeded{false};
};
enum class RegionProcessRole { source, destination };

// Sole local gateway: actual player TLS/session and durable worker owners stay
// here. A complete interaction island executes in exactly one owned Server child.
// Calls are serialized creating-thread host work, not a public admin protocol.
class RegionGateway final {
  public:
    // Initial image is tick zero from a released bootstrap owner. All writers
    // must already match the authority cache and all peers are reauthorized using
    // the actual transports. Opening this owner never restores copied sessions.
    [[nodiscard]] static Result<std::unique_ptr<RegionGateway>>
    create(storage::CharacterAuthority& authority, const std::filesystem::path& server,
           const std::filesystem::path& journal, const RegionProcessImage& initial,
           std::span<net::TlsTransport* const> transports, MonotonicTime now);
    ~RegionGateway();
    RegionGateway(const RegionGateway&) = delete;
    RegionGateway& operator=(const RegionGateway&) = delete;
    [[nodiscard]] Result<RegionRouteReceipt>
    submit(net::TlsTransport& transport, const net::MovementInput& input, MonotonicTime now);
    [[nodiscard]] Result<RegionRouteReceipt>
    submit(net::TlsTransport& transport, const net::CombatAction& input, MonotonicTime now);
    // Buffered post-cut intents have not spent a player command sequence.
    // Drain revalidates and admits their immutable original values in FIFO order.
    // Live transports are borrowed for this call only; none are retained.
    [[nodiscard]] Result<std::vector<RegionRouteReceipt>>
    drain_routes(std::span<net::TlsTransport* const> transports, MonotonicTime now);
    [[nodiscard]] Result<RegionProcessImage> step(std::span<net::TlsTransport* const> transports,
                                                  MonotonicTime now);
    [[nodiscard]] Result<void> prepare_handoff(RegionInstanceId destination,
                                               std::span<net::TlsTransport* const> transports,
                                               MonotonicTime now);
    [[nodiscard]] Result<storage::RuntimeTicket> begin_commit(MonotonicTime deadline,
                                                              MonotonicTime now);
    [[nodiscard]] Result<RegionGatewayPhase> poll_commit(MonotonicTime now);
    [[nodiscard]] Result<void> activate(MonotonicTime now);
    // Hard fences both owned children and confirms exit first, then stages a new
    // incarnation using the last durable cut and actual cached committed writers.
    // An unresolved original SQL ticket must settle before this can proceed.
    [[nodiscard]] Result<void> prepare_recovery(RegionInstanceId replacement,
                                                std::span<net::TlsTransport* const> transports,
                                                MonotonicTime now);
    [[nodiscard]] Result<void> terminate_executor(RegionProcessRole role);
    [[nodiscard]] Result<void> close(MonotonicTime now);
    [[nodiscard]] Result<RegionGatewayStats> stats() const;
    [[nodiscard]] Result<RegionProcessImage> snapshot() const;
    [[nodiscard]] Result<net::InterestPublishResult> publish_interest(net::TlsTransport& transport,
                                                                      MonotonicTime now);
    [[nodiscard]] Result<net::ReplicationControlResult>
    control_interest(net::TlsTransport& transport, const net::ReplicationControl& control,
                     MonotonicTime now);
    [[nodiscard]] Result<storage::RuntimeTicket>
    submit_transfer(net::TlsTransport& transport, const net::SessionEnvelope& envelope,
                    const storage::TransferRequest& request, MonotonicTime deadline,
                    MonotonicTime now);
    [[nodiscard]] Result<storage::RuntimeCompletion>
    take_transfer(net::TlsTransport& transport, storage::RuntimeTicket ticket, MonotonicTime now);

  private:
    struct Impl;
    explicit RegionGateway(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};
} // namespace shark::server
