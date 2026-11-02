#include <shark/physics.hpp>
#include <shark/scene.hpp>
#include <shark/script_runtime.hpp>
#include <shark/time.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace shark;
using namespace std::chrono_literals;

template <typename T> T take(Result<T> result) {
    if (!result)
        INFO(result.error().message);
    REQUIRE(result);
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result)
        INFO(result.error().message);
    REQUIRE(result);
}
template <typename Id> Id id(unsigned value) {
    std::string text = "19c00000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 15U];
        value >>= 4U;
    }
    return Id{take(StableId::parse(text))};
}
std::unique_ptr<Scene> region_scene() {
    return take(Scene::create(
        RegionSceneScope{id<WorldId>(1), {id<RegionId>(2), id<RegionInstanceId>(3), 1}}));
}
std::unique_ptr<Scene> authored_scene() {
    return take(Scene::create(AuthoredSceneScope{id<SceneId>(1)}));
}
EntityHandle add_region(Scene& scene, unsigned number, std::string name,
                        std::string path = "scripts/main.lua", bool enabled = true,
                        bool camera = false) {
    EntitySeed seed{id<WorldEntityId>(number)};
    seed.name = Name{std::move(name)};
    seed.transform.translation = {4, 5, 6};
    seed.script = Script{std::move(path), enabled};
    if (camera)
        seed.camera = Camera{};
    return take(scene.create_entity(std::move(seed)));
}
EntityHandle add_authored(Scene& scene, unsigned number, std::string path = "scripts/main.lua") {
    EntitySeed seed{id<AuthoredEntityId>(number)};
    seed.script = Script{std::move(path), true};
    return take(scene.create_entity(std::move(seed)));
}
ScriptRuntimeConfig server_config() {
    ScriptRuntimeConfig config;
    config.profile = ScriptProfile::server_gameplay;
    return config;
}
std::unique_ptr<ScriptRuntime> make_runtime(Scene& scene, std::string source,
                                            ScriptRuntimeConfig config = server_config()) {
    const std::array snapshots{ScriptSourceSnapshot{"scripts/main.lua", std::move(source)}};
    return take(ScriptRuntime::create(scene, snapshots, config));
}
std::vector<std::string> messages(const std::vector<ScriptLogEntry>& logs) {
    std::vector<std::string> result;
    for (const auto& log : logs)
        result.push_back(log.message);
    return result;
}
} // namespace

TEST_CASE("Lua owner coexists with physics without bypassing its Scene writer lease",
          "[script-runtime]") {
    auto scene = region_scene();
    EntitySeed seed{id<WorldEntityId>(1)};
    seed.name = Name{"Before"};
    seed.script = Script{"scripts/main.lua", true};
    seed.rigid_body = RigidBody{};
    const auto entity = take(scene->create_entity(std::move(seed)));
    auto physics = take(PhysicsWorld::create());
    take(physics->create_body(*scene, entity));
    take(physics->begin_simulation(*scene));

    auto runtime =
        make_runtime(*scene, "function on_fixed_update() shark.entity.set_name('Denied') end");
    take(runtime->start(*scene));
    CHECK_FALSE(runtime->fixed_update(*scene, 0, 0.05));
    CHECK(take(runtime->stats()).faulted);
    CHECK(take(scene->entity_state(entity)).name == Name{"Before"});
    take(runtime->stop(*scene));
    CHECK(take(physics->stats()).simulating);
    take(physics->step(*scene));
    take(physics->end_simulation(*scene));
    take(physics->close());
}

TEST_CASE("Lua owner lease releases after stop, close, destruction and failed creation",
          "[script-runtime]") {
    auto scene = region_scene();
    add_region(*scene, 1, "Owner");
    const std::array source{ScriptSourceSnapshot{"scripts/main.lua", ""}};
    const std::array invalid{ScriptSourceSnapshot{"scripts/main.lua", "function broken("}};

    // Even a failure after admission and VM creation must release the Scene lease.
    CHECK_FALSE(ScriptRuntime::create(*scene, invalid, server_config()));
    auto first = take(ScriptRuntime::create(*scene, source, server_config()));
    CHECK_FALSE(ScriptRuntime::create(*scene, source, server_config()));
    auto foreign = region_scene();
    CHECK_FALSE(first->start(*foreign));
    CHECK_FALSE(ScriptRuntime::create(*scene, source, server_config()));
    take(first->start(*scene));
    take(first->stop(*scene));

    auto second = take(ScriptRuntime::create(*scene, source, server_config()));
    take(second->close());
    auto third = take(ScriptRuntime::create(*scene, source, server_config()));
    third.reset();
    auto fourth = take(ScriptRuntime::create(*scene, source, server_config()));
    take(fourth->close());
}

