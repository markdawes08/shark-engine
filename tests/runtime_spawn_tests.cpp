#include <shark/physics.hpp>
#include <shark/runtime_prefab.hpp>
#include <shark/scene.hpp>
#include <shark/script_runtime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace shark;

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        REQUIRE(result);
    }
    REQUIRE(result);
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result) {
        INFO(result.error().message);
        REQUIRE(result);
    }
    REQUIRE(result);
}
template <typename T> T id(std::uint32_t value) {
    std::string text = "21c00000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 15U];
        value >>= 4U;
    }
    return T{take(StableId::parse(text))};
}
RegionSceneScope region(std::uint64_t epoch = 1) {
    return {id<WorldId>(1), {id<RegionId>(2), id<RegionInstanceId>(3), epoch}};
}
ScriptRuntimeConfig server_config(std::size_t maximum = 16) {
    ScriptRuntimeConfig result;
    result.profile = ScriptProfile::server_gameplay;
    result.max_scripts = maximum;
    return result;
}
RuntimePrefab prefab(bool child = true) {
    PreparedEntity root{id<AuthoredEntityId>(10)};
    root.name = {"spawned"};
    root.rigid_body = RigidBody{};
    root.rigid_body->motion = BodyMotion::dynamic_body;
    root.script = Script{"scripts/spawned.lua", true};
    RuntimePrefab result{id<AssetId>(20), root.id, {}};
    if (child) {
        PreparedEntity visual{id<AuthoredEntityId>(11), root.id};
        visual.name = {"visual"};
        visual.transform.translation.x = 2;
        visual.camera = Camera{};
        visual.script = Script{"scripts/visual.lua", true};
        result.entities.push_back(std::move(visual));
    }
    result.entities.push_back(std::move(root));
    return result;
}
std::string spawn_call(std::uint64_t sequence, int x = 10) {
    return "shark.prefab.spawn(" + std::to_string(sequence) + ",'" +
           id<AssetId>(20).value().to_string() + "'," + std::to_string(x) + ",0,0)";
}
std::vector<std::string> messages(std::vector<ScriptLogEntry> logs) {
    std::vector<std::string> result;
    for (auto& entry : logs)
        result.push_back(std::move(entry.message));
    return result;
}
struct Fixture {
    std::unique_ptr<Scene> scene;
    EntityHandle issuer;
    std::unique_ptr<PhysicsWorld> world;
    std::vector<ScriptSourceSnapshot> sources;
    std::unique_ptr<ScriptRuntime> runtime;
    std::unique_ptr<RuntimeSpawnAuthority> authority;
    std::vector<RuntimePrefab> catalog;

    Fixture(std::string issuer_source, bool issuer_body = false, bool with_child = true,
            std::string spawned_source = {}, std::size_t maximum_scripts = 16,
            bool passive_prefab = false) {
        scene = take(Scene::create(region()));
        EntitySeed seed{id<WorldEntityId>(100)};
        seed.name = {"issuer"};
        seed.script = Script{"scripts/issuer.lua", true};
        if (issuer_body) {
            seed.rigid_body = RigidBody{};
            seed.rigid_body->motion = BodyMotion::dynamic_body;
        }
        issuer = take(scene->create_entity(seed));
        world = take(PhysicsWorld::create({.gravity = {0, 0, 0}}));
        if (issuer_body)
            take(world->create_body(*scene, issuer));
        take(world->begin_simulation(*scene));
        sources = {{"scripts/issuer.lua", std::move(issuer_source)},
                   {"scripts/spawned.lua", std::move(spawned_source)},
                   {"scripts/visual.lua", ""}};
        runtime = take(ScriptRuntime::create(*scene, sources, server_config(maximum_scripts)));
        authority = take(RuntimeSpawnAuthority::create(*scene, region()));
        catalog.push_back(prefab(with_child));
        if (passive_prefab)
            for (auto& entity : catalog.front().entities) {
                entity.rigid_body.reset();
                entity.script.reset();
            }
        take(runtime->enable_spawning(*scene, *world, *authority, catalog));
    }
    void start() {
        take(runtime->start(*scene, *world));
    }
    void update() {
        take(runtime->update(*scene, *world, 1.0 / 60.0));
    }
};
} // namespace

