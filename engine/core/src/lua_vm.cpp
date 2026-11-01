#include "lua_vm.hpp"

#include "utf8.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace shark::detail {
namespace {

constexpr std::size_t min_memory = 64 * 1024;
constexpr std::size_t max_memory = 64 * 1024 * 1024;
constexpr std::size_t max_instructions = 10'000'000;
constexpr std::size_t max_source_bytes = 256 * 1024;
constexpr std::size_t max_chunk_name_bytes = 4096;
constexpr std::size_t max_diagnostic_bytes = 768;
constexpr std::size_t max_binding_text = 256;

Error failure(ErrorCode code, std::string message, const char* operation,
              std::string_view path = {}) {
    return {code, std::move(message), {.operation = operation, .path = std::string(path)}};
}

// Preserve an owned bounded, valid UTF-8 diagnostic even when Lua error values
// contain arbitrary byte strings. ASCII preserves source locations and traces.
std::string diagnostic(lua_State* state, int index) {
    if (lua_type(state, index) != LUA_TSTRING) {
        return "Lua raised a non-string error";
    }
    std::size_t length = 0;
    const char* value = lua_tolstring(state, index, &length);
    std::string result;
    result.reserve(length < max_diagnostic_bytes ? length : max_diagnostic_bytes);
    for (std::size_t i = 0; i < length && i < max_diagnostic_bytes; ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        result.push_back((c >= 32U && c < 127U) || c == '\n' || c == '\t' ? static_cast<char>(c)
                                                                          : '?');
    }
    return result;
}

} // namespace

struct LuaVm::Impl {
    // Trampolines contain only these trivially destructible values when calling
    // Lua APIs that may allocate or raise through the C longjmp boundary.
    struct BindingResult {
        char error[max_binding_text + 1]{};
        char text[max_binding_text + 1]{};
        std::size_t length{0};
        double x{0};
        double y{0};
        double z{0};
        lua_Integer integer{0};
    };
    static_assert(std::is_trivially_destructible_v<BindingResult>);

    static void binding_error(BindingResult& result, std::string_view text) noexcept {
        if (text.empty())
            text = "Script binding failed";
        const auto count = text.size() < max_binding_text ? text.size() : max_binding_text;
        for (std::size_t index = 0; index < count; ++index) {
            const auto byte = static_cast<unsigned char>(text[index]);
            result.error[index] = byte >= 32U && byte < 127U ? static_cast<char>(byte) : '?';
        }
        result.error[count] = '\0';
    }

    static Impl* current(lua_State* state) noexcept {
        void* userdata = nullptr;
        static_cast<void>(lua_getallocf(state, &userdata));
        return static_cast<Impl*>(userdata);
    }

    static bool valid_context(Impl* self, BindingResult& result) noexcept {
        if (self == nullptr || self->active == nullptr || self->active->scene == nullptr) {
            binding_error(result, "No active script callback context");
            return false;
        }
        return true;
    }

    enum class ReadField { name, position, camera_near };

    // No Lua function that can allocate or raise is called in this helper.
    // Any C++ temporaries and Result values are destroyed before the caller
    // pushes the copied POD output or raises a Lua error.
    static BindingResult read_entity(lua_State* state, ReadField field) noexcept {
        BindingResult output;
        auto* self = current(state);
        if (!valid_context(self, output))
            return output;
        if (lua_gettop(state) != 0) {
            binding_error(output, "Expected no arguments");
            return output;
        }
        try {
            auto handle = self->active->scene->resolve(self->active->entity);
            if (!handle) {
                binding_error(output, handle.error().message);
                return output;
            }
            auto entity = self->active->scene->entity_state(handle.value());
            if (!entity) {
                binding_error(output, entity.error().message);
                return output;
            }
            if (field == ReadField::name) {
                const auto& text = entity.value().name.value;
                if (text.size() > max_binding_text) {
                    binding_error(output, "Entity name exceeds the script API bound");
                    return output;
                }
                std::memcpy(output.text, text.data(), text.size());
                output.length = text.size();
            } else if (field == ReadField::position) {
                const auto& translation = entity.value().local_transform.translation;
                output.x = translation.x;
                output.y = translation.y;
                output.z = translation.z;
            } else if (!entity.value().camera) {
                binding_error(output, "Entity has no Camera component");
            } else {
                output.x = entity.value().camera->near_clip;
            }
        } catch (...) {
            binding_error(output, "Cannot inspect the script entity");
        }
        return output;
    }

