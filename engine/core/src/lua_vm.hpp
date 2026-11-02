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

enum class LuaPhase { load, init, update, fixed_update, contact, destroy };
enum class LuaProfile { server_gameplay, client_presentation };

// Private POD bridge: the host owns physics and catches all exceptions. The
// VM may retain these values only across a single protected call. Hit identities
// are immutable UUID strings, never native pointers or actionable body handles.
inline constexpr std::size_t lua_physics_maximum_hits = 32;
enum class LuaPhysicsOperation {
    velocity,
    set_velocity,
    force,
    impulse,
    teleport,
    kinematic_target,
    raycast,
    overlap_sphere,
    overlap_box,
    remove_body,
    create_body
};
struct LuaPhysicsRequest {
    LuaPhysicsOperation operation{};
    double arguments[9]{};
    std::uint32_t layer{0};
    std::uint32_t mask{0xffff};
    bool include_triggers{true};
    std::uint32_t max_hits{static_cast<std::uint32_t>(lua_physics_maximum_hits)};
};
struct LuaPhysicsHit {
    char entity_id[37]{};
    bool is_trigger{false};
    double fraction{0};
    double position[3]{};
    double normal[3]{};
    bool has_normal{false};
};
struct LuaPhysicsReply {
    char error[257]{};
    double values[6]{};
    std::size_t count{0};
    LuaPhysicsHit hits[lua_physics_maximum_hits]{};
};
struct LuaContactInfo {
    char other_entity_id[37]{};
    bool self_trigger{false};
    bool other_trigger{false};
    std::uint64_t completed_tick{0};
};

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
    void* physics_user{nullptr};
    void (*physics_call)(void*, const EntityReference&, const LuaPhysicsRequest&,
                         LuaPhysicsReply&) noexcept {nullptr};
    const LuaContactInfo* contact{nullptr};
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
