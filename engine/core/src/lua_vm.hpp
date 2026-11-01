#pragma once

#include "shark/error.hpp"
#include "shark/scene.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace shark::detail {

// Private VM primitive, not a public authoring API. Contextual calls expose
// checked, capability-limited Scene access only during protected execution.
// Raw calls expose no engine services. No filesystem or network access is added.
struct LuaVmConfig {
    std::size_t memory_limit_bytes{4 * 1024 * 1024};
    std::size_t instruction_limit{100000};
};

enum class LuaPhase { load, init, update, fixed_update, destroy };
enum class LuaProfile { server_gameplay, client_presentation };

// Borrowed only during a synchronous protected call; never retained by Lua.
// The host owns the bounded log sink and must catch all of its exceptions.
struct LuaCallContext {
    Scene* scene;
    EntityReference entity;
    LuaPhase phase;
    LuaProfile profile;
    std::uint64_t tick;
    double delta_seconds;
    void* log_user;
    bool (*log)(void*, std::string_view) noexcept;
};

class LuaVm final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<LuaVm>> create(LuaVmConfig config);
    ~LuaVm();

    LuaVm(const LuaVm&) = delete;
    LuaVm& operator=(const LuaVm&) = delete;
    LuaVm(LuaVm&&) = delete;
    LuaVm& operator=(LuaVm&&) = delete;

    // Executes UTF-8 source in the retained VM; failures do not roll back
    // global changes. The instruction budget covers Lua instructions, not
    // wall time or work inside native library calls. This is not a sandbox.
    // All access and destruction belong to the creating thread.
    [[nodiscard]] Result<void> execute(std::string_view source, std::string_view chunk_name);
    [[nodiscard]] Result<void> execute(std::string_view source, std::string_view chunk_name,
                                       LuaCallContext& context);
    // Only the four named lifecycle callbacks are admitted. Nil is a no-op.
    [[nodiscard]] Result<void> invoke(std::string_view callback, LuaCallContext& context);
    [[nodiscard]] std::size_t memory_used() const noexcept;

  private:
    struct Impl;
    explicit LuaVm(std::unique_ptr<Impl> impl) noexcept;
    [[nodiscard]] Result<void> execute_impl(std::string_view source, std::string_view chunk_name,
                                            LuaCallContext* context);
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::detail