    static BindingResult set_entity_name(lua_State* state) noexcept {
        BindingResult output;
        auto* self = current(state);
        if (!valid_context(self, output))
            return output;
        auto& context = *self->active;
        if (context.profile != LuaProfile::server_gameplay ||
            context.phase != LuaPhase::fixed_update ||
            !std::holds_alternative<RegionSceneScope>(context.entity.scope)) {
            binding_error(output, "Entity mutation requires authoritative server fixed_update");
            return output;
        }
        if (lua_gettop(state) != 1 || lua_type(state, 1) != LUA_TSTRING) {
            binding_error(output, "Expected one name string");
            return output;
        }
        std::size_t length = 0;
        const char* text = lua_tolstring(state, 1, &length); // Exact string: no conversion.
        if (length == 0 || length > max_binding_text) {
            binding_error(output, "Name must be 1..256 bytes");
            return output;
        }
        try {
            auto handle = context.scene->resolve(context.entity);
            if (!handle) {
                binding_error(output, handle.error().message);
                return output;
            }
            auto changed = context.scene->set_name(handle.value(), Name{std::string(text, length)});
            if (!changed)
                binding_error(output, changed.error().message);
        } catch (...) {
            binding_error(output, "Cannot update the script entity name");
        }
        return output;
    }

    static BindingResult write_log(lua_State* state) noexcept {
        BindingResult output;
        auto* self = current(state);
        if (!valid_context(self, output))
            return output;
        if (lua_gettop(state) != 1 || lua_type(state, 1) != LUA_TSTRING) {
            binding_error(output, "Expected one log string");
            return output;
        }
        std::size_t length = 0;
        const char* text = lua_tolstring(state, 1, &length); // Exact string: no conversion.
        if (length == 0 || length > max_binding_text || !valid_utf8({text, length}, true)) {
            binding_error(output, "Log text must be 1..256 valid UTF-8 bytes without controls");
            return output;
        }
        if (self->active->log == nullptr ||
            !self->active->log(self->active->log_user, std::string_view{text, length}))
            binding_error(output, "Script log buffer is full or unavailable");
        return output;
    }

    static BindingResult read_time(lua_State* state) noexcept {
        BindingResult output;
        auto* self = current(state);
        if (!valid_context(self, output))
            return output;
        if (lua_gettop(state) != 0) {
            binding_error(output, "Expected no arguments");
            return output;
        }
        output.integer = static_cast<lua_Integer>(self->active->tick);
        output.x = self->active->delta_seconds;
        return output;
    }

    static int name(lua_State* state) {
        const auto value = read_entity(state, ReadField::name);
        if (value.error[0])
            return luaL_error(state, "%s", value.error);
        lua_pushlstring(state, value.text, value.length);
        return 1;
    }
    static int position(lua_State* state) {
        const auto value = read_entity(state, ReadField::position);
        if (value.error[0])
            return luaL_error(state, "%s", value.error);
        lua_pushnumber(state, value.x);
        lua_pushnumber(state, value.y);
        lua_pushnumber(state, value.z);
        return 3;
    }
    static int camera_near(lua_State* state) {
        const auto value = read_entity(state, ReadField::camera_near);
        if (value.error[0])
            return luaL_error(state, "%s", value.error);
        lua_pushnumber(state, value.x);
        return 1;
    }
    static int set_name(lua_State* state) {
        const auto value = set_entity_name(state);
        return value.error[0] ? luaL_error(state, "%s", value.error) : 0;
    }
    static int log_info(lua_State* state) {
        const auto value = write_log(state);
        return value.error[0] ? luaL_error(state, "%s", value.error) : 0;
    }
    static int tick(lua_State* state) {
        const auto value = read_time(state);
        if (value.error[0])
            return luaL_error(state, "%s", value.error);
        lua_pushinteger(state, value.integer);
        return 1;
    }
    static int delta(lua_State* state) {
        const auto value = read_time(state);
        if (value.error[0])
            return luaL_error(state, "%s", value.error);
        lua_pushnumber(state, value.x);
        return 1;
    }
    static int clamp(lua_State* state) {
        BindingResult value;
        if (!valid_context(current(state), value))
            return luaL_error(state, "%s", value.error);
        if (lua_gettop(state) != 3 || lua_type(state, 1) != LUA_TNUMBER ||
            lua_type(state, 2) != LUA_TNUMBER || lua_type(state, 3) != LUA_TNUMBER)
            return luaL_error(state, "clamp expects three numbers");
        const double number = lua_tonumber(state, 1);
        const double minimum = lua_tonumber(state, 2);
        const double maximum = lua_tonumber(state, 3);
        if (!std::isfinite(number) || !std::isfinite(minimum) || !std::isfinite(maximum) ||
            minimum > maximum)
            return luaL_error(state, "clamp requires finite values and an ordered range");
        lua_pushnumber(state, number < minimum ? minimum : number > maximum ? maximum : number);
        return 1;
    }

