#include "shark/net/combat_client.hpp"

#include <algorithm>
#include <limits>
#include <thread>

namespace shark::net {
namespace {
Error rejected(const char* operation, const char* message, ErrorCode code = ErrorCode::conflict) {
    return {code, message, {.operation = operation}};
}
std::uint64_t last_outcome(const CombatState& state) {
    return state.outcomes.empty() ? 0 : state.outcomes.back().action_sequence;
}
} // namespace

struct CombatClient::Impl {
    std::thread::id owner{std::this_thread::get_id()};
    CombatState state;
    std::vector<CombatActionResult> retained;
    std::uint64_t watermark;
    std::uint64_t observed;
    bool requires_resync{false};
    std::optional<CombatOutcomeGap> gap;

    explicit Impl(const CombatState& initial)
        : state(initial), retained(initial.outcomes), watermark(last_outcome(initial)),
          observed(watermark) {}
    Result<void> check(const char* operation) const {
        if (owner != std::this_thread::get_id())
            return rejected(operation, "Combat presentation requires its creating thread");
        return {};
    }
    Result<void> validate(const CombatState& value) const {
        if (auto valid = validate_combat_state(value); !valid)
            return valid;
        if (value.session != state.session || value.ownership_epoch != state.ownership_epoch ||
            value.stream != state.stream || value.self != state.self ||
            (value.movement && state.movement &&
             value.movement->stream != state.movement->stream) ||
            (state.health == 0 && value.health != 0))
            return rejected("net.combat.client.scope",
                            "Combat reply belongs to a different binding or life");
        if (value.completed_tick < state.completed_tick ||
            (value.completed_tick == state.completed_tick && value != state))
            return rejected("net.combat.client.tick",
                            "Combat reply is stale or conflicts with its tick");
        if (value.movement && state.movement &&
            (value.movement->last_input < state.movement->last_input ||
             value.movement->last_input - state.movement->last_input >
                 value.completed_tick - state.completed_tick))
            return rejected("net.combat.client.movement",
                            "Movement acknowledgement has stale or impossible tick progression");
        if (last_outcome(value) < observed)
            return rejected("net.combat.client.outcome", "Combat outcome history regressed");
        if (last_outcome(value) - observed > value.completed_tick - state.completed_tick)
            return rejected("net.combat.client.outcome",
                            "Combat outcomes exceed completed tick progression");
        for (const auto& outcome : value.outcomes) {
            const auto prior =
                std::find_if(retained.begin(), retained.end(), [&](const auto& known) {
                    return known.action_sequence == outcome.action_sequence;
                });
            if (prior != retained.end() && *prior != outcome)
                return rejected("net.combat.client.outcome",
                                "Combat outcome conflicts with its original receipt");
        }
        return {};
    }
    std::vector<CombatActionResult> history(const CombatState& value) const {
        auto candidate = retained;
        candidate.reserve(combat_maximum_outcomes * 2);
        for (const auto& outcome : value.outcomes) {
            const auto prior =
                std::find_if(candidate.begin(), candidate.end(), [&](const auto& known) {
                    return known.action_sequence == outcome.action_sequence;
                });
            if (prior == candidate.end())
                candidate.push_back(outcome);
        }
        std::sort(candidate.begin(), candidate.end(), [](const auto& a, const auto& b) {
            return a.action_sequence < b.action_sequence;
        });
        if (candidate.size() > combat_maximum_outcomes)
            candidate.erase(candidate.begin(),
                            candidate.end() - static_cast<std::ptrdiff_t>(combat_maximum_outcomes));
        return candidate;
    }
};

CombatClient::CombatClient(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
CombatClient::~CombatClient() = default;

Result<std::unique_ptr<CombatClient>> CombatClient::create(const SessionSnapshot& session,
                                                           const CombatState& initial) {
    constexpr auto operation = "net.combat.client.create";
    if (!session.active || !session.ready || session.ownership_epoch == 0 ||
        session.last_sequence == 0 || initial.session != session.session ||
        initial.ownership_epoch != session.ownership_epoch)
        return rejected(operation, "Combat presentation requires its ready accepted session",
                        ErrorCode::invalid_argument);
    if (auto valid = validate_combat_state(initial); !valid)
        return valid.error();
    try {
        return std::unique_ptr<CombatClient>{new CombatClient{std::make_unique<Impl>(initial)}};
    } catch (...) {
        return rejected(operation, "Cannot allocate combat presentation owner",
                        ErrorCode::execution_failed);
    }
}

Result<CombatClientUpdate> CombatClient::apply(const CombatReply& reply) {
    constexpr auto operation = "net.combat.client.apply";
    if (auto owner = impl_->check(operation); !owner)
        return owner.error();
    if (std::holds_alternative<CombatRejected>(reply)) {
        impl_->requires_resync = true;
        return CombatClientUpdate{CombatClientApplyResult::rejected, {}, impl_->gap};
    }
    const auto* state = std::get_if<CombatState>(&reply);
    if (!state)
        return rejected(operation, "Unknown combat reply", ErrorCode::invalid_argument);
    if (auto valid = impl_->validate(*state); !valid)
        return valid.error();
    if (*state == impl_->state)
        return CombatClientUpdate{impl_->requires_resync ? CombatClientApplyResult::resync_required
                                                         : CombatClientApplyResult::duplicate,
                                  {},
                                  impl_->gap};
    try {
        CombatState candidate = *state;
        auto history = impl_->history(candidate);
        auto gap = impl_->gap;
        bool requires_resync = impl_->requires_resync;
        CombatClientUpdate update{CombatClientApplyResult::applied, {}, gap};
        update.effects.reserve(combat_maximum_outcomes);
        const auto first = std::find_if(
            candidate.outcomes.begin(), candidate.outcomes.end(),
            [&](const auto& outcome) { return outcome.action_sequence > impl_->watermark; });
        if (first != candidate.outcomes.end() &&
            (impl_->watermark == std::numeric_limits<std::uint64_t>::max() ||
             first->action_sequence != impl_->watermark + 1)) {
            requires_resync = true;
            if (!gap)
                gap = CombatOutcomeGap{impl_->watermark + 1, first->action_sequence};
        }
        auto watermark = impl_->watermark;
        if (requires_resync) {
            update.status = CombatClientApplyResult::resync_required;
        } else {
            update.effects.assign(first, candidate.outcomes.end());
            watermark = last_outcome(candidate);
        }
        update.gap = gap;
        const auto observed = last_outcome(candidate);
        impl_->state = std::move(candidate);
        impl_->retained.swap(history);
        impl_->observed = observed;
        impl_->watermark = watermark;
        impl_->requires_resync = requires_resync;
        impl_->gap = gap;
        return update;
    } catch (...) {
        return rejected(operation, "Cannot stage complete combat presentation",
                        ErrorCode::execution_failed);
    }
}

Result<CombatClientUpdate> CombatClient::resynchronize(const CombatState& state) {
    constexpr auto operation = "net.combat.client.resynchronize";
    if (auto owner = impl_->check(operation); !owner)
        return owner.error();
    if (auto valid = impl_->validate(state); !valid)
        return valid.error();
    if (state == impl_->state && !impl_->requires_resync)
        return CombatClientUpdate{CombatClientApplyResult::duplicate, {}, impl_->gap};
    try {
        CombatState candidate = state;
        auto history = impl_->history(candidate);
        const auto watermark = last_outcome(candidate);
        impl_->state = std::move(candidate);
        impl_->retained.swap(history);
        impl_->watermark = watermark;
        impl_->observed = watermark;
        impl_->requires_resync = false;
        return CombatClientUpdate{CombatClientApplyResult::resynchronized, {}, impl_->gap};
    } catch (...) {
        return rejected(operation, "Cannot stage combat presentation baseline",
                        ErrorCode::execution_failed);
    }
}

Result<CombatClientSnapshot> CombatClient::snapshot() const {
    constexpr auto operation = "net.combat.client.snapshot";
    if (auto owner = impl_->check(operation); !owner)
        return owner.error();
    try {
        return CombatClientSnapshot{impl_->state, impl_->watermark, impl_->requires_resync,
                                    impl_->gap};
    } catch (...) {
        return rejected(operation, "Cannot capture combat presentation",
                        ErrorCode::execution_failed);
    }
}

} // namespace shark::net
