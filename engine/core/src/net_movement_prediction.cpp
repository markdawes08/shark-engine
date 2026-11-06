#include "shark/net/movement_prediction.hpp"

#include "shark/physics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <thread>

namespace shark::net {
namespace {
Error rejected(const char* operation, const char* message, ErrorCode code = ErrorCode::conflict) {
    return {code, message, {.operation = operation}};
}
math::Vec3 canonical(math::Vec3 value) {
    for (int axis = 0; axis < 3; ++axis)
        if (value[axis] == 0)
            value[axis] = 0;
    return value;
}
Result<PredictedMovementPose> predict(PredictedMovementPose pose, const MovementInput& input,
                                      double gravity) {
    // A small presentation model of M04a's nominal controls. It deliberately
    // does not run collision/native scripts or claim native arithmetic lockstep.
    pose.velocity.x = movement_profile_speed * input.x / movement_axis_scale;
    pose.velocity.z = movement_profile_speed * input.z / movement_axis_scale;
    pose.velocity.y = std::clamp(pose.velocity.y, -movement_profile_vertical_speed,
                                 movement_profile_vertical_speed);
    constexpr auto delta =
        1.0 / (static_cast<double>(movement_profile_tick_hz) * movement_profile_collision_substeps);
    for (unsigned substep = 0; substep < movement_profile_collision_substeps; ++substep) {
        pose.velocity.y =
            std::clamp(pose.velocity.y + gravity * delta, -movement_profile_vertical_speed,
                       movement_profile_vertical_speed);
        pose.position += pose.velocity * delta;
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(pose.position[axis]) ||
                std::abs(pose.position[axis]) > physics_local_position_limit)
                return rejected("net.movement.predict", "Prediction exceeds bounded local space",
                                ErrorCode::invalid_argument);
    }
    pose.position = canonical(pose.position);
    pose.velocity = canonical(pose.velocity);
    return pose;
}
} // namespace

struct MovementPredictor::Impl {
    struct Sent {
        std::uint64_t preparation_revision;
        MovementInput input;
    };
    std::thread::id owner{std::this_thread::get_id()};
    StableId id;
    SessionSnapshot session;
    MovementState authoritative;
    MovementPredictionConfig config;
    PredictedMovementPose predicted;
    std::array<std::optional<Sent>, movement_prediction_maximum_inputs> pending{};
    std::size_t count{0};
    std::uint64_t sent_high_watermark{0};
    std::uint64_t session_high_watermark;
    std::uint64_t revision{0};
    bool reconciliation_required{false};
    std::optional<Sent> latest_sent;

    Impl(StableId identity, SessionSnapshot accepted, MovementState state,
         MovementPredictionConfig model)
        : id(identity), session(accepted), authoritative(state), config(model),
          predicted{state.position, state.velocity},
          session_high_watermark(accepted.last_sequence) {}
    Result<void> check(const char* operation) const {
        if (std::this_thread::get_id() != owner)
            return rejected(operation, "Movement prediction requires its creating thread");
        return {};
    }
    bool retained(const PreparedMovementInput& value) const {
        const auto matches = [&](const Sent& sent) {
            return sent.preparation_revision == value.revision_ && sent.input == value.input_;
        };
        if (latest_sent && matches(*latest_sent))
            return true;
        for (std::size_t index = 0; index < count; ++index)
            if (matches(*pending[index]))
                return true;
        return false;
    }
};