    // Called only under lua_pcall. All frames between Lua and the host are
    // trivial; any Lua allocation failure unwinds only native Lua/C frames.
    static int initialize_bindings(lua_State* state) {
        lua_newtable(state);
        lua_pushinteger(state, 1);
        lua_setfield(state, -2, "api_version");
        lua_newtable(state);
        lua_pushcfunction(state, &Impl::name);
        lua_setfield(state, -2, "name");
        lua_pushcfunction(state, &Impl::position);
        lua_setfield(state, -2, "position");
        lua_pushcfunction(state, &Impl::camera_near);
        lua_setfield(state, -2, "camera_near");
        lua_pushcfunction(state, &Impl::set_name);
        lua_setfield(state, -2, "set_name");
        lua_setfield(state, -2, "entity");
        lua_newtable(state);
        lua_pushcfunction(state, &Impl::tick);
        lua_setfield(state, -2, "tick");
        lua_pushcfunction(state, &Impl::delta);
        lua_setfield(state, -2, "delta");
        lua_setfield(state, -2, "time");
        lua_newtable(state);
        lua_pushcfunction(state, &Impl::clamp);
        lua_setfield(state, -2, "clamp");
        lua_setfield(state, -2, "math");
        lua_newtable(state);
        lua_pushcfunction(state, &Impl::log_info);
        lua_setfield(state, -2, "info");
        lua_setfield(state, -2, "log");
        lua_setglobal(state, "shark");
        return 0;
    }

    static int invoke_global(lua_State* state) {
        auto* self = current(state);
        lua_getglobal(state, self->callback_name);
        if (lua_isnil(state, -1)) {
            lua_pop(state, 1);
            return 0;
        }
        if (!lua_isfunction(state, -1))
            return luaL_error(state, "Lifecycle callback must be a function or nil");
        lua_call(state, 0, 0);
        return 0;
    }

    Result<void> ensure_bindings() {
        if (bindings_ready)
            return {};
        const int baseline = lua_gettop(state);
        lua_pushcfunction(state, &Impl::initialize_bindings);
        const int status = lua_pcall(state, 0, 0, 0);
        lua_settop(state, baseline);
        if (status != LUA_OK)
            return failure(ErrorCode::execution_failed,
                           "Cannot initialize Lua API (allocation failed or limit exceeded)",
                           "lua.bind");
        bindings_ready = true;
        return {};
    }

    Result<void> call(LuaCallContext* context, int baseline, const char* operation,
                      std::string_view path = {}) {
        lua_pushcfunction(state, &Impl::traceback);
        lua_insert(state, -2);
        const int handler = lua_gettop(state) - 1;
        instructions = 0;
        budget_exhausted = false;
        active = context;
        lua_sethook(state, &Impl::instruction_hook, LUA_MASKCOUNT, 1);
        const int status = lua_pcall(state, 0, 0, handler);
        lua_sethook(state, nullptr, 0, 0);
        active = nullptr;
        callback_name = nullptr;
        if (status != LUA_OK) {
            std::string message;
            if (budget_exhausted) {
                message = (status == LUA_ERRMEM || status == LUA_ERRERR)
                              ? "Lua instruction limit exceeded"
                              : diagnostic(state, -1);
            } else if (status == LUA_ERRMEM) {
                message = "Lua memory allocation failed or limit exceeded during execution";
            } else if (status == LUA_ERRERR) {
                message = "Lua error handler failed";
            } else {
                message = diagnostic(state, -1);
            }
            lua_settop(state, baseline);
            return failure(ErrorCode::execution_failed, std::move(message), operation, path);
        }
        lua_settop(state, baseline);
        return {};
    }
    // Storing the physical size means a failed shrink can safely retain its
    // allocation without making the budget undercount retained memory.
    struct alignas(std::max_align_t) Block {
        std::size_t size;
    };

    explicit Impl(LuaVmConfig config_value)
        : config(config_value), owner(std::this_thread::get_id()) {}

    ~Impl() {
        if (state != nullptr) {
            if (owner != std::this_thread::get_id()) {
                std::terminate();
            }
            lua_sethook(state, nullptr, 0, 0);
            lua_close(state);
        }
    }