TEST_CASE("Lua stop waits for pending Scene mutations before teardown", "[script-runtime]") {
    auto scene = region_scene();
    const auto entity = add_region(*scene, 1, "Before");
    const std::array source{ScriptSourceSnapshot{
        "scripts/main.lua", "function on_destroy() shark.log.info(shark.entity.name()) end"}};
    auto runtime = take(ScriptRuntime::create(*scene, source, server_config()));
    take(runtime->start(*scene));
    take(scene->defer_set_name(entity, Name{"After"}));
    CHECK_FALSE(runtime->stop(*scene));
    CHECK(take(runtime->take_logs()).empty());
    CHECK_FALSE(ScriptRuntime::create(*scene, source, server_config()));
    take(scene->apply_deferred());
    const auto stopped = take(runtime->stop(*scene));
    CHECK(stopped.attempted == 1);
    CHECK(stopped.discarded == 0);
    CHECK(stopped.errors.empty());
    CHECK(messages(take(runtime->take_logs())) == std::vector<std::string>{"After"});
    auto next = take(ScriptRuntime::create(*scene, source, server_config()));
    take(next->close());
}

TEST_CASE("Lua runtime accepts long valid source paths and preserves error context",
          "[script-runtime]") {
    auto scene = region_scene();
    const std::string path =
        "scripts/" + std::string(200, 'a') + "/" + std::string(100, 'b') + ".lua";
    add_region(*scene, 1, "Long path", path);
    const std::array source{ScriptSourceSnapshot{path, "function on_init() error('expected') end"}};
    auto runtime = take(ScriptRuntime::create(*scene, source, server_config()));
    const auto started = runtime->start(*scene);
    REQUIRE_FALSE(started);
    CHECK(started.error().context.path == path);
    take(runtime->stop(*scene));
}

TEST_CASE("Lua scripts follow deterministic lifecycle and an existing server scheduler",
          "[script-runtime]") {
    auto scene = region_scene();
    // Creation order intentionally differs from stable identity order.
    const auto second = add_region(*scene, 20, "Second", "scripts/main.lua", true, true);
    const auto first = add_region(*scene, 10, "First", "scripts/main.lua", true, true);
    auto runtime = make_runtime(*scene, R"lua(
        function on_init()
            assert(shark.api_version == 1)
            shark.api_version = 99 -- This VM's copy cannot modify another VM.
            local x, y, z = shark.entity.position()
            assert(x == 4 and y == 5 and z == 6)
            assert(math.abs(shark.entity.camera_near() - 0.1) < 1e-12)
            assert(shark.math.clamp(4, 0, 3) == 3)
            assert(shark.math.clamp(-2, 0, 3) == 0)
            assert(shark.math.clamp(2, 0, 3) == 2)
            shark.log.info('init:' .. shark.entity.name())
        end
        function on_update()
            assert(shark.api_version == 99)
            shark.log.info('update:' .. shark.entity.name())
        end
        function on_fixed_update()
            assert(shark.time.tick() == 0)
            assert(math.abs(shark.time.delta() - 0.05) < 1e-12)
            shark.entity.set_name(shark.entity.name() .. '!')
            shark.log.info('fixed:' .. shark.entity.name())
        end
        function on_destroy()
            shark.log.info('destroy:' .. shark.entity.name())
        end
    )lua");
    take(runtime->start(*scene));
    take(runtime->update(*scene, 0.01));
    auto scheduler = take(ServerScheduler::create({}, 0ns));
    const TickCallback callback = [&](std::uint64_t tick) {
        return runtime->fixed_update(*scene, tick, 0.05);
    };
    const auto stepped = take(scheduler->poll(50ms, callback));
    CHECK(stepped.executed == 1);
    CHECK(take(scene->entity_state(first)).name == Name{"First!"});
    CHECK(take(scene->entity_state(second)).name == Name{"Second!"});
    const auto stopped = take(runtime->stop(*scene));
    CHECK(stopped.attempted == 2);
    CHECK(stopped.errors.empty());
    const auto logs = take(runtime->take_logs());
    CHECK(messages(logs) == std::vector<std::string>{"init:First", "init:Second", "update:First",
                                                     "update:Second", "fixed:First!",
                                                     "fixed:Second!", "destroy:Second!",
                                                     "destroy:First!"});
    REQUIRE(logs.size() == 8);
    CHECK(logs[0].phase == ScriptPhase::init);
    CHECK(logs[4].phase == ScriptPhase::fixed_update);
    CHECK(logs[4].tick == 0);
    CHECK(logs[6].phase == ScriptPhase::destroy);
    CHECK(logs[0].entity == take(scene->reference(first)));
    CHECK(logs[1].entity == take(scene->reference(second)));
    CHECK(take(runtime->take_logs()).empty());
}

TEST_CASE("Lua name writes require a real authoritative region and fixed-update phase",
          "[script-runtime]") {
    ScriptRuntimeConfig client;
    client.profile = ScriptProfile::client_presentation;
    auto region = region_scene();
    add_region(*region, 9, "Region");
    const std::array empty_source{ScriptSourceSnapshot{"scripts/main.lua", ""}};
    CHECK_FALSE(ScriptRuntime::create(*region, empty_source, client));
    auto client_scene = authored_scene();
    const auto client_entity = add_authored(*client_scene, 10);
    take(client_scene->set_name(client_entity, Name{"Client"}));
    auto client_runtime = make_runtime(
        *client_scene, "function on_fixed_update() shark.entity.set_name('Denied') end", client);
    take(client_runtime->start(*client_scene));
    CHECK_FALSE(client_runtime->fixed_update(*client_scene, 0, 0.05));
    CHECK(take(client_scene->entity_state(client_entity)).name == Name{"Client"});
    CHECK(take(client_runtime->stats()).faulted);
    take(client_runtime->stop(*client_scene));

    auto server_scene = region_scene();
    const auto server_entity = add_region(*server_scene, 11, "Server");
    auto wrong_phase =
        make_runtime(*server_scene, "function on_update() shark.entity.set_name('Denied') end");
    take(wrong_phase->start(*server_scene));
    CHECK_FALSE(wrong_phase->update(*server_scene, 0.05));
    CHECK(take(server_scene->entity_state(server_entity)).name == Name{"Server"});
    take(wrong_phase->stop(*server_scene));

    auto authored = authored_scene();
    add_authored(*authored, 12);
    const std::array source{ScriptSourceSnapshot{"scripts/main.lua", ""}};
    CHECK_FALSE(ScriptRuntime::create(*authored, source, server_config()));
}

