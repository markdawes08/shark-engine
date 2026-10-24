#pragma once

#include "shark/storage/store.hpp"

namespace shark::storage {

// Server-only creating-thread owner. Hydrates one SessionAuthority from complete
// durable records before admission; never exposes a mutable store/session bypass.
// Synchronous I/O is for the current idle service/fixtures, not a live tick loop.
// No sessions, connection tokens or process-local epochs are restored from disk.
class CharacterAuthority final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<CharacterAuthority>>
    create(std::unique_ptr<Store> store, const net::SessionLimits& limits = {});
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

  private:
    struct Impl;
    explicit CharacterAuthority(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::storage
