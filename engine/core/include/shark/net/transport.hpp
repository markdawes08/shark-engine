#pragma once

#include <shark/error.hpp>
#include <shark/net/wire.hpp>
#include <shark/time.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace shark::net {

// No implicit system trust, credentials, hostname lookup or insecure TLS switch.
struct ClientTrust {
    std::string ca_pem{};
    std::string server_name{};
};

struct ServerIdentity {
    std::string certificate_pem{};
    std::string private_key_pem{};
};

struct ClientIdentity {
    std::string certificate_pem{};
    std::string private_key_pem{};
};

using CertificateFingerprint = std::array<std::byte, 32>;

// Enrollment identifier only. Possessing a fingerprint proves no authority.
[[nodiscard]] Result<CertificateFingerprint>
certificate_fingerprint(const std::string& certificate_pem);

struct PeerIdentity {
    // Process-local, nonzero, never reused. Not an account/session identity.
    std::uint64_t connection_id;
    CertificateFingerprint certificate;
    bool operator==(const PeerIdentity&) const = default;
};

struct TransportLimits {
    // Independent send and receive queues, including a partially written frame.
    std::size_t queue_frames{32};
    std::size_t queue_bytes{64 * 1024};
    MonotonicTime handshake_timeout{std::chrono::seconds{5}};
    MonotonicTime idle_timeout{std::chrono::seconds{30}};
    MonotonicTime write_timeout{std::chrono::seconds{5}};
};

enum class TransportState { connecting, handshaking, negotiating, ready, closed, failed };

struct TransportStats {
    TransportState state{TransportState::connecting};
    // Actual socket payload bytes, including TLS overhead; excludes TCP/IP headers.
    std::uint64_t socket_bytes_sent{0};
    std::uint64_t socket_bytes_received{0};
    std::uint64_t frames_sent{0};
    std::uint64_t frames_received{0};
    std::size_t send_queue_frames{0};
    std::size_t send_queue_bytes{0};
    std::size_t receive_queue_frames{0};
    std::size_t receive_queue_bytes{0};
    std::size_t operations{0};
};

class TlsListener;

// Single-owner, nonblocking, IPv4 loopback-only foundation. poll performs at most
// 1..64 TLS/I/O/decoding steps. The caller supplies monotonic time and scheduling;
// deadlines are enforced when polled, without hidden threads or blocking waits.
// TLS 1.3 verifies the server certificate and exact supplied DNS name. It
// authenticates a player credential only in the explicit mutual-TLS mode;
// account enrollment and authorization belong to SessionAuthority.
class TlsTransport final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<TlsTransport>>
    connect_loopback(std::uint16_t port, const ClientTrust& trust, MonotonicTime now,
                     const TransportLimits& limits = {});
    [[nodiscard]] static Result<std::unique_ptr<TlsTransport>>
    connect_loopback_authenticated(std::uint16_t port, const ClientTrust& trust,
                                   const ClientIdentity& identity, MonotonicTime now,
                                   const TransportLimits& limits = {});
    ~TlsTransport();
    TlsTransport(const TlsTransport&) = delete;
    TlsTransport& operator=(const TlsTransport&) = delete;
    TlsTransport(TlsTransport&&) = delete;
    TlsTransport& operator=(TlsTransport&&) = delete;

    [[nodiscard]] Result<TransportStats> poll(MonotonicTime now, std::size_t budget = 16);
    // Reliable FIFO admission, not a peer/application acknowledgement. hello is
    // automatic. Diagnostic mode accepts probe/reply; mutual-TLS session mode
    // accepts session_request/reply and replication_control/state in their
    // respective client/server directions. Admission and TLS grant no ownership.
    [[nodiscard]] Result<void> send(MessageKind kind, std::span<const std::byte> payload);
    // Validated frames remain drainable after authenticated peer close-notify.
    // An explicit local close or a failed stream discards pending input.
    [[nodiscard]] Result<std::optional<Frame>> receive();
    [[nodiscard]] Result<TransportStats> stats() const;
    // Only a ready server-side mutual-TLS connection can return this identity.
    // Rechecks certificate validity; closed, failed and diagnostic peers reject.
    [[nodiscard]] Result<PeerIdentity> authenticated_peer() const;
    // Bounded local abort: drops pending output and releases the socket. No
    // delivery promise or blocking TLS close-notify exchange. Idempotent.
    [[nodiscard]] Result<void> close();

  private:
    [[nodiscard]] static Result<std::unique_ptr<TlsTransport>>
    connect_impl(std::uint16_t port, const ClientTrust& trust, const ClientIdentity* identity,
                 MonotonicTime now, const TransportLimits& limits);
    struct Impl;
    explicit TlsTransport(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
    friend class TlsListener;
};

class TlsListener final {
  public:
    // Port zero requests an ephemeral port. A maximum of 1..16 accepted live
    // transports share this listener's certificate/configuration lifetime.
    [[nodiscard]] static Result<std::unique_ptr<TlsListener>>
    listen_loopback(std::uint16_t port, const ServerIdentity& identity,
                    const TransportLimits& limits = {}, std::size_t max_connections = 16);
    [[nodiscard]] static Result<std::unique_ptr<TlsListener>> listen_loopback_authenticated(
        std::uint16_t port, const ServerIdentity& identity, const std::string& client_ca_pem,
        const TransportLimits& limits = {}, std::size_t max_connections = 16);
    ~TlsListener();
    TlsListener(const TlsListener&) = delete;
    TlsListener& operator=(const TlsListener&) = delete;
    TlsListener(TlsListener&&) = delete;
    TlsListener& operator=(TlsListener&&) = delete;

    [[nodiscard]] Result<std::uint16_t> port() const;
    [[nodiscard]] Result<std::optional<std::unique_ptr<TlsTransport>>> accept(MonotonicTime now);
    [[nodiscard]] Result<void> close();

  private:
    [[nodiscard]] static Result<std::unique_ptr<TlsListener>>
    listen_impl(std::uint16_t port, const ServerIdentity& identity,
                const std::string* client_ca_pem, const TransportLimits& limits,
                std::size_t max_connections);
    struct Impl;
    explicit TlsListener(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::net
