#pragma once

#include "shark/net/session.hpp"
#include "shark/scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace shark::net {

// A stream-local nonzero counter. Never a Scene handle or durable entity ID.
struct ReplicaId {
    std::uint64_t value{0};
    auto operator<=>(const ReplicaId&) const = default;
};

// Only public field 1 in replication version 1: row-major upper 3x4 of the
// validated scene-local affine world matrix. Implicit final row [0,0,0,1].
// Translation is in coefficients 3,7,11. No Name, local TRS, hierarchy, durable
// IDs or private scene revision. Global origin/region transfer remains M05.
struct ReplicatedPose {
    std::array<double, 12> coefficients{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    bool operator==(const ReplicatedPose&) const = default;
};
[[nodiscard]] Result<ReplicatedPose> canonicalize_replicated_pose(ReplicatedPose pose);

enum class ReplicationChange : std::uint8_t { spawn = 1, update = 2, despawn = 3 };
struct ReplicationRecord {
    ReplicationChange kind;
    ReplicaId id;
    // Required for spawn/update; absent for despawn.
    std::optional<ReplicatedPose> pose;
    bool operator==(const ReplicationRecord&) const = default;
};
struct ReplicationUpdate {
    SessionId session;
    std::uint64_t ownership_epoch;
    ReplicaSessionId stream;
    std::uint64_t revision;
    std::uint64_t base_revision;
    bool baseline;
    // Strictly increasing replica IDs; baseline contains only spawns.
    std::vector<ReplicationRecord> changes;
    bool operator==(const ReplicationUpdate&) const = default;
};
struct ReplicationAck {
    SessionEnvelope authority;
    ReplicaSessionId stream;
    std::uint64_t revision;
    bool operator==(const ReplicationAck&) const = default;
};
struct ReplicationResync {
    SessionEnvelope authority;
    ReplicaSessionId stream;
    std::uint64_t known_revision;
    bool operator==(const ReplicationResync&) const = default;
};
using ReplicationControl = std::variant<ReplicationAck, ReplicationResync>;
enum class ReplicationControlResult { acknowledged, duplicate, resync_requested };

inline constexpr std::uint16_t replication_protocol_version = 1;
inline constexpr std::size_t replication_max_entities = 32;
inline constexpr std::size_t replication_max_changes = 64;
inline constexpr std::size_t replication_update_header_bytes = 62;
inline constexpr std::size_t replication_control_bytes = 76;
// Prefix: version u16 BE, kind u8, reserved-zero u8. Update kind1 baseline/2
// delta, then session UUID, ownership epoch u64, stream UUID, revision u64,
// base revision u64, count u16. Each record: kind u8 + replica u64; spawn/update
// add mask u8 (exactly1) and12 IEEE754 binary64 BE coefficients. Positive zero
// only, finite validated affine basis. Baseline32 records=3454 bytes; worst
// delta32 spawns/updates+32 despawns=3742 bytes, within the outer4096 limit.
// Control kind1 ACK/2 resync: SessionEnvelope, stream UUID, revision u64.
// No native object layout or implicit authoring-schema serialization.
[[nodiscard]] Result<void> validate_replication_update(const ReplicationUpdate& update);
[[nodiscard]] Result<std::vector<std::byte>>
encode_replication_update(const ReplicationUpdate& update);
[[nodiscard]] Result<ReplicationUpdate> decode_replication_update(std::span<const std::byte> bytes);
[[nodiscard]] Result<std::vector<std::byte>>
encode_replication_control(const ReplicationControl& control);
[[nodiscard]] Result<ReplicationControl>
decode_replication_control(std::span<const std::byte> bytes);

struct ReplicationLimits {
    std::size_t entities_per_stream{32};                            // 1..32
    std::size_t streams{16};                                        // 1..64
    MonotonicTime acknowledgement_timeout{std::chrono::seconds{5}}; // positive..60s
    std::uint32_t controls_per_window{64};                          // 1..65536
    MonotonicTime control_window{std::chrono::seconds{1}};          // positive..1h
    std::uint32_t resyncs_per_window{4};                            // 1..64
    MonotonicTime resync_window{std::chrono::seconds{10}};          // positive..1h
};
struct ReplicationStats {
    std::size_t streams;
    std::size_t mapped_entities;
    std::size_t pending_updates;
};

enum class ReplicationPublishStatus { sent, unchanged, pending_ack, byte_limited, queue_limited };
struct ReplicationPublication {
    ReplicationPublishStatus status;
    std::size_t framed_bytes{0};
    std::size_t entities{0};
    std::size_t records{0};
};

// Trusted single-owner publisher. Selection is an explicit server allowlist,
// never supplied by a player or discovered from authoring metadata. Source must
// be one live Region Scene on its owner thread. Committed state only. One
// acknowledged state and at most one immutable pending update per stream.
class ReplicationServer final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<ReplicationServer>>
    create(const ReplicationLimits& limits = {});
    ~ReplicationServer();
    ReplicationServer(const ReplicationServer&) = delete;
    ReplicationServer& operator=(const ReplicationServer&) = delete;
    ReplicationServer(ReplicationServer&&) = delete;
    ReplicationServer& operator=(ReplicationServer&&) = delete;