TEST_CASE("runtime prefab values validate a complete bounded local hierarchy",
          "[runtime-spawning]") {
    auto value = prefab();
    const auto original = value;
    const auto valid = take(validate_runtime_prefab(value));
    CHECK(valid == original);
    SECTION("unresolved root") {
        value.root = id<AuthoredEntityId>(99);
    }
    SECTION("duplicate ID") {
        value.entities.front().id = value.root;
    }
    SECTION("external reference") {
        value.entities.front().parent = id<AuthoredEntityId>(99);
    }
    SECTION("second root") {
        value.entities.front().parent.reset();
    }
    SECTION("cyclic reference") {
        value.entities.back().parent = value.entities.front().id;
    }
    SECTION("child rigid body") {
        value.entities.front().rigid_body = RigidBody{};
    }
    SECTION("invalid source") {
        value.entities.front().script->source = "../escape.lua";
    }
    SECTION("invalid transform") {
        value.entities.front().transform.translation.x = std::numeric_limits<double>::infinity();
    }
    const auto rejected = validate_runtime_prefab(value);
    REQUIRE_FALSE(rejected);
    CHECK_FALSE(rejected.error().message.empty());
    CHECK(original == valid);
}

TEST_CASE("runtime spawn authority binds the exact region and Scene incarnation",
          "[runtime-spawning]") {
    auto scene = take(Scene::create(region()));
    auto second = take(Scene::create(region()));
    CHECK_FALSE(RuntimeSpawnAuthority::create(*scene, region(2)));
    auto authored = take(Scene::create(AuthoredSceneScope{id<SceneId>(1)}));
    CHECK_FALSE(RuntimeSpawnAuthority::create(*authored, region()));
    auto grant = take(RuntimeSpawnAuthority::create(*scene, region()));
    auto world = take(PhysicsWorld::create());
    take(world->begin_simulation(*second));
    auto runtime = take(ScriptRuntime::create(*second, {}, server_config()));
    const std::array catalog{prefab()};
    CHECK_FALSE(runtime->enable_spawning(*second, *world, *grant, catalog));
    std::optional<Result<void>> foreign;
    std::thread worker([&] { foreign.emplace(grant->revoke()); });
    worker.join();
    REQUIRE(foreign);
    CHECK_FALSE(*foreign);
    take(grant->revoke());
    take(grant->revoke());
    CHECK_FALSE(runtime->enable_spawning(*second, *world, *grant, catalog));
}

TEST_CASE("runtime spawn defers publication and initializes complete independent instances",
          "[runtime-spawning]") {
    Fixture f{
        "function issue() if done then return end; done=true; "
        "local a=" +
            spawn_call(1, 10) + "; local b=" + spawn_call(2, 20) +
            "; assert(a~=b); assert(#shark.physics.overlap_sphere(10,0,0,1,0,65535,true)==0); "
            "shark.log.info('queued') end; "
            "function on_update() issue() end; function on_fixed_update() issue() end",
        false, true,
        "function on_init() local x=shark.entity.position(); "
        "local vx=shark.physics.velocity(); assert(vx==0); "
        "shark.log.info('init:'..math.floor(x)) end"};
    // Captures must own their values rather than borrow the host's vectors.
    f.catalog.front().entities.front().name = {"host changed"};
    f.sources[1].text = "error('host replaced source')";
    f.start();
    bool fixed = false;
    SECTION("frame callback") {
        f.update();
    }
    SECTION("fixed callback") {
        fixed = true;
        take(f.runtime->fixed_update(*f.scene, *f.world, 37, 0.05));
    }
    CHECK(take(f.scene->stats()).live_entities == 5);
    CHECK(take(f.world->stats()).bodies == 2);
    CHECK(take(f.runtime->stats()).initialized == 5);
    const auto owned_logs = take(f.runtime->take_logs());
    for (const auto& entry : owned_logs)
        CHECK(entry.tick == (fixed ? 37U : 0U));
    const auto logs = messages(owned_logs);
    REQUIRE(logs.size() == 3);
    CHECK(logs.front() == "queued");
    CHECK(std::count(logs.begin(), logs.end(), "init:10") == 1);
    CHECK(std::count(logs.begin(), logs.end(), "init:20") == 1);
    const auto report = take(f.runtime->take_entity_mutations());
    REQUIRE(report.outcomes.size() == 2);
    std::set<WorldEntityId> roots;
    for (const auto& outcome : report.outcomes) {
        CHECK(outcome->kind == ScriptEntityMutationKind::spawn);
        CHECK(outcome->state == ScriptEntityMutationState::applied);
        CHECK_FALSE(outcome->error);
        REQUIRE(outcome->root);
        CHECK(roots.insert(*outcome->root).second);
        REQUIRE(outcome->created_entities.size() == 2);
        CHECK(outcome->created_bodies == 1);
        CHECK(outcome->initialized_scripts == 2);
        const auto root = take(f.scene->find(*outcome->root));
        const auto state = take(f.scene->entity_state(root));
        CHECK_FALSE(state.parent);
        CHECK(state.reference.authority == region().authority);
        REQUIRE(state.children.size() == 1);
        const auto child = take(f.scene->entity_state(state.children.front()));
        CHECK(child.parent == root);
        CHECK(child.name == Name{"visual"});
        CHECK(child.local_transform.translation.x == 2);
        CHECK(child.world_matrix[3][0] == state.local_transform.translation.x + 2);
        CHECK(std::holds_alternative<WorldEntityId>(child.reference.identity));
    }
    CHECK(take(f.runtime->take_entity_mutations()).outcomes.empty());
    take(f.runtime->stop(*f.scene, *f.world));
}

