#pragma once

#include "shark/net/transport.hpp"
#include "shark/stable_id.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>
#include <vector>

namespace shark::net {

using AccountId = TypedId<struct AccountIdDomain>;
using CharacterId = TypedId<struct CharacterIdDomain>;
using SessionId = TypedId<struct SessionIdDomain>;

struct LoginRequest {
    AccountId account;
    CharacterId character;
    bool operator==(const LoginRequest&) const = default;
};

struct SessionEnvelope {
    SessionId session;
    std::uint64_t ownership_epoch;
    std::uint64_t sequence;
    CharacterId character;
    bool operator==(const SessionEnvelope&) const = default;
};

struct SetReadyRequest {
    SessionEnvelope authority;
    bool ready;
    bool operator==(const SetReadyRequest&) const = default;
};
struct HeartbeatRequest {
    SessionEnvelope authority;
    bool operator==(const HeartbeatRequest&) const = default;
};
struct LogoutRequest {
    SessionEnvelope authority;
    bool operator==(const LogoutRequest&) const = default;
};
using SessionRequest = std::variant<LoginRequest, SetReadyRequest, HeartbeatRequest, LogoutRequest>;

// Owned state of the caller's session only. Ready is a session preference, not
// permission to author a scene or grant movement, combat, inventory or progression.
struct SessionSnapshot {
    SessionId session;
    AccountId account;
    CharacterId character;
    std::uint64_t ownership_epoch;
    std::uint64_t last_sequence;
    bool ready;
    bool active;
    bool operator==(const SessionSnapshot&) const = default;
};
struct SessionRejected {
    bool operator==(const SessionRejected&) const = default;
};
using SessionReply = std::variant<SessionSnapshot, SessionRejected>;

// Strip every internal admission failure to the same public reply, without IDs,
// diagnostics or the distinction between unknown, revoked and wrong ownership.
[[nodiscard]] SessionReply public_session_reply(const Result<SessionSnapshot>& result);

struct SessionLimits {
    std::size_t accounts{64};
    std::size_t characters{128};
    std::size_t certificates{128};
    std::size_t live_sessions{64};
    std::uint32_t login_attempts_per_window{128};
    std::uint32_t auth_attempts_per_window{8};
    std::uint32_t commands_per_window{64};
    MonotonicTime auth_window{std::chrono::seconds{10}};
    MonotonicTime command_window{std::chrono::seconds{1}};
    MonotonicTime session_lifetime{std::chrono::hours{1}};
    MonotonicTime session_idle_timeout{std::chrono::seconds{60}};
};
struct SessionStats {
    std::size_t accounts;
    std::size_t characters;
    std::size_t certificates;
    std::size_t live_sessions;
};

[[nodiscard]] Result<std::uint64_t> next_ownership_epoch(std::uint64_t previous);

// Single-owner, bounded, in-memory service. No durable identity store, credentials,
// player gameplay, authoring transactions or administration protocol is provided.
// Provisioning/revocation are trusted owner operations, never player requests.
// Registry records and revocation tombstones remain until service destruction;
// removal/re-enrollment cannot reset rate windows or ownership fences.
class SessionAuthority final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<SessionAuthority>>
    create(const SessionLimits& limits = {});
    ~SessionAuthority();
    SessionAuthority(const SessionAuthority&) = delete;
    SessionAuthority& operator=(const SessionAuthority&) = delete;
    SessionAuthority(SessionAuthority&&) = delete;
    SessionAuthority& operator=(SessionAuthority&&) = delete;

    [[nodiscard]] Result<void> provision_account(AccountId account);
    [[nodiscard]] Result<void> assign_character(AccountId account, CharacterId character);
    [[nodiscard]] Result<void> enroll_certificate(AccountId account,
                                                  const CertificateFingerprint& certificate);
    [[nodiscard]] Result<void> revoke_account(AccountId account);
    [[nodiscard]] Result<void> revoke_certificate(const CertificateFingerprint& certificate);
    // An old ID never revokes a replacement session. False means no live match.
    [[nodiscard]] Result<bool> revoke_session(SessionId session);

    // Calls authenticated_peer() on this actual server-side mTLS transport on
    // every request. A copied PeerIdentity is never an authorization input.
    // One active session per account. A successful login requires a connection
    // created after the account's previous successful login connection, rotates
    // its OS-entropy SessionId and advances its nonwrapping account-wide epoch.
    // The same epoch also fences each owned character across character switches.
    // Rejections spend applicable rate budgets but never advance a session's
    // sequence, refresh its lease or replace existing ownership.
    [[nodiscard]] Result<SessionSnapshot> apply(TlsTransport& transport,
                                                const SessionRequest& request, MonotonicTime now);
    // Trusted read for downstream services. Rechecks actual live mTLS provenance
    // and expires ended leases, without spending a command sequence/rate budget
    // or extending lifetime/idle time. Readiness is returned, never implied.
    [[nodiscard]] Result<SessionSnapshot> current(TlsTransport& transport, MonotonicTime now);
    [[nodiscard]] Result<std::size_t> expire(MonotonicTime now);
    [[nodiscard]] Result<SessionStats> stats() const;

  private:
    struct Impl;
    explicit SessionAuthority(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// Independent session protocol version, inside outer gameplay-wire version 1.
// Four-byte prefix: version u16 BE, request/reply kind u8, reserved zero u8.
// Requests: Login=1 (36 bytes), SetReady=2 (53), Heartbeat=3 (52), Logout=4 (52).
// Replies: accepted=1 (70 bytes), generic rejection=2 (4). UUID bytes are in
// canonical display order; all integers are BE and bool bytes are exactly 0/1.
// No trailing data, unknown versions/kinds, nil/non-v4 UUIDs or native layouts.
inline constexpr std::uint16_t session_protocol_version = 1;
inline constexpr std::size_t session_max_request_bytes = 53;
inline constexpr std::size_t session_max_reply_bytes = 70;
[[nodiscard]] Result<std::vector<std::byte>> encode_session_request(const SessionRequest& request);
[[nodiscard]] Result<SessionRequest> decode_session_request(std::span<const std::byte> bytes);
[[nodiscard]] Result<std::vector<std::byte>> encode_session_reply(const SessionReply& reply);
[[nodiscard]] Result<SessionReply> decode_session_reply(std::span<const std::byte> bytes);

} // namespace shark::net
