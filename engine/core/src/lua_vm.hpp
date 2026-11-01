#pragma once

#include "shark/error.hpp"

#include <cstddef>
#include <memory>
#include <string_view>

namespace shark::detail {

// Private primitive, not a gameplay or authoring API. This owner does not give
// scripts access to a Scene, the filesystem, network, or engine services.
struct LuaVmConfig {
    std::size_t memory_limit_bytes{4 * 1024 * 1024};
    std::size_t instruction_limit{100000};
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
    [[nodiscard]] std::size_t memory_used() const noexcept;

  private:
    struct Impl;
    explicit LuaVm(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::detail