TEST_CASE("Lua checked components and math reject invalid values without mutating the scene",
          "[script-runtime]") {
    const std::array sources{"function on_init() shark.entity.camera_near() end",
                             "function on_init() shark.math.clamp(1, 2, 0) end",
                             "function on_init() shark.math.clamp('1', 0, 2) end",
                             "function on_init() shark.math.clamp(0/0, 0, 2) end",
                             "function on_init() shark.math.clamp(1/0, 0, 2) end",
                             "function on_init() shark.log.info(42) end",
                             "function on_init() shark.entity.name('unexpected') end",
                             "function on_init() shark.entity.position('unexpected') end",
                             "function on_init() shark.entity.camera_near('unexpected') end",
                             "function on_init() shark.time.tick('unexpected') end",
                             "function on_init() shark.time.delta('unexpected') end"};
    for (const auto* source : sources) {
        INFO(source);
        auto scene = region_scene();
        const auto entity = add_region(*scene, 10, "Original");
        const auto before = take(scene->stats()).revision;
        auto runtime = make_runtime(*scene, source);
        CHECK_FALSE(runtime->start(*scene));
        CHECK(take(runtime->stats()).faulted);
        CHECK(take(scene->stats()).revision == before);
        CHECK(take(scene->entity_state(entity)).name == Name{"Original"});
        take(runtime->stop(*scene));
    }
}

TEST_CASE("Lua authoritative setters reject invalid arguments atomically", "[script-runtime]") {
    const std::array sources{
        "function on_fixed_update() shark.entity.set_name('') end",
        "function on_fixed_update() shark.entity.set_name(42) end",
        "function on_fixed_update() shark.entity.set_name('bad\\nname') end",
        "function on_fixed_update() shark.entity.set_name(string.rep('x',257)) end",
        "function on_fixed_update() shark.entity.set_name('ok','extra') end"};
    for (const auto* source : sources) {
        INFO(source);
        auto scene = region_scene();
        const auto entity = add_region(*scene, 10, "Original");
        auto runtime = make_runtime(*scene, source);
        take(runtime->start(*scene));
        const auto before = take(scene->stats()).revision;
        CHECK_FALSE(runtime->fixed_update(*scene, 0, 0.05));
        CHECK(take(scene->stats()).revision == before);
        CHECK(take(scene->entity_state(entity)).name == Name{"Original"});
        take(runtime->stop(*scene));
    }
}

TEST_CASE("Lua initialization failure preserves prior work and tears it down once",
          "[script-runtime]") {
    auto scene = region_scene();
    add_region(*scene, 10, "First");
    add_region(*scene, 20, "Second");
    auto runtime = make_runtime(*scene, R"lua(
        function on_init()
            shark.log.info('init:' .. shark.entity.name())
            if shark.entity.name() == 'Second' then error('init failure') end
        end
        function on_destroy() shark.log.info('destroy:' .. shark.entity.name()) end
    )lua");
    CHECK_FALSE(runtime->start(*scene));
    CHECK(take(runtime->stats()).faulted);
    const auto stopped = take(runtime->stop(*scene));
    CHECK(stopped.attempted == 1);
    CHECK(stopped.errors.empty());
    CHECK(messages(take(runtime->take_logs())) ==
          std::vector<std::string>{"init:First", "init:Second", "destroy:First"});
    CHECK(take(runtime->close()) == 0);
}

TEST_CASE("Lua callback failure preserves earlier writes and faults without retry",
          "[script-runtime]") {
    auto scene = region_scene();
    const auto first = add_region(*scene, 10, "First");
    const auto second = add_region(*scene, 20, "Second");
    auto runtime = make_runtime(*scene, R"lua(
        function on_fixed_update()
            local name = shark.entity.name()
            if name == 'Second' then error('second failed') end
            shark.entity.set_name(name .. '!')
        end
    )lua");
    take(runtime->start(*scene));
    CHECK_FALSE(runtime->fixed_update(*scene, 0, 0.05));
    CHECK(take(scene->entity_state(first)).name == Name{"First!"});
    CHECK(take(scene->entity_state(second)).name == Name{"Second"});
    CHECK(take(runtime->stats()).faulted);
    CHECK_FALSE(take(runtime->stats()).last_fixed_tick.has_value());
    CHECK_FALSE(runtime->fixed_update(*scene, 0, 0.05));
    CHECK(take(scene->entity_state(first)).name == Name{"First!"});
    CHECK(take(runtime->stop(*scene)).attempted == 2);
}