MovementPredictor::MovementPredictor(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
MovementPredictor::~MovementPredictor() = default;

Result<std::unique_ptr<MovementPredictor>>
MovementPredictor::create(const SessionSnapshot& session, const MovementState& initial,
                          MovementPredictionConfig config) {
    constexpr auto operation = "net.movement.prediction.create";
    if (!session.active || !session.ready || session.last_sequence == 0 ||
        session.ownership_epoch == 0 || initial.session != session.session ||
        initial.ownership_epoch != session.ownership_epoch || initial.last_input != 0 ||
        !std::isfinite(config.gravity_y) ||
        std::abs(config.gravity_y) > physics_gravity_component_limit ||
        config.maximum_pending == 0 || config.maximum_pending > movement_prediction_maximum_inputs)
        return rejected(operation, "Prediction requires a fresh ready binding and bounded model",
                        ErrorCode::invalid_argument);
    if (auto valid = validate_movement_state(initial); !valid)
        return valid.error();
    auto owner = generate_stable_id();
    if (!owner)
        return owner.error();
    try {
        return std::unique_ptr<MovementPredictor>{
            new MovementPredictor{std::make_unique<Impl>(owner.value(), session, initial, config)}};
    } catch (...) {
        return rejected(operation, "Cannot allocate movement prediction owner",
                        ErrorCode::execution_failed);
    }
}

Result<PreparedMovementInput> MovementPredictor::prepare(SessionEnvelope authority, std::int16_t x,
                                                         std::int16_t z) const {
    constexpr auto operation = "net.movement.prediction.prepare";
    if (auto valid = impl_->check(operation); !valid)
        return valid.error();
    if (impl_->reconciliation_required || impl_->count == impl_->config.maximum_pending ||
        impl_->revision == std::numeric_limits<std::uint64_t>::max() ||
        impl_->sent_high_watermark == std::numeric_limits<std::uint64_t>::max() ||
        authority.session != impl_->session.session ||
        authority.character != impl_->session.character ||
        authority.ownership_epoch != impl_->session.ownership_epoch ||
        authority.sequence <= impl_->session_high_watermark)
        return rejected(operation,
                        "Prediction admission requires current authority and history capacity");
    MovementInput input{authority, impl_->authoritative.stream, impl_->sent_high_watermark + 1, x,
                        z};
    if (auto valid = validate_movement_input(input); !valid)
        return valid.error();
    auto predicted = predict(impl_->predicted, input, impl_->config.gravity_y);
    if (!predicted)
        return predicted.error();
    auto bytes = encode_movement_input(input);
    if (!bytes)
        return bytes.error();
    return PreparedMovementInput{impl_->id, impl_->revision, input, predicted.value(),
                                 std::move(bytes).value()};
}

Result<PredictionSendResult> MovementPredictor::send(TlsTransport& transport,
                                                     const PreparedMovementInput& prepared) {
    constexpr auto operation = "net.movement.prediction.send";
    if (auto valid = impl_->check(operation); !valid)
        return valid.error();
    if (prepared.owner_ != impl_->id)
        return rejected(operation, "Prepared movement belongs to another prediction owner");
    if (impl_->retained(prepared))
        return PredictionSendResult::duplicate;
    if (impl_->reconciliation_required || impl_->revision != prepared.revision_ ||
        impl_->revision == std::numeric_limits<std::uint64_t>::max() ||
        impl_->count == impl_->config.maximum_pending)
        return rejected(operation, "Prepared movement is stale, suspended or history is full");
    // Everything below admission is a fixed-size value publication, without
    // callbacks, allocation or a new validation/correction window.
    if (auto sent = transport.send(MessageKind::movement_input, prepared.bytes_); !sent)
        return sent.error();
    Impl::Sent receipt{prepared.revision_, prepared.input_};
    impl_->pending[impl_->count] = receipt;
    ++impl_->count;
    impl_->latest_sent = receipt;
    impl_->sent_high_watermark = prepared.input_.input_sequence;
    impl_->session_high_watermark = prepared.input_.authority.sequence;
    impl_->predicted = prepared.predicted_;
    ++impl_->revision;
    return PredictionSendResult::admitted;
}

Result<PredictionCorrectionResult> MovementPredictor::apply(const MovementReply& correction) {
    constexpr auto operation = "net.movement.prediction.correct";
    if (auto valid = impl_->check(operation); !valid)
        return valid.error();
    if (std::holds_alternative<MovementRejected>(correction)) {
        if (auto stopped = suspend(); !stopped)
            return stopped.error();
        return PredictionCorrectionResult::reconciliation_required;
    }
    const auto* state = std::get_if<MovementState>(&correction);
    if (!state)
        return rejected(operation, "Unknown movement correction", ErrorCode::invalid_argument);
    if (auto valid = validate_movement_state(*state); !valid)
        return valid.error();
    const auto& previous = impl_->authoritative;
    if (state->session != previous.session || state->ownership_epoch != previous.ownership_epoch ||
        state->stream != previous.stream)
        return rejected(operation, "Movement correction belongs to a different binding");
    if (*state == previous)
        return PredictionCorrectionResult::duplicate;
    if (state->completed_tick <= previous.completed_tick ||
        state->last_input < previous.last_input || state->last_input > impl_->sent_high_watermark ||
        state->last_input - previous.last_input > state->completed_tick - previous.completed_tick ||
        impl_->revision == std::numeric_limits<std::uint64_t>::max())
        return rejected(operation,
                        "Movement correction has stale or impossible tick/input progression");
    std::size_t acknowledged = 0;
    while (acknowledged < impl_->count &&
           impl_->pending[acknowledged]->input.input_sequence <= state->last_input)
        ++acknowledged;
    PredictedMovementPose candidate{state->position, state->velocity};
    bool replay_failed = false;
    if (!impl_->reconciliation_required) {
        for (std::size_t index = acknowledged; index < impl_->count; ++index) {
            auto replayed =
                predict(candidate, impl_->pending[index]->input, impl_->config.gravity_y);
            if (!replayed) {
                // An approximate future cannot veto a valid committed server
                // correction. Retain exact unacknowledged history and suspend.
                replay_failed = true;
                candidate = {state->position, state->velocity};
                break;
            }
            candidate = replayed.value();
        }
    }
    // Complete correction and bounded replay validate before changing history.
    for (std::size_t index = acknowledged; index < impl_->count; ++index)
        impl_->pending[index - acknowledged] = std::move(impl_->pending[index]);
    const auto remaining = impl_->count - acknowledged;
    for (std::size_t index = remaining; index < impl_->count; ++index)
        impl_->pending[index].reset();
    impl_->count = remaining;
    impl_->authoritative = *state;
    impl_->predicted = candidate;
    impl_->reconciliation_required = impl_->reconciliation_required || replay_failed;
    ++impl_->revision;
    return replay_failed ? PredictionCorrectionResult::reconciliation_required
                         : PredictionCorrectionResult::applied;
}

Result<void> MovementPredictor::suspend() {
    if (auto valid = impl_->check("net.movement.prediction.suspend"); !valid)
        return valid;
    if (!impl_->reconciliation_required) {
        impl_->reconciliation_required = true;
        impl_->predicted = {impl_->authoritative.position, impl_->authoritative.velocity};
        if (impl_->revision != std::numeric_limits<std::uint64_t>::max())
            ++impl_->revision;
    }
    return {};
}

Result<MovementPredictionSnapshot> MovementPredictor::snapshot() const {
    if (auto valid = impl_->check("net.movement.prediction.snapshot"); !valid)
        return valid.error();
    return MovementPredictionSnapshot{impl_->authoritative, impl_->predicted, impl_->count,
                                      impl_->revision, impl_->reconciliation_required};
}

} // namespace shark::net