    // Revalidates actual transport/current ready session on every call without
    // refreshing its lease. Returns retained pending bytes' logical value until
    // ACK, or nullopt when acknowledged public state is unchanged. Pending ACK
    // expiry rejects until explicit resync or teardown; no unbounded history.
    [[nodiscard]] Result<std::optional<ReplicationUpdate>>
    prepare(Scene& scene, std::span<const EntityHandle> published, SessionAuthority& sessions,
            TlsTransport& transport, MonotonicTime now);
    // Managed admission: capture/encode a complete candidate, then commit its
    // revision, IDs and ACK deadline only after successful TLS FIFO admission.
    // Never resends pending state. Requires an empty outgoing queue, including
    // after resync; an earlier admitted frame cannot be replaced or overtaken.
    // Byte allowance includes the outer wire header, not TLS/TCP overhead.
    // Do not mix prepare and publish within one session stream.
    [[nodiscard]] Result<ReplicationPublication>
    publish(Scene& scene, std::span<const EntityHandle> published, SessionAuthority& sessions,
            TlsTransport& transport, MonotonicTime now, std::size_t available_framed_bytes);
    // Fully validate control before consuming shared session sequence/rate and
    // lease via Heartbeat. Invalid controls spend their separate bounded window
    // but do not acknowledge state, consume session sequence or refresh leases.
    [[nodiscard]] Result<ReplicationControlResult> control(SessionAuthority& sessions,
                                                           TlsTransport& transport,
                                                           const ReplicationControl& control,
                                                           MonotonicTime now);
    // Caller releases ended/disconnected sessions. Old IDs cannot release a
    // replacement. Reconnect on the same account reclaims its old stream.
    [[nodiscard]] Result<bool> forget(SessionId session);
    [[nodiscard]] Result<ReplicationStats> stats() const;

  private:
    struct Impl;
    explicit ReplicationServer(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

struct ReplicaEntity {
    ReplicaId id;
    ReplicatedPose pose;
    bool operator==(const ReplicaEntity&) const = default;
};
struct ReplicaSnapshot {
    std::optional<ReplicaSessionId> stream;
    std::uint64_t revision;
    bool has_baseline;
    std::vector<ReplicaEntity> entities;
};
enum class ReplicaApplyResult { applied, duplicate, resync_required };

// Single-owner presentation cache, not a second authoritative Scene. Caller
// supplies only messages received from its verified TLS server. A fresh login
// constructs a fresh cache; copied values confer no server-side authority.
class ReplicaClient final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<ReplicaClient>>
    create(const SessionSnapshot& session, std::size_t max_entities = 32);
    ~ReplicaClient();
    ReplicaClient(const ReplicaClient&) = delete;
    ReplicaClient& operator=(const ReplicaClient&) = delete;
    ReplicaClient(ReplicaClient&&) = delete;
    ReplicaClient& operator=(ReplicaClient&&) = delete;
    // Validate complete candidate then swap. Exact repeat of latest accepted
    // update is idempotent; conflicting/stale revisions reject. A missing base
    // yields resync_required without partial publication. Baselines can skip
    // revisions but cannot change the stream within an authenticated session.
    [[nodiscard]] Result<ReplicaApplyResult> apply(const ReplicationUpdate& update);
    [[nodiscard]] Result<ReplicaSnapshot> snapshot() const;
    // Cache loss retains stream/revision fences; only a newer full baseline can
    // restore it. Never reset fences to accept previously rejected old traffic.
    [[nodiscard]] Result<void> discard_baseline();

  private:
    struct Impl;
    explicit ReplicaClient(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::net
