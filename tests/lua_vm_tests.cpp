#include "lua_vm.hpp"
#include "physics_contact_scope.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {
using shark::ErrorCode;
using shark::Result;
using shark::detail::LuaVm;
using shark::detail::LuaVmConfig;

std::unique_ptr<LuaVm> create_vm(LuaVmConfig config = {}) {
    auto result = LuaVm::create(config);
    INFO((result ? std::string{} : result.error().message));
    REQUIRE(result);
    return std::move(result).value();
}

void succeeds(LuaVm& vm, const std::string& source) {
    const auto result = vm.execute(source, "test.lua");
    INFO((result ? std::string{} : result.error().message));
    REQUIRE(result);
}

void fails(const Result<void>& result, ErrorCode code) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().message.empty());
    CHECK(result.error().context.operation == "lua.execute");
}
} // namespace

TEST_CASE("Lua VM exposes bounded standard helpers and isolates VM state", "[core-lua]") {
    auto first = create_vm();
    auto second = create_vm();
    succeeds(*first, R"lua(
        assert(math.floor(2.9) == 2)
        assert(string.upper('shark') == 'SHARK')
        local a = {3, 1, 2}
        table.sort(a)
        assert(table.concat(a, ',') == '1,2,3')
        assert(utf8.len('abc') == 3)
        isolated = 17
    )lua");
    succeeds(*second, "assert(isolated == nil); isolated = 23");
    succeeds(*first, "assert(isolated == 17)");
    CHECK(first->memory_used() > 0);
    CHECK(first->memory_used() <= LuaVmConfig{}.memory_limit_bytes);
}

TEST_CASE("Lua VM excludes host access and budget escape helpers", "[core-lua]") {
    auto vm = create_vm();
    succeeds(*vm, R"lua(
        assert(io == nil and os == nil and package == nil and debug == nil)
        assert(require == nil and dofile == nil and loadfile == nil and load == nil)
        assert(coroutine == nil and pcall == nil and xpcall == nil)
        assert(collectgarbage == nil and string.dump == nil)
        assert(math.random == nil and math.randomseed == nil)
        -- No unbounded Lua finalizer can be installed and run during teardown.
        assert(setmetatable == nil and getmetatable == nil)
    )lua");
}

TEST_CASE("Lua VM structural mutations require a protected host callback", "[core-lua]") {
    auto vm = create_vm();
    succeeds(*vm, "assert(shark == nil)");
    const auto identity = shark::StableId::parse("21c00000-0000-4000-8000-000000000001");
    REQUIRE(identity);
    auto created_scene =
        shark::Scene::create(shark::AuthoredSceneScope{shark::SceneId{identity.value()}});
    REQUIRE(created_scene);
    auto scene = std::move(created_scene).value();
    auto created_entity = scene->create_entity({shark::AuthoredEntityId{identity.value()}});
    REQUIRE(created_entity);
    auto reference = scene->reference(created_entity.value());
    REQUIRE(reference);
    // A contextual VM has bindings, but no runtime mutation host was supplied.
    shark::detail::LuaCallContext context{scene.get(),
                                          reference.value(),
                                          shark::detail::LuaPhase::load,
                                          shark::detail::LuaProfile::client_presentation,
                                          0,
                                          0.01,
                                          nullptr,
                                          nullptr};
    const auto exposed = vm->execute("assert(type(shark.prefab.spawn)=='function'); "
                                     "assert(type(shark.entity.destroy)=='function'); "
                                     "function on_update() shark.prefab.spawn(1,"
                                     "'21c00000-0000-4000-8000-000000000014',0,0,0) end; "
                                     "function on_fixed_update() shark.entity.destroy(1) end",
                                     "bindings.lua", context);
    INFO((exposed ? std::string{} : exposed.error().message));
    REQUIRE(exposed);
    context.phase = shark::detail::LuaPhase::update;
    const auto spawn = vm->invoke("on_update", context);
    REQUIRE_FALSE(spawn);
    CHECK(spawn.error().code == ErrorCode::execution_failed);
    CHECK(spawn.error().context.operation == "lua.invoke");
    context.phase = shark::detail::LuaPhase::fixed_update;
    const auto destroy = vm->invoke("on_fixed_update", context);
    REQUIRE_FALSE(destroy);
    CHECK(destroy.error().code == ErrorCode::execution_failed);
    CHECK(destroy.error().context.operation == "lua.invoke");
}

TEST_CASE("Lua VM rejects creation and source execution inside a native contact callback",
          "[core-lua]") {
    auto vm = create_vm();
    CHECK_FALSE(shark::detail::NativeContactScope::active());
    {
        const shark::detail::NativeContactScope contact;
        CHECK(shark::detail::NativeContactScope::active());
        const auto created = LuaVm::create({});
        REQUIRE_FALSE(created);
        CHECK(created.error().code == ErrorCode::conflict);
        const auto loaded = vm->execute("native_callback_ran = true", "contact.lua");
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().code == ErrorCode::conflict);
        {
            const shark::detail::NativeContactScope nested;
            CHECK(shark::detail::NativeContactScope::active());
        }
        CHECK(shark::detail::NativeContactScope::active());
    }
    CHECK_FALSE(shark::detail::NativeContactScope::active());
    succeeds(*vm, "assert(native_callback_ran == nil)");
}

