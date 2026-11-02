#pragma once

#include <cstdint>

namespace shark::detail {

// Private per-thread marker for the locked single-thread native contact
// collector. No Scene, Lua or application callback may run in this scope.
// The read-only count lets Core integration tests observe an actual native
// callback without exposing a mutable fixture port to engine consumers.
class NativeContactScope final {
  public:
    NativeContactScope() noexcept;
    ~NativeContactScope() noexcept;
    NativeContactScope(const NativeContactScope&) = delete;
    NativeContactScope& operator=(const NativeContactScope&) = delete;

    [[nodiscard]] static bool active() noexcept;
    [[nodiscard]] static std::uint64_t entered() noexcept;

  private:
    bool previous_;
    static thread_local bool active_;
    static thread_local std::uint64_t entered_;
};

} // namespace shark::detail