TEST_CASE("Lua destroy errors do not skip later reverse-order cleanup", "[script-runtime]") {
    auto scene = region_scene();
    add_region(*scene, 10, "First");
    add_region(*scene, 20, "Second");
    auto runtime = make_runtime(*scene, R"lua(
        function on_destroy()
            shark.log.info('destroy:' .. shark.entity.name())
            error('destroy failed')
        end
    )lua");
    take(runtime->start(*scene));
    const auto stopped = take(runtime->stop(*scene));
    CHECK(stopped.attempted == 2);
    CHECK(stopped.errors.size() == 2);
    CHECK(messages(take(runtime->take_logs())) ==
          std::vector<std::string>{"destroy:Second", "destroy:First"});
    CHECK(take(runtime->close()) == 0);
}

TEST_CASE("Lua teardown skips stale or changed Script owners and still cleans valid entries",
          "[script-runtime]") {
    for (const bool recreate : {false, true}) {
        INFO(recreate);
        auto scene = region_scene();
        const auto first = add_region(*scene, 10, "First");
        const auto second = add_region(*scene, 20, "Second");
        const auto second_reference = take(scene->reference(second));
        auto runtime =
            make_runtime(*scene, "function on_destroy() shark.log.info('destroy callback') end");
        take(runtime->start(*scene));
        if (recreate) {
            take(scene->destroy_entity(first));
            const auto replacement = add_region(*scene, 10, "Replacement");
            CHECK(replacement.generation != first.generation);
        } else {
            take(scene->set_script(first, Script{"scripts/other.lua", true}));
        }
        const auto stopped = take(runtime->stop(*scene));
        CHECK(stopped.attempted == 1);
        CHECK(stopped.discarded == 1);
        REQUIRE(stopped.errors.size() == 1);
        CHECK(stopped.errors[0].context.entity_id == id<WorldEntityId>(10).value().to_string());
        const auto logs = take(runtime->take_logs());
        REQUIRE(logs.size() == 1);
        CHECK(logs[0].entity == second_reference);
        CHECK(logs[0].message == "destroy callback");
    }
}

TEST_CASE("Lua stale references do not retarget a recreated entity", "[script-runtime]") {
    auto scene = region_scene();
    const auto old = add_region(*scene, 10, "Old");
    auto runtime =
        make_runtime(*scene, "function on_update() shark.log.info(shark.entity.name()) end");
    take(runtime->start(*scene));
    take(scene->destroy_entity(old));
    const auto replacement = add_region(*scene, 10, "Replacement");
    CHECK(replacement.generation != old.generation);
    CHECK_FALSE(runtime->update(*scene, 0.1));
    CHECK(take(scene->entity_state(replacement)).name == Name{"Replacement"});
    CHECK(take(runtime->take_logs()).empty());
    take(runtime->stop(*scene));
}

TEST_CASE("Lua runaway callbacks stop without retry and teardown remains explicit",
          "[script-runtime]") {
    auto scene = region_scene();
    const auto entity = add_region(*scene, 10, "Original");
    auto config = server_config();
    config.instruction_limit = 10000;
    auto runtime = make_runtime(*scene, R"lua(
        function on_fixed_update() while true do end end
        function on_destroy() shark.log.info('destroyed') end
    )lua",
                                config);
    take(runtime->start(*scene));
    const auto runaway = runtime->fixed_update(*scene, 0, 0.05);
    REQUIRE_FALSE(runaway);
    CHECK(runaway.error().message.find("instruction") != std::string::npos);
    CHECK(take(runtime->stats()).faulted);
    CHECK_FALSE(runtime->fixed_update(*scene, 0, 0.05));
    CHECK(take(scene->entity_state(entity)).name == Name{"Original"});
    const auto stopped = take(runtime->stop(*scene));
    CHECK(stopped.attempted == 1);
    CHECK(stopped.errors.empty());
    CHECK(messages(take(runtime->take_logs())) == std::vector<std::string>{"destroyed"});
    auto fresh = make_runtime(*scene, "function on_init() shark.log.info('recovered') end");
    take(fresh->start(*scene));
    CHECK(messages(take(fresh->take_logs())) == std::vector<std::string>{"recovered"});
    CHECK(take(fresh->close()) == 1);
}

TEST_CASE("Lua owner rejects foreign scenes and threads and closes without borrowing a Scene",
          "[script-runtime]") {
    auto scene = region_scene();
    add_region(*scene, 10, "Owned");
    auto runtime = make_runtime(*scene, "function on_destroy() shark.log.info('must-not-run') end");
    take(runtime->start(*scene));
    auto other = region_scene();
    add_region(*other, 10, "Other");
    CHECK_FALSE(runtime->update(*other, 0.1));
    bool foreign_rejected = false;
    std::thread thread([&] { foreign_rejected = !runtime->update(*scene, 0.1); });
    thread.join();
    CHECK(foreign_rejected);
    scene.reset();
    CHECK(take(runtime->close()) == 1);
    CHECK(take(runtime->take_logs()).empty());
}

