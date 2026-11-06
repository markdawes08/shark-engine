#include <shark/net/combat_client.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <thread>

namespace {
using namespace shark;
using namespace shark::net;

template <typename T> T take(Result<T> result) {
    if (!result)
        INFO(result.error().context.operation << ": " << result.error().message);
    REQUIRE(result);
    return std::move(result).value();
}
template <typename T> T id(unsigned value) {
    std::string text = "a04d0000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 15U];
        value >>= 4U;
    }
    return T{take(StableId::parse(text))};
}
SessionSnapshot session() {
    return {id<SessionId>(1), id<AccountId>(2), id<CharacterId>(3), 1, 1, true, true};
}
CombatState state(std::uint64_t tick = 10, std::uint64_t first = 0, std::uint64_t last = 0) {
    const auto accepted = session();
    CombatState result{
        accepted.session,
        accepted.ownership_epoch,
        id<CombatStreamId>(4),
        tick,
        id<CombatTargetId>(5),
        100,
        0,
        0,
        MovementState{
            accepted.session, accepted.ownership_epoch, id<MovementStreamId>(6), tick, 0, {}, {}},
        {{id<CombatTargetId>(7), 100, 0}},
        {}};
    if (first != 0)
        for (auto sequence = first; sequence <= last; ++sequence)
            result.outcomes.push_back({sequence, id<CombatTargetId>(7), CombatActionKind::strike,
                                       CombatOutcome::hit, 25});
    return result;
}
void same(const CombatClientSnapshot& actual, const CombatClientSnapshot& expected) {
    REQUIRE(actual.state == expected.state);
    REQUIRE(actual.outcome_watermark == expected.outcome_watermark);
    REQUIRE(actual.requires_resync == expected.requires_resync);
    REQUIRE(actual.gap == expected.gap);
}
} // namespace

TEST_CASE("Combat presentation baselines old outcomes and emits each new receipt once",
          "[combat-client]") {
    const auto baseline = state(10, 1, 3);
    auto client = take(CombatClient::create(session(), baseline));
    REQUIRE(take(client->snapshot()).outcome_watermark == 3);
    const auto repeated = take(client->apply(CombatReply{baseline}));
    REQUIRE(repeated.status == CombatClientApplyResult::duplicate);
    REQUIRE(repeated.effects.empty());
    const auto next = state(11, 1, 4);
    const auto update = take(client->apply(CombatReply{next}));
    REQUIRE(update.status == CombatClientApplyResult::applied);
    REQUIRE(update.effects.size() == 1);
    REQUIRE(update.effects.front() == next.outcomes.back());
    REQUIRE(take(client->apply(CombatReply{next})).effects.empty());
    REQUIRE(take(client->snapshot()).outcome_watermark == 4);
    const auto later = state(12, 2, 5);
    REQUIRE(take(client->apply(CombatReply{later})).effects.front().action_sequence == 5);
    auto owned = take(client->snapshot());
    owned.state.targets.clear();
    owned.state.outcomes.front().amount = 1;
    REQUIRE(take(client->snapshot()).state == later);
}

TEST_CASE("Missing combat effects freeze delivery and explicit resync never replays a baseline",
          "[combat-client]") {
    auto client = take(CombatClient::create(session(), state()));
    auto missing = state(20, 3, 6);
    missing.health = 75;
    const auto gap = take(client->apply(CombatReply{missing}));
    REQUIRE(gap.status == CombatClientApplyResult::resync_required);
    REQUIRE(gap.effects.empty());
    REQUIRE((gap.gap == CombatOutcomeGap{1, 3}));
    auto snapshot = take(client->snapshot());
    REQUIRE(snapshot.state == missing);
    REQUIRE(snapshot.requires_resync);
    REQUIRE(snapshot.outcome_watermark == 0);
    const auto later = state(21, 4, 7);
    const auto frozen = take(client->apply(CombatReply{later}));
    REQUIRE(frozen.effects.empty());
    REQUIRE(frozen.gap == gap.gap);
    REQUIRE(take(client->apply(CombatReply{later})).status ==
            CombatClientApplyResult::resync_required);
    const auto reconciled = take(client->resynchronize(later));
    REQUIRE(reconciled.status == CombatClientApplyResult::resynchronized);
    REQUIRE(reconciled.effects.empty());
    REQUIRE(reconciled.gap == gap.gap);
    snapshot = take(client->snapshot());
    REQUIRE_FALSE(snapshot.requires_resync);
    REQUIRE(snapshot.outcome_watermark == 7);
    REQUIRE(snapshot.gap == gap.gap);
    REQUIRE(take(client->resynchronize(later)).status == CombatClientApplyResult::duplicate);
    const auto fresh = take(client->apply(CombatReply{state(22, 1, 8)}));
    REQUIRE(fresh.effects.size() == 1);
    REQUIRE(fresh.effects.front().action_sequence == 8);
    REQUIRE(fresh.gap == gap.gap);

    REQUIRE(take(client->apply(CombatReply{CombatRejected{}})).status ==
            CombatClientApplyResult::rejected);
    REQUIRE(take(client->snapshot()).requires_resync);
    const auto after_rejection = state(23, 2, 9);
    REQUIRE(take(client->apply(CombatReply{after_rejection})).effects.empty());
    REQUIRE(take(client->snapshot()).outcome_watermark == 8);
    REQUIRE(take(client->resynchronize(after_rejection)).effects.empty());
    REQUIRE(take(client->snapshot()).outcome_watermark == 9);
    REQUIRE(take(client->apply(CombatReply{state(24, 3, 10)})).effects.front().action_sequence ==
            10);
}