    static void* allocate(void* user, void* pointer, std::size_t /*old_size*/,
                          std::size_t new_size) noexcept {
        auto* self = static_cast<Impl*>(user);
        auto* old_block = pointer == nullptr ? nullptr : static_cast<Block*>(pointer) - 1;
        const std::size_t previous = old_block == nullptr ? 0 : old_block->size;
        if (new_size == 0) {
            if (old_block != nullptr) {
                self->used -= previous;
                std::free(old_block);
            }
            return nullptr;
        }
        if (new_size > std::numeric_limits<std::size_t>::max() - sizeof(Block)) {
            return nullptr;
        }
        const std::size_t requested = new_size + sizeof(Block);
        if (requested > previous &&
            requested - previous > self->config.memory_limit_bytes - self->used) {
            return nullptr;
        }
        auto* block = static_cast<Block*>(std::realloc(old_block, requested));
        if (block == nullptr) {
            // Lua requires shrink operations to succeed. Retaining the old
            // physical size keeps both that guarantee and honest accounting.
            return requested <= previous ? pointer : nullptr;
        }
        self->used = self->used - previous + requested;
        block->size = requested;
        return block + 1;
    }

    static int initialize(lua_State* state) {
        luaL_requiref(state, LUA_GNAME, luaopen_base, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
        lua_pop(state, 1);

        // No protected calls/coroutines can catch the host's budget error; no
        // metatable creation can create a script finalizer during lua_close.
        // File loaders, collection controls, printing and dynamic source
        // loading also await explicit engine-owned policies.
        const char* removed[] = {"pcall",          "xpcall", "setmetatable", "getmetatable",
                                 "collectgarbage", "dofile", "loadfile",     "load",
                                 "print",          "warn"};
        for (const char* name : removed) {
            lua_pushnil(state);
            lua_setglobal(state, name);
        }
        lua_getglobal(state, LUA_STRLIBNAME);
        lua_pushnil(state);
        lua_setfield(state, -2, "dump");
        lua_pop(state, 1);
        lua_getglobal(state, LUA_MATHLIBNAME);
        lua_pushnil(state);
        lua_setfield(state, -2, "random");
        lua_pushnil(state);
        lua_setfield(state, -2, "randomseed");
        lua_pop(state, 1);
        return 0;
    }

    static int traceback(lua_State* state) {
        const char* message = lua_type(state, 1) == LUA_TSTRING ? lua_tostring(state, 1)
                                                                : "Lua raised a non-string error";
        luaL_traceback(state, state, message, 1);
        return 1;
    }

    static void instruction_hook(lua_State* state, lua_Debug* /*event*/) {
        void* userdata = nullptr;
        static_cast<void>(lua_getallocf(state, &userdata));
        auto* self = static_cast<Impl*>(userdata);
        ++self->instructions;
        if (self->instructions >= self->config.instruction_limit) {
            // Disable before raising so the protected error handler can build
            // its traceback; exposed Lua cannot catch and resume this error.
            lua_sethook(state, nullptr, 0, 0);
            self->budget_exhausted = true;
            static_cast<void>(luaL_error(state, "Lua instruction limit exceeded"));
        }
    }

    LuaVmConfig config;
    std::thread::id owner;
    lua_State* state{nullptr};
    std::size_t used{0};
    std::size_t instructions{0};
    bool budget_exhausted{false};
    bool bindings_ready{false};
    LuaCallContext* active{nullptr};
    const char* callback_name{nullptr};
};

LuaVm::LuaVm(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LuaVm::~LuaVm() = default;

Result<std::unique_ptr<LuaVm>> LuaVm::create(LuaVmConfig config) {
    if (config.memory_limit_bytes < min_memory || config.memory_limit_bytes > max_memory ||
        config.instruction_limit == 0 || config.instruction_limit > max_instructions) {
        return failure(ErrorCode::invalid_argument, "Invalid Lua VM resource limits", "lua.create");
    }
    auto impl = std::make_unique<Impl>(config);
    impl->state = lua_newstate(&Impl::allocate, impl.get(), 0);
    if (impl->state == nullptr) {
        return failure(ErrorCode::execution_failed, "Lua VM allocation failed", "lua.create");
    }
    // The initial Lua stack has room for this zero-upvalue function; library
    // creation and all its allocations happen inside the protected call.
    lua_pushcfunction(impl->state, &Impl::initialize);
    if (lua_pcall(impl->state, 0, 0, 0) != LUA_OK) {
        return failure(ErrorCode::execution_failed, "Lua VM library initialization failed",
                       "lua.create");
    }
    return std::unique_ptr<LuaVm>(new LuaVm(std::move(impl)));
}

Result<void> LuaVm::execute(std::string_view source, std::string_view chunk_name) {
    return execute_impl(source, chunk_name, nullptr);
}

Result<void> LuaVm::execute(std::string_view source, std::string_view chunk_name,
                            LuaCallContext& context) {
    return execute_impl(source, chunk_name, &context);
}

namespace {
bool valid_call_context(const LuaCallContext& context) noexcept {
    return context.scene != nullptr &&
           (context.profile == LuaProfile::server_gameplay ||
            context.profile == LuaProfile::client_presentation) &&
           (context.phase == LuaPhase::load || context.phase == LuaPhase::init ||
            context.phase == LuaPhase::update || context.phase == LuaPhase::fixed_update ||
            context.phase == LuaPhase::destroy) &&
           context.tick <= static_cast<std::uint64_t>(std::numeric_limits<lua_Integer>::max()) &&
           std::isfinite(context.delta_seconds) && context.delta_seconds >= 0 &&
           context.delta_seconds <= 1;
}
} // namespace

Result<void> LuaVm::execute_impl(std::string_view source, std::string_view chunk_name,
                                 LuaCallContext* context) {
    if (impl_->owner != std::this_thread::get_id()) {
        return failure(ErrorCode::conflict, "Lua VM called from a different thread", "lua.execute");
    }
    if (impl_->active != nullptr)
        return failure(ErrorCode::conflict, "Lua VM cannot be entered recursively", "lua.execute");
    if (context != nullptr && (!valid_call_context(*context) || context->phase != LuaPhase::load))
        return failure(ErrorCode::invalid_argument, "Invalid Lua load context", "lua.execute");
    if (source.size() > max_source_bytes || source.find('\0') != std::string_view::npos ||
        !valid_utf8(source) || chunk_name.empty() || chunk_name.size() > max_chunk_name_bytes ||
        !valid_utf8(chunk_name, true)) {
        return failure(ErrorCode::invalid_argument, "Invalid Lua source or chunk name",
                       "lua.execute");
    }
    if (context != nullptr) {
        if (auto bound = impl_->ensure_bindings(); !bound)
            return bound;
    }
    lua_State* state = impl_->state;
    const int baseline = lua_gettop(state);
    const std::string name = "@" + std::string(chunk_name);
    const int load_status =
        luaL_loadbufferx(state, source.data(), source.size(), name.c_str(), "t");
    if (load_status != LUA_OK) {
        std::string message = load_status == LUA_ERRMEM
                                  ? "Lua memory allocation failed or limit exceeded while loading"
                                  : diagnostic(state, -1);
        lua_settop(state, baseline);
        return failure(ErrorCode::execution_failed, std::move(message), "lua.execute", chunk_name);
    }
    return impl_->call(context, baseline, "lua.execute", chunk_name);
}

Result<void> LuaVm::invoke(std::string_view callback, LuaCallContext& context) {
    if (impl_->owner != std::this_thread::get_id())
        return failure(ErrorCode::conflict, "Lua VM called from a different thread", "lua.invoke");
    if (impl_->active != nullptr)
        return failure(ErrorCode::conflict, "Lua VM cannot be entered recursively", "lua.invoke");
    if (!valid_call_context(context))
        return failure(ErrorCode::invalid_argument, "Invalid Lua callback context", "lua.invoke");
    const char* name = nullptr;
    if (callback == "on_init" && context.phase == LuaPhase::init)
        name = "on_init";
    else if (callback == "on_update" && context.phase == LuaPhase::update)
        name = "on_update";
    else if (callback == "on_fixed_update" && context.phase == LuaPhase::fixed_update)
        name = "on_fixed_update";
    else if (callback == "on_destroy" && context.phase == LuaPhase::destroy)
        name = "on_destroy";
    else
        return failure(ErrorCode::invalid_argument, "Unknown Lua lifecycle callback or phase",
                       "lua.invoke");
    if (auto bound = impl_->ensure_bindings(); !bound)
        return bound;
    const int baseline = lua_gettop(impl_->state);
    impl_->callback_name = name;
    lua_pushcfunction(impl_->state, &Impl::invoke_global);
    return impl_->call(&context, baseline, "lua.invoke");
}

std::size_t LuaVm::memory_used() const noexcept {
    if (impl_->owner != std::this_thread::get_id()) {
        std::terminate();
    }
    return impl_->used;
}

} // namespace shark::detail