TEST_CASE("Lua VM rejects invalid bounds and invalid input", "[core-lua]") {
    LuaVmConfig config;
    config.memory_limit_bytes = 65535;
    const auto low_memory = LuaVm::create(config);
    REQUIRE_FALSE(low_memory);
    CHECK(low_memory.error().code == ErrorCode::invalid_argument);
    config.memory_limit_bytes = 64U * 1024U * 1024U + 1U;
    REQUIRE_FALSE(LuaVm::create(config));
    config.memory_limit_bytes = 4U * 1024U * 1024U;
    config.instruction_limit = 0;
    REQUIRE_FALSE(LuaVm::create(config));
    config.instruction_limit = 10000001;
    REQUIRE_FALSE(LuaVm::create(config));

    auto vm = create_vm();
    fails(vm->execute(std::string(256U * 1024U + 1U, ' '), "test.lua"),
          ErrorCode::invalid_argument);
    fails(vm->execute(std::string("a\0b", 3), "test.lua"), ErrorCode::invalid_argument);
    fails(vm->execute(std::string(1, static_cast<char>(0xff)), "test.lua"),
          ErrorCode::invalid_argument);
    fails(vm->execute("return 1", std::string(4097, 'a')), ErrorCode::invalid_argument);
    fails(vm->execute("return 1", std::string("a\0b", 3)), ErrorCode::invalid_argument);
    fails(vm->execute("return 1", std::string(1, static_cast<char>(0xff))),
          ErrorCode::invalid_argument);
    fails(vm->execute("return 1", ""), ErrorCode::invalid_argument);
    // The bytecode signature is valid UTF-8 but must never be loaded as text.
    fails(vm->execute(std::string("\x1bLua", 4), "binary.lua"), ErrorCode::execution_failed);
    succeeds(*vm, "assert(1 + 1 == 2)");
    REQUIRE(vm->execute("assert(true)", std::string(4096, 'a')));
}

TEST_CASE("Lua syntax and runtime failures have diagnostics and do not poison VM", "[core-lua]") {
    auto vm = create_vm();
    const auto syntax = vm->execute("local =", "syntax.lua");
    fails(syntax, ErrorCode::execution_failed);
    CHECK(syntax.error().context.path == "syntax.lua");
    const auto runtime =
        vm->execute("local function inner() error('shark failure') end\ninner()", "runtime.lua");
    fails(runtime, ErrorCode::execution_failed);
    CHECK(runtime.error().context.path == "runtime.lua");
    CHECK(runtime.error().message.find("shark failure") != std::string::npos);
    CHECK(runtime.error().message.find("stack traceback") != std::string::npos);
    fails(vm->execute("changed_before_error = 42; error('failed')", "partial.lua"),
          ErrorCode::execution_failed);
    // VM recovery does not imply rollback of prior Lua assignments.
    succeeds(*vm, "assert(changed_before_error == 42)");
    succeeds(*vm, "assert(math.abs(-4) == 4)");
}

TEST_CASE("Lua error diagnostics are bounded and handle arbitrary error values", "[core-lua]") {
    auto vm = create_vm();
    const auto long_error = vm->execute("error(string.rep('z', 2000))", "long.lua");
    fails(long_error, ErrorCode::execution_failed);
    CHECK(long_error.error().message.size() <= 768);
    const auto bytes = vm->execute("error(string.char(255, 254, 0))", "bytes.lua");
    fails(bytes, ErrorCode::execution_failed);
    for (const char character : bytes.error().message) {
        const auto byte = static_cast<unsigned char>(character);
        CHECK(((byte >= 32U && byte < 127U) || byte == '\n' || byte == '\t'));
    }
    const auto non_string = vm->execute("error({})", "nonstring.lua");
    fails(non_string, ErrorCode::execution_failed);
    succeeds(*vm, "assert(1 == 1)");
}

TEST_CASE("Lua instruction exhaustion interrupts runaway code and recovers", "[core-lua]") {
    LuaVmConfig config;
    config.instruction_limit = 10000;
    auto vm = create_vm(config);
    const auto runaway = vm->execute("while true do end", "runaway.lua");
    fails(runaway, ErrorCode::execution_failed);
    CHECK(runaway.error().message.find("instruction") != std::string::npos);
    succeeds(*vm, "assert(2 * 3 == 6)");
}

TEST_CASE("Lua allocator enforces its budget and remains usable after exhaustion", "[core-lua]") {
    LuaVmConfig config;
    config.memory_limit_bytes = 128U * 1024U;
    config.instruction_limit = 10000000;
    auto vm = create_vm(config);
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto exhausted = vm->execute(
            "local t = {}; for i = 1, 100000 do t[i] = string.rep('x', 1024) end", "memory.lua");
        fails(exhausted, ErrorCode::execution_failed);
        CHECK(exhausted.error().message.find("memory") != std::string::npos);
        CHECK(vm->memory_used() <= config.memory_limit_bytes);
        succeeds(*vm, "assert(7 == 7)");
        CHECK(vm->memory_used() <= config.memory_limit_bytes);
    }
}

TEST_CASE("Lua VM rejects execution from a foreign thread", "[core-lua]") {
    auto vm = create_vm();
    std::optional<Result<void>> foreign;
    std::thread worker([&] { foreign.emplace(vm->execute("foreign = true", "foreign.lua")); });
    worker.join();
    REQUIRE(foreign.has_value());
    REQUIRE_FALSE(*foreign);
    CHECK_FALSE(foreign->error().message.empty());
    succeeds(*vm, "assert(foreign == nil)");
}