TEST_CASE("Script runtime admits bounded snapshots and requires enabled source coverage",
          "[script-runtime]") {
    auto scene = region_scene();
    add_region(*scene, 10, "Enabled");
    const auto before = take(scene->stats()).revision;
    const std::array valid{ScriptSourceSnapshot{"scripts/main.lua", ""}};
    auto config = server_config();
    config.max_scripts = 0;
    CHECK_FALSE(ScriptRuntime::create(*scene, valid, config));
    config = server_config();
    config.max_scripts = script_runtime_maximum_scripts + 1;
    CHECK_FALSE(ScriptRuntime::create(*scene, valid, config));
    config = server_config();
    config.instruction_limit = 0;
    CHECK_FALSE(ScriptRuntime::create(*scene, valid, config));
    config = server_config();
    config.memory_bytes_per_script = 65535;
    CHECK_FALSE(ScriptRuntime::create(*scene, valid, config));
    config = server_config();
    config.max_scripts = 2;
    config.memory_bytes_per_script = script_runtime_maximum_vm_bytes;
    CHECK_FALSE(ScriptRuntime::create(*scene, valid, config));
    config = server_config();
    const std::array duplicate{valid[0], valid[0]};
    CHECK_FALSE(ScriptRuntime::create(*scene, duplicate, config));
    const std::array missing{ScriptSourceSnapshot{"scripts/other.lua", ""}};
    CHECK_FALSE(ScriptRuntime::create(*scene, missing, config));
    const std::array invalid_path{ScriptSourceSnapshot{"../escape.lua", ""}};
    CHECK_FALSE(ScriptRuntime::create(*scene, invalid_path, config));
    const std::array oversized{ScriptSourceSnapshot{
        "scripts/main.lua", std::string(script_runtime_maximum_source_bytes + 1, ' ')}};
    CHECK_FALSE(ScriptRuntime::create(*scene, oversized, config));
    const std::array malformed{ScriptSourceSnapshot{"scripts/main.lua", std::string("x\0y", 3)}};
    CHECK_FALSE(ScriptRuntime::create(*scene, malformed, config));
    const std::array syntax{ScriptSourceSnapshot{"scripts/main.lua", "function ="}};
    const auto invalid = ScriptRuntime::create(*scene, syntax, config);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().context.path == "scripts/main.lua");
    CHECK(take(scene->stats()).revision == before);

    auto disabled = region_scene();
    add_region(*disabled, 11, "Disabled", "scripts/missing.lua", false);
    auto empty = take(ScriptRuntime::create(*disabled, {}, config));
    take(empty->start(*disabled));
    CHECK(take(empty->stats()).scripts == 0);
    take(empty->stop(*disabled));
}

