#pragma once

#include "shark/net/movement.hpp"

#include <memory>
#include <utility>

namespace shark::net {

inline constexpr std::size_t movement_prediction_maximum_inputs = movement_maximum_pending_inputs;
struct MovementPredictionConfig {
    // Explicit host model, not a claim of native physics equivalence. The
    // current server profile fixes 20 Hz/6 m/s; its gravity is trusted config.
    double gravity_y{-9.81};
    std::size_t maximum_pending{movement_prediction_maximum_inputs};
};
struct PredictedMovementPose {
    math::Vec3 position;
    math::Vec3 velocity;
    bool operator==(const PredictedMovementPose&) const = default;
};
struct MovementPredictionSnapshot {
    MovementState authoritative;
    PredictedMovementPose predicted;
    std::size_t pending_inputs;
    std::uint64_t revision;
    bool reconciliation_required;
};

class MovementPredictor;
// Immutable exact input/bytes for an attempted FIFO admission. Preparing again
// does not reserve an input identity. Keep this value for a known-not-admitted
// retry; ambiguous host outcomes require explicit reconciliation.
class PreparedMovementInput final {
  public:
    [[nodiscard]] const MovementInput& input() const& noexcept {
        return input_;
    }
    const MovementInput& input() const&& = delete;
    [[nodiscard]] std::span<const std::byte> bytes() const& noexcept {
        return bytes_;
    }
    std::span<const std::byte> bytes() const&& = delete;

  private:
    friend class MovementPredictor;
    PreparedMovementInput(StableId owner, std::uint64_t revision, MovementInput input,
                          PredictedMovementPose predicted, std::vector<std::byte> bytes)
        : owner_(owner), revision_(revision), input_(input), predicted_(predicted),
          bytes_(std::move(bytes)) {}
    StableId owner_;
    std::uint64_t revision_;
    MovementInput input_;
    PredictedMovementPose predicted_;
    std::vector<std::byte> bytes_;
};
enum class PredictionSendResult { admitted, duplicate };
enum class PredictionCorrectionResult { applied, duplicate, reconciliation_required };

// Creating-thread, CPU-only presentation owner. Feed only corrections received
// from this connection's verified server. It owns no Scene, native physics,
// gameplay callback or durable service. Fresh login/binding uses a fresh owner.
class MovementPredictor final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<MovementPredictor>>
    create(const SessionSnapshot& session, const MovementState& initial,
           MovementPredictionConfig config = {});
    ~MovementPredictor();
    MovementPredictor(const MovementPredictor&) = delete;
    MovementPredictor& operator=(const MovementPredictor&) = delete;

    // The host's one shared session sequencer supplies the envelope. Session
    // commands may interleave; movement input IDs remain consecutive.
    [[nodiscard]] Result<PreparedMovementInput> prepare(SessionEnvelope authority, std::int16_t x,
                                                        std::int16_t z) const;
    // Preflight all work, admit exact bytes, then publish fixed-size history
    // without another failure window. Retained exact repeats never resend.
    [[nodiscard]] Result<PredictionSendResult> send(TlsTransport& transport,
                                                    const PreparedMovementInput& prepared);
    [[nodiscard]] Result<PredictionCorrectionResult> apply(const MovementReply& correction);
    // Host reports an ambiguous send/session outcome. Preserve exact history;
    // stop prediction until explicit fresh binding/session reconciliation.
    [[nodiscard]] Result<void> suspend();
    [[nodiscard]] Result<MovementPredictionSnapshot> snapshot() const;

  private:
    struct Impl;
    explicit MovementPredictor(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::net