TEST_CASE(
    "Combat presentation rejects stale scopes ticks and conflicting original receipts atomically",
    "[combat-client]") {
    auto client = take(CombatClient::create(session(), state()));
    auto accepted = state(11, 1, 1);
    accepted.movement->last_input = 1;
    REQUIRE(take(client->apply(CombatReply{accepted})).effects.size() == 1);
    const auto before = take(client->snapshot());
    for (unsigned variant = 0; variant < 12; ++variant) {
        INFO(variant);
        auto invalid = state(12, 1, 2);
        invalid.movement->last_input = 2;
        switch (variant) {
        case 0:
            invalid.session = id<SessionId>(90);
            invalid.movement->session = invalid.session;
            break;
        case 1:
            ++invalid.ownership_epoch;
            invalid.movement->ownership_epoch = invalid.ownership_epoch;
            break;
        case 2:
            invalid.stream = id<CombatStreamId>(90);
            break;
        case 3:
            invalid.self = id<CombatTargetId>(90);
            break;
        case 4:
            invalid.movement->stream = id<MovementStreamId>(90);
            break;
        case 5:
            invalid.completed_tick = 11;
            invalid.movement->completed_tick = 11;
            break;
        case 6:
            invalid = state(10);
            break;
        case 7:
            invalid.outcomes.front().amount = 24;
            break;
        case 8:
            invalid.outcomes.clear();
            break;
        case 9:
            invalid = state(12, 1, 3); // Two new outcomes in one completed tick.
            invalid.movement->last_input = 2;
            break;
        case 10:
            invalid.movement->last_input = 0; // A newer tick cannot revoke an applied input.
            break;
        default:
            invalid.movement->last_input = 3; // Two movement inputs cannot apply in one tick.
            break;
        }
        REQUIRE_FALSE(client->apply(CombatReply{invalid}));
        same(take(client->snapshot()), before);
        REQUIRE_FALSE(client->resynchronize(invalid));
        same(take(client->snapshot()), before);
    }
    auto progressed = state(12, 1, 2);
    progressed.movement->last_input = 2;
    REQUIRE(take(client->apply(CombatReply{progressed})).effects.size() == 1);
    REQUIRE(take(client->snapshot()).state.movement->last_input == 2);
}

TEST_CASE("A new combat life or session requires a fresh explicit presentation owner",
          "[combat-client]") {
    auto client = take(CombatClient::create(session(), state()));
    auto dead = state(11);
    dead.health = 0;
    dead.respawn_tick = 71;
    dead.movement.reset();
    REQUIRE(take(client->apply(CombatReply{dead})).status == CombatClientApplyResult::applied);
    auto same_life_revived = state(71);
    REQUIRE_FALSE(client->apply(CombatReply{same_life_revived}));
    auto fresh = state(71);
    fresh.self = id<CombatTargetId>(40);
    fresh.stream = id<CombatStreamId>(41);
    fresh.movement->stream = id<MovementStreamId>(42);
    REQUIRE_FALSE(client->apply(CombatReply{fresh}));
    REQUIRE_FALSE(client->resynchronize(fresh));
    auto replacement = take(CombatClient::create(session(), fresh));
    REQUIRE_FALSE(replacement->apply(CombatReply{dead}));
    const auto after_respawn = fresh;
    REQUIRE(take(replacement->apply(CombatReply{after_respawn})).effects.empty());
    auto next_session = session();
    next_session.session = id<SessionId>(43);
    ++next_session.ownership_epoch;
    fresh.session = next_session.session;
    fresh.ownership_epoch = next_session.ownership_epoch;
    fresh.movement->session = next_session.session;
    fresh.movement->ownership_epoch = next_session.ownership_epoch;
    REQUIRE_FALSE(replacement->apply(CombatReply{fresh}));
    auto reconnected = take(CombatClient::create(next_session, fresh));
    REQUIRE_FALSE(reconnected->apply(CombatReply{after_respawn}));
}

TEST_CASE(
    "Combat presentation remains bounded and owner-thread checked across long outcome streams",
    "[combat-client]") {
    auto invalid_session = session();
    invalid_session.ready = false;
    REQUIRE_FALSE(CombatClient::create(invalid_session, state()));
    invalid_session = session();
    invalid_session.last_sequence = 0;
    REQUIRE_FALSE(CombatClient::create(invalid_session, state()));
    auto invalid_state = state();
    invalid_state.health = 101;
    REQUIRE_FALSE(CombatClient::create(session(), invalid_state));
    auto client = take(CombatClient::create(session(), state()));
    for (std::uint64_t sequence = 1; sequence <= 32; ++sequence) {
        const auto next = state(10 + sequence, sequence > 8 ? sequence - 7 : 1, sequence);
        const auto update = take(client->apply(CombatReply{next}));
        REQUIRE(update.effects.size() == 1);
        REQUIRE(update.effects.front().action_sequence == sequence);
        REQUIRE(take(client->snapshot()).state.outcomes.size() <= 8);
        REQUIRE(take(client->snapshot()).outcome_watermark == sequence);
    }
    const auto coalesced = state(50, 33, 40);
    const auto complete = take(client->apply(CombatReply{coalesced}));
    REQUIRE(complete.effects.size() == 8);
    REQUIRE(complete.effects.front().action_sequence == 33);
    REQUIRE(complete.effects.back().action_sequence == 40);
    const auto before = take(client->snapshot());
    bool rejected_snapshot = false;
    bool rejected_apply = false;
    std::thread foreign{[&] {
        rejected_snapshot = !client->snapshot();
        rejected_apply = !client->apply(CombatReply{coalesced});
    }};
    foreign.join();
    REQUIRE(rejected_snapshot);
    REQUIRE(rejected_apply);
    same(take(client->snapshot()), before);
    REQUIRE(take(client->resynchronize(coalesced)).effects.empty());
    REQUIRE(take(client->apply(CombatReply{coalesced})).effects.empty());
}
