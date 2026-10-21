#pragma once

#include "shark/net/replication.hpp"

namespace shark::net {

// Trusted server configuration, never player input or an authored component.
enum class InterestVisibility : std::uint8_t { hidden = 0, public_pose = 1, owner_pose = 2 };
struct InterestEntry {
    EntityHandle entity;
    InterestVisibility visibility{InterestVisibility::hidden};
    std::optional<CharacterId> owner{};
    std::uint8_t priority{0};
    // Required rows still need authorization and spatial eligibility. A crowded
    // required set exceeding the visible limit rejects rather than dropping one.
    bool required{false};
    // Exactly one live observer row must match the current session's character.
    // Other rows may share that owner. A hidden observer supplies only the focus.
    bool observer{false};
};

struct InterestLimits {
    std::size_t candidate_limit{128}; // 1..128
    double enter_radius{100.0};       // finite, positive, <=1000000 scene-local metres
    double exit_radius{110.0};        // finite, enter_radius..1000000
    MonotonicTime publish_period{std::chrono::milliseconds{100}}; // 1ms..1s
    std::size_t framed_bytes_per_second{48 * 1024};               // 1..1048576
    std::size_t framed_burst_bytes{8 * 1024};                     // 3762..1048576
    std::size_t work_units{8192};                                 // 1..65536 per selection
    // Owns the visible/connection limits and ACK/control/resync limits.
    ReplicationLimits replication{};
};

struct InterestWork {
    std::size_t catalog_rows{0};
    std::size_t catalog_comparisons{0};
    std::size_t scene_reads{0};
    std::size_t distance_tests{0};
    std::size_t ranking_comparisons{0};
    // The complete policy is checked before any throttle. Bounded by128 rows,
    // 128 checked references and two bounded merge sorts plus adjacent checks.
    std::size_t policy_rows{0};
    std::size_t policy_scene_reads{0};
    std::size_t policy_comparisons{0};
    // Separate, unthrottled fail-closed checks of at most 32 last-sent handles.
    // Each may scan the bounded catalog and perform one checked Scene read.
    std::size_t privacy_checks{0};
    [[nodiscard]] std::size_t selection_units() const noexcept {
        return catalog_rows + catalog_comparisons + scene_reads + distance_tests +
               ranking_comparisons;
    }
};

enum class InterestPublishStatus {
    sent,
    unchanged,
    pending_ack,
    cadence_limited,
    byte_limited,
    queue_limited,
    work_limited,
    privacy_closed
};
struct InterestPublishResult {
    InterestPublishStatus status;
    std::size_t selected_entities{0};
    std::size_t framed_bytes{0};
    std::size_t records{0};
    InterestWork work{};
    std::size_t eligible_entities{0};
    std::size_t omitted_entities{0};
};
struct InterestStats {
    std::size_t streams{0};
    std::size_t selected_entities{0};
    std::size_t last_sent_entities{0};
    std::uint64_t publications{0};
    std::uint64_t framed_bytes{0};
    std::uint64_t records{0};
    std::uint64_t selection_units{0};
    std::uint64_t privacy_checks{0};
    std::uint64_t coalesced{0};
    std::uint64_t privacy_closures{0};
    std::uint64_t policy_units{0};
    std::uint64_t omitted_entities{0};
    // Owned vector capacity * sizeof(element), excluding allocator overhead,
    // temporary selection/capture storage and RSS. Fixed stream slots remain
    // reserved after forget; selected/last-sent allocations follow live streams.
    std::size_t stream_capacity_bytes{0};
    std::size_t selected_capacity_bytes{0};
    std::size_t last_sent_capacity_bytes{0};
    ReplicationStats replication{};
};

// Single-owner CPU service. Catalog, Scene and character-to-observer binding
// belong to the trusted server. The only replicated field remains public pose.
// Call publish/control with the current complete catalog before polling output
// after policy changes. A newly forbidden live last-sent entity aborts that actual
// transport, revokes its exact session and forgets its stream, including after ACK.
// Bytes already delivered cannot be recalled. Destroyed handles are normal exits.
// On a failed Result, close/release or repair and revalidate before polling output;
// owner/time failures may precede policy validation.
// No callback, Scene mutation, global origin, new wire schema or socket owner.
class InterestServer final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<InterestServer>>
    create(const InterestLimits& limits = {});
    ~InterestServer();
    InterestServer(const InterestServer&) = delete;
    InterestServer& operator=(const InterestServer&) = delete;
    InterestServer(InterestServer&&) = delete;
    InterestServer& operator=(InterestServer&&) = delete;

    // Selection uses committed world translations in one live Region Scene.
    // Required first, then descending priority, retained membership, distance,
    // and ascending local handle. Entry/exit radii are inclusive. Pose updates
    // coalesce in the Scene until atomic reliable FIFO admission; pending frames
    // are never resent. Budgets debit exact outer-framed bytes only when sent.
    [[nodiscard]] Result<InterestPublishResult> publish(Scene& scene,
                                                        std::span<const InterestEntry> catalog,
                                                        SessionAuthority& sessions,
                                                        TlsTransport& transport, MonotonicTime now);
    // Same unthrottled privacy validation before the existing authenticated ACK
    // or resync path. Rejection never consumes a session command sequence.
    [[nodiscard]] Result<ReplicationControlResult>
    control(Scene& scene, std::span<const InterestEntry> catalog, SessionAuthority& sessions,
            TlsTransport& transport, const ReplicationControl& control, MonotonicTime now);
    // Ended-session cleanup; never discard live privacy history and keep polling
    // that connection. Exact identity prevents stale cleanup removing a successor.
    [[nodiscard]] Result<bool> forget(SessionId session);
    [[nodiscard]] Result<InterestStats> stats() const;

  private:
    struct Impl;
    explicit InterestServer(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::net
