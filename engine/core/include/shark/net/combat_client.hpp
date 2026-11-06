#pragma once

#include "shark/net/combat.hpp"

#include <memory>

namespace shark::net {

struct CombatOutcomeGap {
    std::uint64_t expected;
    std::uint64_t observed;
    bool operator==(const CombatOutcomeGap&) const = default;
};
enum class CombatClientApplyResult {
    applied,
    duplicate,
    resync_required,
    rejected,
    resynchronized
};
struct CombatClientUpdate {
    CombatClientApplyResult status;
    // Owned presentation observations, at most eight. Never execute gameplay,
    // Lua, durable operations or prediction from these historical values.
    std::vector<CombatActionResult> effects;
    std::optional<CombatOutcomeGap> gap;
};
struct CombatClientSnapshot {
    CombatState state;
    // Highest observed outcome deliberately accepted for presentation. Initial
    // and explicit resync baselines skip old effects rather than emitting them.
    std::uint64_t outcome_watermark;
    bool requires_resync;
    // First gap is retained across subsequent packets and explicit resync.
    std::optional<CombatOutcomeGap> gap;
};

// Creating-thread CPU presentation cache. Feed only replies received from this
// accepted session's verified server. It owns no Scene, Lua or durable service.
// A fresh life/binding/session constructs a fresh owner; packets cannot rebind it.
class CombatClient final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<CombatClient>>
    create(const SessionSnapshot& session, const CombatState& initial);
    ~CombatClient();
    CombatClient(const CombatClient&) = delete;
    CombatClient& operator=(const CombatClient&) = delete;

    // Stage complete state/effects before one publication. Exact repeats emit
    // nothing. Missing outcome history preserves valid state but freezes effects.
    [[nodiscard]] Result<CombatClientUpdate> apply(const CombatReply& reply);
    // Explicit verified-server baseline acknowledgement, not an automatic retry
    // or a session-sequence reconciliation. Never emits skipped historical effects.
    [[nodiscard]] Result<CombatClientUpdate> resynchronize(const CombatState& state);
    [[nodiscard]] Result<CombatClientSnapshot> snapshot() const;

  private:
    struct Impl;
    explicit CombatClient(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::net