TEST_CASE("Script fixed ticks reject invalid input without consuming the next tick",
          "[script-runtime]") {
    auto scene = region_scene();
    add_region(*scene, 10, "Entity");
    auto runtime = make_runtime(
        *scene, "function on_fixed_update() shark.log.info(tostring(shark.time.tick())) end");
    take(runtime->start(*scene));
    take(runtime->fixed_update(*scene, 5, 0.05));
    CHECK_FALSE(runtime->fixed_update(*scene, 5, 0.05));
    CHECK_FALSE(runtime->fixed_update(*scene, 7, 0.05));
    CHECK_FALSE(runtime->fixed_update(*scene, 6, -0.1));
    CHECK_FALSE(runtime->fixed_update(*scene, 6, std::numeric_limits<double>::infinity()));
    CHECK_FALSE(runtime->fixed_update(*scene, 6, std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE(runtime->update(*scene, 1.01));
    CHECK_FALSE(take(runtime->stats()).faulted);
    take(runtime->fixed_update(*scene, 6, 0.05));
    CHECK(take(runtime->stats()).last_fixed_tick == 6);
    CHECK(messages(take(runtime->take_logs())) == std::vector<std::string>{"5", "6"});
    take(runtime->stop(*scene));
}

TEST_CASE("Lua logs and allocator exhaust within their bounds and remain drainable",
          "[script-runtime]") {
    auto scene = region_scene();
    add_region(*scene, 10, "Entity");
    auto logging = make_runtime(*scene, R"lua(
        function on_init() for i = 1, 257 do shark.log.info('x') end end
    )lua");
    CHECK_FALSE(logging->start(*scene));
    auto logs = take(logging->take_logs());
    CHECK(logs.size() == script_runtime_maximum_logs);
    CHECK(take(logging->take_logs()).empty());
    CHECK(take(logging->stop(*scene)).attempted == 0);

    auto config = server_config();
    config.max_scripts = 1;
    config.memory_bytes_per_script = 128 * 1024;
    auto memory = make_runtime(*scene, R"lua(
        function on_init() local t = {}; for i = 1, 100000 do t[i] = string.rep('x', 1024) end end
    )lua",
                               config);
    const auto exhausted = memory->start(*scene);
    REQUIRE_FALSE(exhausted);
    CHECK(exhausted.error().message.find("memory") != std::string::npos);
    CHECK(take(memory->stats()).faulted);
    take(memory->stop(*scene));
    auto fresh = make_runtime(*scene, "function on_init() shark.log.info('ok') end");
    take(fresh->start(*scene));
    CHECK(messages(take(fresh->take_logs())) == std::vector<std::string>{"ok"});
    take(fresh->stop(*scene));
}

namespace {
struct ScriptPhysicsFixture {
    std::unique_ptr<Scene> scene = region_scene();
    std::unique_ptr<PhysicsWorld> world;
    EntityHandle actor;
    EntityHandle trigger;
    PhysicsBodyHandle actor_body;
    PhysicsBodyHandle trigger_body;
    std::unique_ptr<ScriptRuntime> runtime;

    explicit ScriptPhysicsFixture(std::string source) {
        EntitySeed actor_seed{id<WorldEntityId>(101)};
        actor_seed.name = Name{"Actor"};
        actor_seed.script = Script{"scripts/main.lua", true};
        actor_seed.rigid_body = RigidBody{};
        actor_seed.rigid_body->motion = BodyMotion::dynamic_body;
        actor = take(scene->create_entity(actor_seed));
        EntitySeed trigger_seed{id<WorldEntityId>(102)};
        trigger_seed.name = Name{"Trigger"};
        trigger_seed.script = Script{"scripts/main.lua", true};
        trigger_seed.rigid_body = RigidBody{};
        trigger_seed.rigid_body->is_trigger = true;
        trigger = take(scene->create_entity(trigger_seed));
        world = take(PhysicsWorld::create({.gravity = {0, 0, 0}}));
        actor_body = take(world->create_body(*scene, actor));
        trigger_body = take(world->create_body(*scene, trigger));
        take(world->begin_simulation(*scene));
        runtime = make_runtime(*scene, std::move(source));
    }
};
} // namespace

TEST_CASE("Lua contact callbacks defer body removal and recreation until the ordered batch ends",
          "[script-runtime]") {
    ScriptPhysicsFixture f{R"lua(
        function on_contact_enter(other, self_trigger, other_trigger, tick)
            assert(tick == 1)
            if shark.entity.name() == 'Actor' then
                assert(not self_trigger and other_trigger)
                shark.physics.remove_body()
                shark.physics.create_body()
                local vx = shark.physics.velocity()
                assert(vx == 0) -- Deferred commands have not changed this body yet.
                shark.log.info('actor:' .. other)
            else
                assert(self_trigger and not other_trigger)
                shark.log.info('trigger:' .. other)
            end
        end
        function on_contact_exit(other, self_trigger, other_trigger, tick)
            shark.log.info('unexpected stale exit')
        end
    )lua"};
    take(f.runtime->start(*f.scene, *f.world));
    take(f.world->step(*f.scene));
    const auto delivered = take(f.runtime->dispatch_contacts(*f.scene, *f.world));
    REQUIRE(delivered.events.size() == 1);
    CHECK(delivered.events[0].phase == PhysicsContactPhase::enter);
    CHECK(delivered.attempted_callbacks == 2);
    CHECK(delivered.skipped_events == 0);
    CHECK(delivered.applied_commands == 2);
    CHECK(delivered.discarded_commands == 0);
    CHECK_FALSE(delivered.error);
    const auto logs = take(f.runtime->take_logs());
    REQUIRE(logs.size() == 2);
    CHECK(logs[0].entity.handle == f.actor);
    CHECK(logs[0].message == "actor:" + id<WorldEntityId>(102).value().to_string());
    CHECK(logs[1].entity.handle == f.trigger);
    CHECK(logs[1].message == "trigger:" + id<WorldEntityId>(101).value().to_string());

    CHECK_FALSE(f.world->body_state(*f.scene, f.actor_body));
    CHECK(take(f.scene->entity_state(f.actor)).rigid_body.has_value());
    PhysicsQueryFilter filter;
    filter.include_triggers = false;
    const auto current = take(f.world->overlap_sphere(*f.scene, {0, 0, 0}, 1, filter));
    REQUIRE(current.hits.size() == 1);
    CHECK(current.hits[0].entity.handle == f.actor);
    CHECK(current.hits[0].body != f.actor_body);

    // The removal exit still names the retired generation. It remains visible
    // as an owned observation, but cannot retarget the recreated actor's script.
    const auto stale = take(f.runtime->dispatch_contacts(*f.scene, *f.world));
    REQUIRE(stale.events.size() == 1);
    CHECK(stale.events[0].phase == PhysicsContactPhase::exit);
    CHECK(stale.events[0].cause == PhysicsContactCause::body_removed);
    CHECK(stale.skipped_events == 1);
    CHECK(stale.attempted_callbacks == 0);
    CHECK(take(f.runtime->take_logs()).empty());
    take(f.runtime->stop(*f.scene, *f.world));
}

TEST_CASE("Lua contact queue overflow discards the batch without mutating bodies",
          "[script-runtime]") {
    ScriptPhysicsFixture f{R"lua(
        function on_contact_enter(other, self_trigger, other_trigger, tick)
            if shark.entity.name() == 'Actor' then
                for i = 1, 65 do
                    shark.physics.impulse(1, 0, 0) -- The last exceeds the queue.
                end
            end
        end
    )lua"};
    take(f.runtime->start(*f.scene, *f.world));
    take(f.world->step(*f.scene));
    const auto delivered = take(f.runtime->dispatch_contacts(*f.scene, *f.world));
    REQUIRE(delivered.events.size() == 1);
    REQUIRE(delivered.error);
    CHECK(delivered.error->context.path == "scripts/main.lua");
    CHECK(delivered.error->context.entity_id == id<WorldEntityId>(101).value().to_string());
    CHECK(delivered.applied_commands == 0);
    CHECK(delivered.discarded_commands == 64);
    CHECK(take(f.runtime->stats()).faulted);
    CHECK(take(f.world->body_state(*f.scene, f.actor_body)).entity.handle == f.actor);
    CHECK(take(f.world->velocity(*f.scene, f.actor_body)).linear == math::Vec3{0, 0, 0});
    CHECK(take(f.scene->entity_state(f.actor)).rigid_body.has_value());
    CHECK(take(f.world->stats()).pending_events == 0);
    take(f.runtime->stop(*f.scene, *f.world));
}

TEST_CASE("Lua deferred physics application failure preserves partial effects and script context",
          "[script-runtime]") {
    ScriptPhysicsFixture f{R"lua(
        function on_contact_enter(other, self_trigger, other_trigger, tick)
            if shark.entity.name() == 'Actor' then
                shark.physics.impulse(1, 0, 0)
                shark.physics.create_body()
            end
        end
    )lua"};
    take(f.runtime->start(*f.scene, *f.world));
    take(f.world->step(*f.scene));
    const auto delivered = take(f.runtime->dispatch_contacts(*f.scene, *f.world));
    REQUIRE(delivered.error);
    CHECK(delivered.error->context.operation == "script.contacts.dispatch");
    CHECK(delivered.error->context.path == "scripts/main.lua");
    CHECK(delivered.error->context.entity_id == id<WorldEntityId>(101).value().to_string());
    CHECK(delivered.applied_commands == 1);
    CHECK(delivered.discarded_commands == 1);
    CHECK(take(f.world->velocity(*f.scene, f.actor_body)).linear.x == Catch::Approx(1));
    CHECK(take(f.runtime->stats()).faulted);
    take(f.runtime->stop(*f.scene, *f.world));
}

TEST_CASE("Lua server physics bindings use the shared owner for controls and bounded queries",
          "[script-runtime]") {
    ScriptPhysicsFixture f{R"lua(
        function on_fixed_update()
            if shark.entity.name() ~= 'Actor' then return end
            local vx, vy, vz, ax, ay, az = shark.physics.velocity()
            assert(vx == 0 and vy == 0 and vz == 0 and ax == 0 and ay == 0 and az == 0)
            shark.physics.set_velocity(1, 0, 0, 0, 0, 0)
            shark.physics.impulse(2, 0, 0)
            shark.physics.force(60, 0, 0)
            local hits = shark.physics.raycast(-5, 0, 0, 5, 0, 0, 0, 65535, false)
            assert(#hits == 1 and not hits[1].is_trigger)
            assert(math.abs(hits[1].fraction - 0.45) < 0.0001)
            assert(math.abs(hits[1].x + 0.5) < 0.0001)
            assert(math.abs(hits[1].normal_x + 1) < 0.0001)
            shark.log.info('ray:' .. hits[1].entity_id)
            local spheres = shark.physics.overlap_sphere(0, 0, 0, 1, 0, 65535, false)
            local boxes = shark.physics.overlap_box(0, 0, 0, 1, 1, 1, 0, 65535, true)
            assert(#spheres == 1 and #boxes == 2)
            assert(spheres[1].entity_id == hits[1].entity_id)
        end
    )lua"};
    take(f.runtime->start(*f.scene, *f.world));
    take(f.runtime->fixed_update(*f.scene, *f.world, 0, 1.0 / 60.0));
    CHECK(take(f.world->velocity(*f.scene, f.actor_body)).linear.x == 3);
    const auto logs = take(f.runtime->take_logs());
    REQUIRE(logs.size() == 1);
    CHECK(logs[0].message == "ray:" + id<WorldEntityId>(101).value().to_string());
    take(f.world->step(*f.scene));
    CHECK(take(f.world->velocity(*f.scene, f.actor_body)).linear.x ==
          Catch::Approx(4).margin(1e-5));
    take(f.runtime->stop(*f.scene, *f.world));
}

TEST_CASE("Lua physics bindings reject invalid input and protect client authority",
          "[script-runtime]") {
    const std::array invalid_sources{
        "function on_fixed_update() shark.physics.velocity(1) end",
        "function on_fixed_update() shark.physics.force('x', 0, 0) end",
        "function on_fixed_update() shark.physics.impulse(0, 0, math.huge) end",
        "function on_fixed_update() shark.physics.set_velocity(257, 0, 0, 0, 0, 0) end",
        "function on_fixed_update() shark.physics.raycast(0, 0, 0, 0, 0, 0, 0, 65535, true) end",
        "function on_fixed_update() shark.physics.raycast(-1, 0, 0, 1, 0, 0, 0.5, 65535, true) end",
        "function on_fixed_update() shark.physics.raycast(-1, 0, 0, 1, 0, 0, 16, 65535, true) end",
        "function on_fixed_update() shark.physics.raycast(-1, 0, 0, 1, 0, 0, 0, -1, true) end",
        "function on_fixed_update() shark.physics.raycast(-1, 0, 0, 1, 0, 0, 0, 65535, 1) end",
        "function on_fixed_update() shark.physics.overlap_sphere(0, 0, 0, 0, 0, 65535, true) end",
        "function on_fixed_update() shark.physics.overlap_box(0, 0, 0, 1, -1, 1, 0, 65535, true) "
        "end",
        "function on_fixed_update() shark.physics.remove_body() end",
        "function on_fixed_update() shark.physics.create_body() end",
        "function on_fixed_update() shark.physics.teleport(100000, 0, 0) end",
        "function on_fixed_update() shark.physics.kinematic_target(1, 0, 0) end"};
    for (const auto* source : invalid_sources) {
        INFO(source);
        ScriptPhysicsFixture f{source};
        take(f.runtime->start(*f.scene, *f.world));
        const auto result = f.runtime->fixed_update(*f.scene, *f.world, 0, 1.0 / 60.0);
        CHECK_FALSE(result);
        CHECK(take(f.runtime->stats()).faulted);
        CHECK(take(f.scene->entity_state(f.actor)).rigid_body.has_value());
        take(f.runtime->stop(*f.scene, *f.world));
    }
    auto scene = authored_scene();
    EntitySeed seed{id<AuthoredEntityId>(101)};
    seed.script = Script{"scripts/main.lua", true};
    seed.rigid_body = RigidBody{};
    seed.rigid_body->motion = BodyMotion::dynamic_body;
    const auto entity = take(scene->create_entity(seed));
    auto world = take(PhysicsWorld::create({.gravity = {0, 0, 0}}));
    const auto body = take(world->create_body(*scene, entity));
    take(world->begin_simulation(*scene));
    ScriptRuntimeConfig client;
    auto runtime = make_runtime(*scene, R"lua(
        function on_fixed_update()
            local vx = shark.physics.velocity()
            local hits = shark.physics.overlap_sphere(0, 0, 0, 1, 0, 65535, false)
            assert(vx == 0 and #hits == 1)
            shark.log.info('client reads succeeded')
            shark.physics.impulse(1, 0, 0)
        end
    )lua",
                                client);
    take(runtime->start(*scene, *world));
    CHECK_FALSE(runtime->fixed_update(*scene, *world, 0, 1.0 / 60.0));
    CHECK(messages(take(runtime->take_logs())) ==
          std::vector<std::string>{"client reads succeeded"});
    CHECK(take(world->velocity(*scene, body)).linear == math::Vec3{0, 0, 0});
    take(runtime->stop(*scene, *world));
}

TEST_CASE("Lua teleport and kinematic targets preserve mode and fixed-step rules",
          "[script-runtime]") {
    {
        ScriptPhysicsFixture f{R"lua(
            function on_fixed_update()
                if shark.entity.name() == 'Actor' then shark.physics.teleport(2, 0, 0) end
            end
        )lua"};
        take(f.world->set_velocity(*f.scene, f.actor_body, {{3, 0, 0}, {0, 1, 0}}));
        take(f.runtime->start(*f.scene, *f.world));
        take(f.runtime->fixed_update(*f.scene, *f.world, 0, 1.0 / 60.0));
        CHECK(take(f.scene->entity_state(f.actor)).local_transform.translation.x == 2);
        CHECK(take(f.world->velocity(*f.scene, f.actor_body)).linear == math::Vec3{0, 0, 0});
        take(f.runtime->stop(*f.scene, *f.world));
    }
    {
        auto scene = region_scene();
        EntitySeed seed{id<WorldEntityId>(103)};
        seed.script = Script{"scripts/main.lua", true};
        seed.rigid_body = RigidBody{};
        seed.rigid_body->motion = BodyMotion::kinematic_body;
        const auto entity = take(scene->create_entity(seed));
        auto world = take(PhysicsWorld::create({.gravity = {0, 0, 0}}));
        take(world->create_body(*scene, entity));
        take(world->begin_simulation(*scene));
        auto runtime = make_runtime(
            *scene, "function on_fixed_update() shark.physics.kinematic_target(2, 0, 0) end");
        take(runtime->start(*scene, *world));
        take(runtime->fixed_update(*scene, *world, 0, 1.0 / 60.0));
        CHECK(take(scene->entity_state(entity)).local_transform.translation.x == 0);
        take(world->step(*scene));
        CHECK(take(scene->entity_state(entity)).local_transform.translation.x ==
              Catch::Approx(2).margin(1e-5));
        take(runtime->stop(*scene, *world));
    }
}

TEST_CASE("Lua physics queries reject overflow rather than returning partial hits",
          "[script-runtime]") {
    auto scene = region_scene();
    std::vector<EntityHandle> entities;
    for (unsigned index = 0; index < 33; ++index) {
        EntitySeed seed{id<WorldEntityId>(200 + index)};
        seed.rigid_body = RigidBody{};
        if (index == 0)
            seed.script = Script{"scripts/main.lua", true};
        entities.push_back(take(scene->create_entity(seed)));
    }
    auto world = take(PhysicsWorld::create({.gravity = {0, 0, 0}}));
    for (const auto entity : entities)
        take(world->create_body(*scene, entity));
    take(world->begin_simulation(*scene));
    auto runtime = make_runtime(*scene, R"lua(
        function on_fixed_update()
            shark.physics.overlap_sphere(0, 0, 0, 1, 0, 65535, true)
        end
    )lua");
    take(runtime->start(*scene, *world));
    CHECK_FALSE(runtime->fixed_update(*scene, *world, 0, 1.0 / 60.0));
    CHECK(take(runtime->stats()).faulted);
    take(runtime->stop(*scene, *world));
}
