#pragma once

#include "shark/scene.hpp"
#include "shark/storage/store.hpp"

namespace shark::storage {

using RuntimeId = TypedId<struct RuntimeIdDomain>;
inline constexpr std::size_t maximum_runtime_pending = 16;
struct RuntimeLimits {
    std::size_t pending{maximum_runtime_pending};
};
struct RuntimeTicket {
    RuntimeId owner;
    std::uint64_t sequence;
    bool operator==(const RuntimeTicket&) const = default;
};
enum class RuntimeDelivery { pending, ready, timed_out, fenced, reconciliation_required };
using RuntimeValue = std::variant<CharacterRecord, TransferResult>;
struct RuntimeCompletion {
    RuntimeTicket ticket;
    RuntimeDelivery delivery;
    // The original economic identity/payload, including on an uncertain result.
    // These are trusted owner values, never a player response or authorization.
    std::optional<TransferRequest> transfer;
    // Every settled result is returned, including fenced or timed-out delivery.
    // Delivery status still forbids applying those values to the old session/Scene.
    std::optional<Result<RuntimeValue>> outcome;
};
struct RuntimeStats {
    std::size_t capacity;
    std::size_t retained;
    std::size_t unsettled;
    std::size_t completed;
    std::size_t timed_out;
    bool reconciliation_required;
};

// Server-only creating-thread owner. Hydrates one SessionAuthority from complete
// durable records before admission; never exposes a mutable store/session bypass.
// Synchronous I/O is for the current idle service/fixtures, not a live tick loop.
// No sessions, connection tokens or process-local epochs are restored from disk.
class CharacterAuthority final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<CharacterAuthority>>
    create(std::unique_ptr<Store> store, const net::SessionLimits& limits = {});
    // Opens and destroys Store on one dedicated worker; SessionAuthority and
    // scene admission stay on this creating thread. Never moves an existing
    // Store across threads. Startup is explicit blocking maintenance work.
    [[nodiscard]] static Result<std::unique_ptr<CharacterAuthority>>
    open_runtime(const std::filesystem::path& path, StoreId expected_store, WorldId expected_world,
                 const net::SessionLimits& limits = {}, RuntimeLimits runtime_limits = {});
    ~CharacterAuthority();
    CharacterAuthority(const CharacterAuthority&) = delete;
    CharacterAuthority& operator=(const CharacterAuthority&) = delete;
    CharacterAuthority(CharacterAuthority&&) = delete;
    CharacterAuthority& operator=(CharacterAuthority&&) = delete;

    [[nodiscard]] Result<StoreMetadata> metadata() const;
    // Trusted owner administration; never a player or authoring endpoint.
    [[nodiscard]] Result<void> provision_account(net::AccountId account);
    [[nodiscard]] Result<void> enroll_certificate(net::AccountId account,
                                                  const net::CertificateFingerprint& certificate);
    [[nodiscard]] Result<void> revoke_account(net::AccountId account);
    [[nodiscard]] Result<void> revoke_certificate(const net::CertificateFingerprint& certificate);
    [[nodiscard]] Result<bool> revoke_session(net::SessionId session);
    // Trusted one-time fixture provisioning, never a player grant or state write.
    [[nodiscard]] Result<EconomyRecord> initialize_economy(net::AccountId account,
                                                           net::CharacterId character,
                                                           const EconomyState& initial);

    // Account admission uses the actual live mTLS transport and shares existing
    // bounded authentication budgets. It does not issue or restore a session.
    [[nodiscard]] Result<CharacterRoster> roster(net::TlsTransport& transport,
                                                 net::AccountId account, MonotonicTime now);
    [[nodiscard]] Result<CharacterRecord> create_character(net::TlsTransport& transport,
                                                           net::AccountId account,
                                                           std::uint64_t expected_roster_revision,
                                                           const CharacterState& initial,
                                                           MonotonicTime now);
    [[nodiscard]] Result<net::SessionSnapshot>
    apply(net::TlsTransport& transport, const net::SessionRequest& request, MonotonicTime now);
    // Both operations require a current ready session and spend the exact next
    // command sequence/rate admission. Rejected revision/state never changes the
    // database; admitted stale-revision/storage failures consume their sequence.
    [[nodiscard]] Result<CharacterRecord>
    load(net::TlsTransport& transport, const net::SessionEnvelope& authority, MonotonicTime now);
    // accepted_state is trusted server output, NOT a player-supplied checkpoint.
    [[nodiscard]] Result<CharacterRecord>
    save(net::TlsTransport& transport, const net::SessionEnvelope& authority,
         std::uint64_t expected_revision, const CharacterState& accepted_state, MonotonicTime now);

    // Ready, sequenced session admission applies to queries and retries too.
    // Results are trusted server values, not public player response payloads.
    [[nodiscard]] Result<EconomyRecord>
    economy(net::TlsTransport& transport, const net::SessionEnvelope& authority, MonotonicTime now);
    [[nodiscard]] Result<OperationStream> operation_stream(net::TlsTransport& transport,
                                                           const net::SessionEnvelope& authority,
                                                           MonotonicTime now);
    [[nodiscard]] Result<TransferResult> transfer(net::TlsTransport& transport,
                                                  const net::SessionEnvelope& authority,
                                                  const TransferRequest& request,
                                                  MonotonicTime now);

    // Runtime submission copies bounded server-owned values, admits exactly one
    // live command, and returns without database I/O. One retained operation per
    // account; all queued, executing and untaken completions count toward capacity.
    // Deadlines are strictly after now and at most 60 seconds later. A timeout
    // never cancels or rolls back a dispatched write and never frees its slot.
    [[nodiscard]] Result<RuntimeTicket>
    submit_save(net::TlsTransport& transport, const net::SessionEnvelope& authority, Scene& scene,
                EntityHandle entity, std::uint64_t expected_revision,
                const CharacterState& accepted_state, MonotonicTime deadline, MonotonicTime now);
    [[nodiscard]] Result<RuntimeTicket> submit_transfer(net::TlsTransport& transport,
                                                        const net::SessionEnvelope& authority,
                                                        Scene& scene, EntityHandle entity,
                                                        const TransferRequest& request,
                                                        MonotonicTime deadline, MonotonicTime now);
    [[nodiscard]] Result<RuntimeStats> poll(MonotonicTime now);
    [[nodiscard]] Result<RuntimeStats> runtime_stats() const;
    // Rechecks live transport/session and the exact Scene EntityReference. Never
    // mutates Scene. A settled take consumes the slot, including fenced/expired
    // delivery; an unsettled timeout remains retained. Keep original save inputs
    // and use the returned transfer request for explicit durable reconciliation.
    [[nodiscard]] Result<RuntimeCompletion> take(RuntimeTicket ticket, net::TlsTransport& transport,
                                                 Scene& scene, MonotonicTime now);
    // Explicit blocking maintenance, only after every retained result is settled
    // and taken. Joins the worker, reopens the exact StoreId/world/schema, hydrates
    // authorization, and rotates runtime identity. All prior sessions/tickets
    // become invalid. No operation is automatically replayed; caller reconciles
    // transfers by their original ID and saves by durable revision/state.
    [[nodiscard]] Result<void> recover();

  private:
    struct Impl;
    explicit CharacterAuthority(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::storage
