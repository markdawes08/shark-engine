#include "lua_vm.hpp"

#include "utf8.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace shark::detail {
namespace {

constexpr std::size_t min_memory = 64 * 1024;
constexpr std::size_t max_memory = 64 * 1024 * 1024;
constexpr std::size_t max_instructions = 10'000'000;
constexpr std::size_t max_source_bytes = 256 * 1024;
constexpr std::size_t max_chunk_name_bytes = 255;
constexpr std::size_t max_diagnostic_bytes = 768;

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
    if (impl_->owner != std::this_thread::get_id()) {
        return failure(ErrorCode::conflict, "Lua VM called from a different thread", "lua.execute");
    }
    if (source.size() > max_source_bytes || source.find('\0') != std::string_view::npos ||
        !valid_utf8(source) || chunk_name.empty() || chunk_name.size() > max_chunk_name_bytes ||
        !valid_utf8(chunk_name, true)) {
        return failure(ErrorCode::invalid_argument, "Invalid Lua source or chunk name",
                       "lua.execute");
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

    lua_pushcfunction(state, &Impl::traceback);
    lua_insert(state, -2);
    const int handler = lua_gettop(state) - 1;
    impl_->instructions = 0;
    impl_->budget_exhausted = false;
    lua_sethook(state, &Impl::instruction_hook, LUA_MASKCOUNT, 1);
    const int status = lua_pcall(state, 0, 0, handler);
    lua_sethook(state, nullptr, 0, 0);
    if (status != LUA_OK) {
        std::string message;
        if (impl_->budget_exhausted) {
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
        return failure(ErrorCode::execution_failed, std::move(message), "lua.execute", chunk_name);
    }
    lua_settop(state, baseline);
    return {};
}

std::size_t LuaVm::memory_used() const noexcept {
    if (impl_->owner != std::this_thread::get_id()) {
        std::terminate();
    }
    return impl_->used;
}

} // namespace shark::detail