TEST_CASE("runtime mutation retries preserve one reserved ID and reject payload changes",
          "[runtime-spawning]") {
    SECTION("identical retry") {
        Fixture f{"function on_update() local current=" + spawn_call(1) +
                      "; if prior then assert(prior==current) end; prior=current end",
                  false, false};
        f.start();
        f.update();
        const auto original = take(f.runtime->take_entity_mutations());
        REQUIRE(original.outcomes.size() == 1);
        const auto revision = take(f.scene->stats()).revision;
        f.update();
        CHECK(take(f.scene->stats()).revision == revision);
        CHECK(take(f.scene->stats()).live_entities == 2);
        CHECK(take(f.runtime->take_entity_mutations()).outcomes.empty());
        CHECK_FALSE(take(f.runtime->stats()).faulted);
    }
    SECTION("mismatched retry") {
        Fixture f{"function on_update() n=(n or 9)+1; shark.prefab.spawn(1,'" +
                      id<AssetId>(20).value().to_string() + "',n,0,0) end",
                  false, false};
        f.start();
        f.update();
        take(f.runtime->take_entity_mutations());
        const auto before = take(f.scene->stats());
        CHECK_FALSE(f.runtime->update(*f.scene, *f.world, 0.01));
        CHECK(take(f.scene->stats()).revision == before.revision);
        CHECK(take(f.scene->stats()).live_entities == 2);
        CHECK(take(f.runtime->stats()).faulted);
    }
}

TEST_CASE("runtime expired mutation receipts cannot create another instance",
          "[runtime-spawning]") {
    const auto retained = script_runtime_maximum_entity_mutations;
    Fixture f{"function on_update() n=(n or 0)+1; local sequence=n; if n>" +
                  std::to_string(retained + 1) +
                  " then sequence=1 end; shark.prefab.spawn(sequence,'" +
                  id<AssetId>(20).value().to_string() + "',sequence,0,0) end",
              false,
              false,
              {},
              16,
              true};
    f.start();
    for (std::size_t index = 0; index <= retained; ++index) {
        f.update();
        CHECK(take(f.runtime->take_entity_mutations()).outcomes.size() == 1);
    }
    const auto before = take(f.scene->stats());
    CHECK_FALSE(f.runtime->update(*f.scene, *f.world, 0.01));
    CHECK(take(f.scene->stats()).revision == before.revision);
    CHECK(take(f.scene->stats()).live_entities == before.live_entities);
    CHECK(take(f.runtime->stats()).faulted);
}

TEST_CASE("runtime self destruction waits for update completion and tears down in order",
          "[runtime-spawning]") {
    Fixture f{R"lua(
        function on_update()
            shark.entity.destroy(1)
            assert(shark.entity.name()=='issuer')
            shark.log.info('queued')
        end
        function on_destroy()
            local vx=shark.physics.velocity()
            assert(vx==0)
            shark.log.info('destroyed')
        end
    )lua",
              true, false};
    const auto original = take(f.scene->reference(f.issuer));
    f.start();
    f.update();
    CHECK(messages(take(f.runtime->take_logs())) ==
          std::vector<std::string>{"queued", "destroyed"});
    CHECK_FALSE(f.scene->resolve(original));
    CHECK(take(f.scene->stats()).live_entities == 0);
    CHECK(take(f.world->stats()).bodies == 0);
    CHECK(take(f.runtime->stats()).initialized == 0);
    const auto report = take(f.runtime->take_entity_mutations());
    REQUIRE(report.outcomes.size() == 1);
    const auto& receipt = *report.outcomes.front();
    CHECK(receipt.kind == ScriptEntityMutationKind::destroy);
    CHECK(receipt.state == ScriptEntityMutationState::applied);
    CHECK(receipt.removed_entities == std::vector<EntityReference>{original});
    CHECK(receipt.removed_bodies == 1);
    CHECK(receipt.destroyed_scripts == 1);
    CHECK_FALSE(receipt.error);
    CHECK(take(f.runtime->stop(*f.scene, *f.world)).attempted == 0);
}

TEST_CASE("runtime spawn authority revocation rejects new work without effects",
          "[runtime-spawning]") {
    Fixture f{"function on_update() " + spawn_call(1) + " end", false, false};
    f.start();
    const auto before = take(f.scene->stats());
    take(f.authority->revoke());
    CHECK_FALSE(f.runtime->update(*f.scene, *f.world, 0.01));
    CHECK(take(f.scene->stats()).revision == before.revision);
    CHECK(take(f.scene->stats()).live_entities == before.live_entities);
    CHECK(take(f.world->stats()).bodies == 0);
}

TEST_CASE("runtime spawn rejects invalid arguments phase and missing authority",
          "[runtime-spawning]") {
    std::string source;
    bool at_init = false;
    SECTION("zero sequence") {
        source = spawn_call(0);
    }
    SECTION("sequence gap") {
        source = spawn_call(2);
    }
    SECTION("unknown asset") {
        source = "shark.prefab.spawn(1,'" + id<AssetId>(999).value().to_string() + "',0,0,0)";
    }
    SECTION("nonfinite position") {
        source = "shark.prefab.spawn(1,'" + id<AssetId>(20).value().to_string() + "',0/0,0,0)";
    }
    SECTION("init cannot spawn") {
        source = spawn_call(1);
        at_init = true;
    }
    SECTION("wrong argument type") {
        source = "shark.entity.destroy('1')";
    }
    Fixture f{"function " + std::string(at_init ? "on_init" : "on_update") + "() " + source +
                  " end",
              false, false};
    const auto before = take(f.scene->stats());
    if (at_init)
        CHECK_FALSE(f.runtime->start(*f.scene, *f.world));
    else {
        f.start();
        CHECK_FALSE(f.runtime->update(*f.scene, *f.world, 0.01));
    }
    CHECK(take(f.scene->stats()).revision == before.revision);
    CHECK(take(f.scene->stats()).live_entities == 1);
    CHECK(take(f.world->stats()).bodies == 0);
}

TEST_CASE("runtime repeated spawn destroy releases dynamic script and body capacity",
          "[runtime-spawning]") {
    Fixture f{"function on_update() sequence=(sequence or 0)+1; shark.prefab.spawn(sequence,'" +
                  id<AssetId>(20).value().to_string() + "',sequence*3,0,0) end",
              false, false, "function on_update() shark.entity.destroy(1) end", 3};
    f.start();
    std::set<WorldEntityId> roots;
    for (int pass = 0; pass < 12; ++pass) {
        f.update();
        const auto report = take(f.runtime->take_entity_mutations());
        for (const auto& receipt : report.outcomes) {
            CHECK(receipt->state == ScriptEntityMutationState::applied);
            CHECK_FALSE(receipt->error);
            if (receipt->kind == ScriptEntityMutationKind::spawn) {
                REQUIRE(receipt->root);
                CHECK(roots.insert(*receipt->root).second);
            }
        }
        CHECK(take(f.scene->stats()).live_entities == 2);
        CHECK(take(f.world->stats()).bodies == 1);
        CHECK(take(f.runtime->stats()).initialized == 2);
        take(f.runtime->take_logs());
    }
    CHECK(roots.size() == 12);
    take(f.authority->revoke());
    const auto stopped = take(f.runtime->stop(*f.scene, *f.world));
    CHECK(stopped.errors.empty());
    CHECK(stopped.remaining_spawned_roots == 0);
    CHECK(stopped.removed_spawned_entities == 1);
    CHECK(stopped.removed_spawned_bodies == 1);
    CHECK(take(f.scene->stats()).live_entities == 1);
    CHECK(take(f.world->stats()).bodies == 0);
    take(f.world->close());
    CHECK(take(f.world->stats()).bodies == 0);
    CHECK(take(f.runtime->stats()).initialized == 0);
}

TEST_CASE("runtime contact self destruction preserves callback order and stale exits",
          "[runtime-spawning]") {
    auto scene = take(Scene::create(region()));
    EntitySeed actor{id<WorldEntityId>(100)};
    actor.name = {"issuer"};
    actor.script = Script{"scripts/contact.lua", true};
    actor.rigid_body = RigidBody{};
    actor.rigid_body->motion = BodyMotion::dynamic_body;
    const auto actor_handle = take(scene->create_entity(actor));
    const auto actor_reference = take(scene->reference(actor_handle));
    EntitySeed sensor{id<WorldEntityId>(101)};
    sensor.name = {"sensor"};
    sensor.script = actor.script;
    sensor.rigid_body = RigidBody{};
    sensor.rigid_body->is_trigger = true;
    const auto sensor_handle = take(scene->create_entity(sensor));
    auto world = take(PhysicsWorld::create({.gravity = {0, 0, 0}}));
    take(world->create_body(*scene, actor_handle));
    take(world->create_body(*scene, sensor_handle));
    take(world->begin_simulation(*scene));
    const std::array sources{ScriptSourceSnapshot{"scripts/contact.lua", R"lua(
        function on_contact_enter()
            local name=shark.entity.name()
            if name=='issuer' then shark.entity.destroy(1) end
            shark.log.info('contact:'..name)
        end
        function on_destroy() shark.log.info('destroy:'..shark.entity.name()) end
    )lua"}};
    auto runtime = take(ScriptRuntime::create(*scene, sources, server_config()));
    auto authority = take(RuntimeSpawnAuthority::create(*scene, region()));
    auto value = prefab(false);
    value.entities.front().script.reset();
    const std::array catalog{value};
    take(runtime->enable_spawning(*scene, *world, *authority, catalog));
    take(runtime->start(*scene, *world));
    take(world->step(*scene));
    const auto contact = take(runtime->dispatch_contacts(*scene, *world));
    CHECK_FALSE(contact.error);
    CHECK(contact.attempted_callbacks == 2);
    const auto contact_logs = take(runtime->take_logs());
    REQUIRE_FALSE(contact_logs.empty());
    CHECK(contact_logs.back().tick == 1);
    CHECK(messages(contact_logs) ==
          std::vector<std::string>{"contact:issuer", "contact:sensor", "destroy:issuer"});
    CHECK_FALSE(scene->resolve(actor_reference));
    CHECK(take(scene->stats()).live_entities == 1);
    CHECK(take(world->stats()).bodies == 1);
    const auto mutations = take(runtime->take_entity_mutations());
    REQUIRE(mutations.outcomes.size() == 1);
    CHECK(mutations.outcomes.front()->state == ScriptEntityMutationState::applied);
    CHECK(mutations.outcomes.front()->removed_bodies == 1);
    const auto removed_exit = take(runtime->dispatch_contacts(*scene, *world));
    CHECK_FALSE(removed_exit.error);
    CHECK(removed_exit.events.size() == 1);
    CHECK(removed_exit.skipped_events == 1);
    CHECK(removed_exit.attempted_callbacks == 0);
    CHECK(take(runtime->take_logs()).empty());
    take(runtime->stop(*scene, *world));
}

TEST_CASE("runtime client and replica profiles cannot acquire structural authority",
          "[runtime-spawning]") {
    auto owner_scene = take(Scene::create(region()));
    auto authority = take(RuntimeSpawnAuthority::create(*owner_scene, region()));
    auto world = take(PhysicsWorld::create());
    take(world->begin_simulation(*owner_scene));
    std::unique_ptr<Scene> scene;
    EntitySeed seed{id<AuthoredEntityId>(100)};
    SECTION("authored presentation") {
        scene = take(Scene::create(AuthoredSceneScope{id<SceneId>(1)}));
    }
    SECTION("replica presentation") {
        scene = take(Scene::create(ReplicaSceneScope{id<WorldId>(1), id<ReplicaSessionId>(2)}));
        seed.identity = id<WorldEntityId>(100);
        seed.replica_authority = region().authority;
    }
    seed.script = Script{"scripts/client.lua", true};
    const auto entity = take(scene->create_entity(seed));
    const auto original = take(scene->reference(entity));
    const std::array sources{ScriptSourceSnapshot{
        "scripts/client.lua", "function on_update() " + spawn_call(1) + " end"}};
    auto runtime = take(ScriptRuntime::create(*scene, sources));
    const std::array catalog{prefab(false)};
    CHECK_FALSE(runtime->enable_spawning(*scene, *world, *authority, catalog));
    take(runtime->start(*scene));
    const auto before = take(scene->stats());
    CHECK_FALSE(runtime->update(*scene, 0.01));
    CHECK(take(scene->stats()).revision == before.revision);
    CHECK(take(scene->stats()).live_entities == 1);
    CHECK(scene->resolve(original));
    CHECK(take(world->stats()).bodies == 0);
}

TEST_CASE("runtime spawn initialization failure reports surviving partial effects",
          "[runtime-spawning]") {
    Fixture f{"function on_update() " + spawn_call(1) + " end", false, false,
              "function on_init() error('expected init failure') end"};
    f.start();
    CHECK_FALSE(f.runtime->update(*f.scene, *f.world, 0.01));
    CHECK(take(f.runtime->stats()).faulted);
    const auto report = take(f.runtime->take_entity_mutations());
    REQUIRE(report.outcomes.size() == 1);
    const auto& receipt = *report.outcomes.front();
    CHECK(receipt.state == ScriptEntityMutationState::failed);
    REQUIRE(receipt.error);
    CHECK(receipt.error->message.find("expected init failure") != std::string::npos);
    CHECK(receipt.error->context.path == "scripts/spawned.lua");
    REQUIRE(receipt.root);
    CHECK_FALSE(receipt.created_entities.empty());
    CHECK(receipt.created_bodies == 1);
    CHECK(take(f.scene->stats()).live_entities == 2);
    CHECK(take(f.world->stats()).bodies == 1);
    CHECK(f.scene->find(*receipt.root));
    take(f.runtime->stop(*f.scene, *f.world));
    take(f.world->close());
    CHECK(take(f.world->stats()).bodies == 0);
}

TEST_CASE("runtime structural queue overflow rejects the entire pending batch",
          "[runtime-spawning]") {
    auto scene = take(Scene::create(region()));
    EntitySeed issuer{id<WorldEntityId>(100)};
    issuer.script = Script{"scripts/overflow.lua", true};
    take(scene->create_entity(issuer));
    auto world = take(PhysicsWorld::create());
    take(world->begin_simulation(*scene));
    const std::array sources{ScriptSourceSnapshot{
        "scripts/overflow.lua", "function on_update() for i=1,65 do shark.prefab.spawn(i,'" +
                                    id<AssetId>(20).value().to_string() + "',i,0,0) end end"}};
    auto runtime = take(ScriptRuntime::create(*scene, sources, server_config()));
    auto authority = take(RuntimeSpawnAuthority::create(*scene, region()));
    const std::array catalog{
        RuntimePrefab{id<AssetId>(20), id<AuthoredEntityId>(10), {{id<AuthoredEntityId>(10)}}}};
    take(runtime->enable_spawning(*scene, *world, *authority, catalog));
    take(runtime->start(*scene, *world));
    const auto before = take(scene->stats());
    CHECK_FALSE(runtime->update(*scene, *world, 0.01));
    CHECK(take(runtime->stats()).faulted);
    CHECK(take(scene->stats()).revision == before.revision);
    CHECK(take(scene->stats()).live_entities == 1);
    CHECK(take(world->stats()).bodies == 0);
    const auto report = take(runtime->take_entity_mutations());
    REQUIRE_FALSE(report.outcomes.empty());
    for (const auto& receipt : report.outcomes) {
        CHECK(receipt->state == ScriptEntityMutationState::discarded);
        CHECK(receipt->created_entities.empty());
        CHECK(receipt->created_bodies == 0);
    }
}
