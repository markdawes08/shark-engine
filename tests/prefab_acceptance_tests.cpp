#include <shark/physics.hpp>
#include <shark/scene_render.hpp>
#include <shark/script_runtime.hpp>
#include <shark/tools/prepared_content.hpp>
#include <shark/tools/project_files.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace shark;
using namespace shark::tools;
namespace fs = std::filesystem;

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        REQUIRE(result);
    }
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result) {
        INFO(result.error().message);
        REQUIRE(result);
    }
}
template <typename T> T id(unsigned number) {
    const auto suffix = std::to_string(number);
    return T{take(StableId::parse("21e00000-0000-4000-8000-" +
                                  std::string(12 - suffix.size(), '0') + suffix))};
}
RegionSceneScope region(std::uint64_t epoch) {
    return {id<WorldId>(2), {id<RegionId>(3), id<RegionInstanceId>(4), epoch}};
}
std::string actor_source(unsigned version) {
    return R"lua(
        local age=0
        function on_init()
            local x,y,z=shark.entity.position()
            assert((x==10 or x==20) and y==0 and z==0)
            local vx,vy,vz=shark.physics.velocity()
            assert(vx==0 and vy==0 and vz==0)
            shark.log.info('init-root-v)lua" +
           std::to_string(version) + R"lua(')
        end
        function on_fixed_update()
            age=age+1
            if age==1 then
                local x=shark.entity.position()
                shark.physics.set_velocity(x<15 and 1 or -1,0,0,0,0,0)
            elseif age==2 then
                shark.entity.destroy(1)
            end
            shark.log.info('fixed-root')
        end
        function on_destroy()
            local vx=shark.physics.velocity()
            assert(math.abs(vx)==1 or vx==0)
            shark.log.info('destroy-root')
        end
    )lua";
}

class Archive final {
  public:
    Archive() {
        root = fs::temp_directory_path() /
               ("shark-prefab-acceptance-" + take(generate_stable_id()).to_string());
        REQUIRE(fs::create_directory(root));
        REQUIRE(fs::create_directory(root / "assets"));
        REQUIRE(fs::create_directory(root / "scripts"));
        files = take(ProjectFiles::open(root));
        SourceEntity actor{id<AuthoredEntityId>(10), {}, {"Actor"}};
        actor.transform.translation.x = 99;
        actor.rigid_body = RigidBody{};
        actor.rigid_body->motion = BodyMotion::dynamic_body;
        actor.script = Script{"scripts/actor.lua", true};
        actor.extras_json = R"({"source_only":"root"})";
        SourceEntity child{id<AuthoredEntityId>(11), actor.id, {"Child"}};
        child.transform.translation.x = 2;
        child.script = Script{"scripts/child.lua", true};
        SourceEntity leaf{id<AuthoredEntityId>(12), child.id, {"Leaf"}};
        leaf.transform.translation.y = 3;
        leaf.mesh_renderer = MeshRenderer{id<AssetId>(30), 0, true};
        leaf.script = Script{"scripts/leaf.lua", true};
        // Deliberate forward references: preparation must establish hierarchy
        // before any native body or script initialization can observe it.
        document = PrefabDocument{id<ProjectId>(1),
                                  id<AssetId>(20),
                                  actor.id,
                                  {leaf, child, actor},
                                  R"({"source_only":"prefab"})"};
        const AssetMetadata metadata{document->project,
                                     document->asset,
                                     "prefab",
                                     "Shark authored offline acceptance fixture",
                                     "MIT",
                                     "shark.prefab"};
        take(files->write_asset_metadata(
            take(ProjectPath::parse("assets/Actor.sharkprefab.sharkasset")), metadata, {}));
        save();
        write_source("scripts/actor.lua", actor_source(1));
        write_source("scripts/child.lua", R"lua(
            function on_init() shark.log.info('init-child') end
            function on_fixed_update() shark.log.info('fixed-child') end
            function on_destroy() shark.log.info('destroy-child') end
        )lua");
        write_source("scripts/leaf.lua", R"lua(
            function on_init()
                local x,y,z=shark.entity.position()
                assert(x==0 and y==3 and z==0)
                shark.log.info('init-leaf')
            end
            function on_fixed_update() shark.log.info('fixed-leaf') end
            function on_destroy() shark.log.info('destroy-leaf') end
        )lua");
        const auto asset = id<AssetId>(20).value().to_string();
        write_source("scripts/issuer.lua", R"lua(
            local sequence=0
            local west,east
            function on_contact_enter()
                sequence=sequence+1
                west=shark.prefab.spawn(sequence,')lua" +
                                               asset + R"lua(',10,0,0)
                assert(west==shark.prefab.spawn(sequence,')lua" +
                                               asset + R"lua(',10,0,0))
                sequence=sequence+1
                east=shark.prefab.spawn(sequence,')lua" +
                                               asset + R"lua(',20,0,0)
                assert(west~=east)
                assert(#shark.physics.overlap_sphere(10,0,0,1,0,65535,true)==0)
                shark.log.info('contact-issuer')
            end
            function on_fixed_update()
                if west then
                    assert(west==shark.prefab.spawn(sequence-1,')lua" +
                                               asset + R"lua(',10,0,0))
                    assert(east==shark.prefab.spawn(sequence,')lua" +
                                               asset + R"lua(',20,0,0))
                end
            end
        )lua");
        write_source("scripts/sensor.lua", R"lua(
            function on_contact_enter()
                assert(#shark.physics.overlap_sphere(10,0,0,1,0,65535,true)==0)
                assert(#shark.physics.overlap_sphere(20,0,0,1,0,65535,true)==0)
                shark.log.info('contact-sensor')
            end
        )lua");
    }
    ~Archive() {
        files.reset();
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    void save() {
        revision = take(files->write_prefab(take(ProjectPath::parse("assets/Actor.sharkprefab")),
                                            *document, revision))
                       .revision;
    }
    void write_source(const std::string& path, const std::string& source) {
        const auto found = source_revisions.find(path);
        const auto expected = found == source_revisions.end()
                                  ? std::optional<FileRevision>{}
                                  : std::optional<FileRevision>{found->second};
        auto committed = take(files->write_text(take(ProjectPath::parse(path)), source, expected));
        source_revisions.insert_or_assign(path, std::move(committed.revision));
    }
    RuntimePrefab runtime_prefab() {
        const auto observed =
            take(files->read_prefab(take(ProjectPath::parse("assets/Actor.sharkprefab"))));
        CHECK(observed.source.document == *document);
        return take(prepare_runtime_prefab(observed.source.document));
    }
    std::vector<ScriptSourceSnapshot> sources() {
        std::vector<ScriptSourceSnapshot> result;
        for (const auto path : {"scripts/issuer.lua", "scripts/sensor.lua", "scripts/actor.lua",
                                "scripts/child.lua", "scripts/leaf.lua"})
            result.push_back({path, take(files->read_text(take(ProjectPath::parse(path)))).text});
        return result;
    }

    fs::path root;
    std::unique_ptr<ProjectFiles> files;
    std::optional<PrefabDocument> document;
    std::optional<FileRevision> revision;
    std::map<std::string, FileRevision> source_revisions;
};

struct LiveInstance {
    std::shared_ptr<const ScriptEntityMutationReceipt> receipt;
    EntityReference root;
    EntityReference child;
    EntityReference leaf;
    PhysicsBodyHandle body;
};
class Host final {
  public:
    Host(Archive& archive, std::uint64_t epoch, RuntimeSpawnAuthority* old_grant = nullptr) {
        scene = take(Scene::create(region(epoch)));
        EntitySeed issuer_seed{id<WorldEntityId>(100)};
        issuer_seed.name = {"Issuer"};
        issuer_seed.transform.translation.x = 100;
        issuer_seed.rigid_body = RigidBody{};
        issuer_seed.rigid_body->motion = BodyMotion::dynamic_body;
        issuer_seed.rigid_body->is_trigger = true;
        issuer_seed.script = Script{"scripts/issuer.lua", true};
        issuer = take(scene->reference(take(scene->create_entity(issuer_seed))));
        EntitySeed sensor_seed{id<WorldEntityId>(101)};
        sensor_seed.name = {"Sensor"};
        sensor_seed.rigid_body = RigidBody{};
        sensor_seed.script = Script{"scripts/sensor.lua", true};
        sensor = take(scene->reference(take(scene->create_entity(sensor_seed))));
        EntitySeed camera_seed{id<WorldEntityId>(102)};
        camera_seed.camera = Camera{};
        camera_seed.transform.translation = {15, 4, 35};
        camera = take(scene->create_entity(camera_seed));
        PhysicsWorldConfig physics_config;
        physics_config.gravity = {0, 0, 0};
        physics_config.max_bodies = 4;
        physics = take(PhysicsWorld::create(physics_config));
        issuer_body = take(physics->create_body(*scene, issuer->handle));
        take(physics->create_body(*scene, sensor->handle));
        take(physics->begin_simulation(*scene));
        const auto captured = archive.sources();
        ScriptRuntimeConfig config;
        config.profile = ScriptProfile::server_gameplay;
        config.max_scripts = 8; // Two host VMs and exactly two three-VM instances.
        runtime = take(ScriptRuntime::create(*scene, captured, config));
        const std::array catalog{archive.runtime_prefab()};
        if (old_grant)
            CHECK_FALSE(runtime->enable_spawning(*scene, *physics, *old_grant, catalog));
        grant = take(RuntimeSpawnAuthority::create(*scene, region(epoch)));
        take(runtime->enable_spawning(*scene, *physics, *grant, catalog));
        take(runtime->start(*scene, *physics));
        CHECK(take(runtime->take_logs()).empty());
        const auto builtin = take(PreparedAssets::create());
        auto mesh = *take(builtin->find(builtin_cube_asset_id()));
        mesh.asset = id<AssetId>(30);
        const auto bytes = take(encode_prepared_mesh(mesh));
        const std::array inputs{PreparedAssetInput{mesh.asset, bytes}};
        assets = take(PreparedAssets::create(inputs));
    }
    std::vector<LiveInstance> spawn_pair(unsigned version, double child_x = 2) {
        Transform touching;
        take(physics->teleport(*scene, issuer_body, touching));
        take(runtime->fixed_update(*scene, *physics, take(physics->stats()).completed_ticks,
                                   1.0 / 60.0));
        take(physics->step(*scene));
        // A native enter only collects observations. No Lua was called yet.
        CHECK(take(runtime->take_logs()).empty());
        CHECK(take(scene->stats()).live_entities == 3);
        const auto contacts = take(runtime->dispatch_contacts(*scene, *physics));
        REQUIRE_FALSE(contacts.error);
        REQUIRE(contacts.events.size() == 1);
        CHECK(contacts.events.front().phase == PhysicsContactPhase::enter);
        CHECK(contacts.attempted_callbacks == 2);
        const auto logs = take(runtime->take_logs());
        REQUIRE(logs.size() == 8);
        CHECK(logs[0].message == "contact-issuer");
        CHECK(logs[1].message == "contact-sensor");
        for (std::size_t index = 2; index < logs.size(); ++index) {
            CHECK(logs[index].phase == ScriptPhase::init);
            CHECK(logs[index].tick == contacts.events.front().completed_tick);
        }
        const std::vector<std::string> expected{"init-root-v" + std::to_string(version),
                                                "init-child", "init-leaf"};
        for (std::size_t index = 0; index < 6; ++index)
            CHECK(logs[index + 2].message == expected[index % 3]);
        auto outcomes = take(runtime->take_entity_mutations()).outcomes;
        REQUIRE(outcomes.size() == 2);
        std::vector<LiveInstance> result;
        for (std::size_t index = 0; index < outcomes.size(); ++index) {
            const auto& receipt = outcomes[index];
            REQUIRE(receipt->state == ScriptEntityMutationState::applied);
            CHECK(receipt->kind == ScriptEntityMutationKind::spawn);
            CHECK_FALSE(receipt->error);
            CHECK_FALSE(receipt->native_progress_uncertain);
            CHECK(receipt->created_bodies == 1);
            CHECK(receipt->initialized_scripts == 3);
            REQUIRE(receipt->created_entities.size() == 3);
            REQUIRE(receipt->root);
            const auto root = take(scene->reference(take(scene->find(*receipt->root))));
            const auto root_state = take(scene->entity_state(root.handle));
            REQUIRE(root_state.children.size() == 1);
            CHECK(root_state.local_transform.translation.x == (index == 0 ? 10 : 20));
            CHECK(root_state.name == Name{"Actor"});
            const auto child_state = take(scene->entity_state(root_state.children.front()));
            REQUIRE(child_state.children.size() == 1);
            const auto leaf_state = take(scene->entity_state(child_state.children.front()));
            CHECK(child_state.local_transform.translation.x == child_x);
            CHECK(child_state.world_matrix[3][0] == root_state.world_matrix[3][0] + child_x);
            CHECK(leaf_state.local_transform.translation == math::Vec3{0, 3, 0});
            CHECK(leaf_state.world_matrix[3][0] == root_state.world_matrix[3][0] + child_x);
            CHECK(leaf_state.world_matrix[3][1] == 3);
            CHECK(leaf_state.mesh_renderer == MeshRenderer{id<AssetId>(30), 0, true});
            result.push_back({receipt, root, child_state.reference, leaf_state.reference,
                              take(physics->find_body(*scene, root))});
        }
        CHECK(take(scene->stats()).live_entities == 9);
        CHECK(take(physics->stats()).bodies == 4);
        CHECK(take(runtime->stats()).initialized == 8);
        CHECK(take(runtime->take_entity_mutations()).outcomes.empty());
        return result;
    }
    RenderScene render() const {
        const auto current = take(scene->stats());
        RenderExtractionOptions options{camera, {current.instance, current.revision}};
        options.frustum_culling = false;
        return take(extract_render_scene(*scene, *assets, options));
    }
    void leave_sensor() {
        // The caller already ran this tick's fixed callbacks to destroy the
        // actors. Complete that same tick after moving the retained host body.
        Transform apart;
        apart.translation.x = 100;
        take(physics->teleport(*scene, issuer_body, apart));
        take(physics->step(*scene));
        const auto exits = take(runtime->dispatch_contacts(*scene, *physics));
        REQUIRE_FALSE(exits.error);
        REQUIRE(exits.events.size() == 1);
        CHECK(exits.events.front().phase == PhysicsContactPhase::exit);
        CHECK(take(physics->stats()).pending_events == 0);
        CHECK(take(runtime->take_logs()).empty());
    }
    void stop(std::size_t remaining_instances) {
        take(grant->revoke());
        const auto stopped = take(runtime->stop(*scene, *physics));
        CHECK(stopped.errors.empty());
        CHECK(stopped.remaining_spawned_roots == 0);
        CHECK(stopped.removed_spawned_entities == remaining_instances * 3);
        CHECK(stopped.removed_spawned_bodies == remaining_instances);
        CHECK(take(runtime->stats()).initialized == 0);
        CHECK(take(scene->stats()).live_entities == 3);
        CHECK(take(physics->stats()).bodies == 2);
        const auto final_render = render();
        CHECK(final_render.records().empty());
        CHECK(take(runtime->close()) == 0);
        take(physics->close());
        CHECK(take(physics->stats()).bodies == 0);
        CHECK_FALSE(take(physics->stats()).simulating);
    }

    std::unique_ptr<Scene> scene;
    std::optional<EntityReference> issuer;
    std::optional<EntityReference> sensor;
    EntityHandle camera;
    PhysicsBodyHandle issuer_body;
    std::unique_ptr<PhysicsWorld> physics;
    std::unique_ptr<ScriptRuntime> runtime;
    std::unique_ptr<RuntimeSpawnAuthority> grant;
    std::shared_ptr<const PreparedAssets> assets;
};

void check_destroy_order(const std::vector<ScriptLogEntry>& logs, const LiveInstance& instance) {
    const auto position = [&](const EntityReference& entity) {
        const auto found = std::find_if(logs.begin(), logs.end(), [&](const auto& entry) {
            return entry.entity == entity && entry.phase == ScriptPhase::destroy;
        });
        REQUIRE(found != logs.end());
        return std::distance(logs.begin(), found);
    };
    CHECK(position(instance.leaf) < position(instance.child));
    CHECK(position(instance.child) < position(instance.root));
}
} // namespace

TEST_CASE("saved prefab contact instances complete repeated simulation and render lifetimes",
          "[prefab-acceptance]") {
    Archive archive;
    const auto source_before = *archive.document;
    const auto bridged = archive.runtime_prefab();
    REQUIRE(bridged.entities.size() == 3);
    CHECK(bridged.asset == source_before.asset);
    CHECK(bridged.root == source_before.root);
    CHECK(bridged.entities.front().mesh_renderer == source_before.entities.front().mesh_renderer);
    CHECK(bridged.entities.back().rigid_body == source_before.entities.back().rigid_body);
    Host host{archive, 1};
    const auto base_scene = take(host.scene->stats());
    std::set<WorldEntityId> roots;
    std::set<EntityReference> retired;
    std::optional<RenderScene> retained_render;
    std::weak_ptr<const PreparedMeshAsset> mesh = take(host.assets->find(id<AssetId>(30)));
    for (std::uint64_t cycle = 0; cycle < 16; ++cycle) {
        INFO("contact lifetime cycle " << cycle);
        const auto instances = host.spawn_pair(1);
        const auto frame = host.render();
        REQUIRE(frame.records().size() == 2);
        if (cycle == 0) {
            const auto ordinary_write =
                host.scene->set_name(instances.front().root.handle, {"Denied"});
            REQUIRE_FALSE(ordinary_write);
            CHECK(ordinary_write.error().code == ErrorCode::conflict);
        }
        if (cycle == 15)
            retained_render = frame;
        for (const auto& instance : instances) {
            REQUIRE(instance.receipt->root);
            CHECK(roots.insert(*instance.receipt->root).second);
            for (const auto& old : retired)
                CHECK_FALSE(host.scene->resolve(old));
        }
        take(host.runtime->fixed_update(*host.scene, *host.physics,
                                        take(host.physics->stats()).completed_ticks, 1.0 / 60.0));
        const auto updated = take(host.runtime->take_logs());
        REQUIRE(updated.size() == 6);
        CHECK(take(host.runtime->take_entity_mutations()).outcomes.empty());
        // Name remains source content under the physics writer lease. The
        // supported opposite native velocities exercise independent live state.
        CHECK(take(host.scene->entity_state(instances[0].root.handle)).name == Name{"Actor"});
        CHECK(take(host.scene->entity_state(instances[1].root.handle)).name == Name{"Actor"});
        CHECK(take(host.physics->velocity(*host.scene, instances[0].body)).linear.x == 1);
        CHECK(take(host.physics->velocity(*host.scene, instances[1].body)).linear.x == -1);
        take(host.physics->step(*host.scene));
        const auto movement_contacts =
            take(host.runtime->dispatch_contacts(*host.scene, *host.physics));
        CHECK_FALSE(movement_contacts.error);
        CHECK(movement_contacts.events.empty());
        const auto west = take(host.scene->entity_state(instances[0].root.handle));
        const auto east = take(host.scene->entity_state(instances[1].root.handle));
        CHECK(west.local_transform.translation.x > 10);
        CHECK(east.local_transform.translation.x < 20);
        CHECK(take(host.scene->entity_state(instances[0].leaf.handle)).world_matrix[3][0] ==
              west.world_matrix[3][0] + 2);
        CHECK(take(host.scene->entity_state(instances[1].leaf.handle)).world_matrix[3][0] ==
              east.world_matrix[3][0] + 2);
        take(host.runtime->fixed_update(*host.scene, *host.physics,
                                        take(host.physics->stats()).completed_ticks, 1.0 / 60.0));
        const auto destroyed = take(host.runtime->take_logs());
        REQUIRE(destroyed.size() == 12);
        for (const auto& instance : instances)
            check_destroy_order(destroyed, instance);
        const auto outcomes = take(host.runtime->take_entity_mutations()).outcomes;
        REQUIRE(outcomes.size() == 2);
        for (const auto& outcome : outcomes) {
            CHECK(outcome->state == ScriptEntityMutationState::applied);
            CHECK(outcome->kind == ScriptEntityMutationKind::destroy);
            CHECK(outcome->removed_entities.size() == 3);
            CHECK(outcome->removed_bodies == 1);
            CHECK(outcome->destroyed_scripts == 3);
            CHECK(outcome->attempted_destroy_callbacks == 3);
            CHECK_FALSE(outcome->error);
        }
        for (const auto& instance : instances) {
            for (const auto& reference : instance.receipt->created_entities) {
                CHECK_FALSE(host.scene->resolve(reference));
                retired.insert(reference);
            }
            CHECK_FALSE(host.physics->body_state(*host.scene, instance.body));
        }
        CHECK(take(host.scene->stats()).live_entities == base_scene.live_entities);
        CHECK(take(host.physics->stats()).bodies == 2);
        CHECK(take(host.runtime->stats()).initialized == 2);
        const auto empty_frame = host.render();
        CHECK(empty_frame.records().empty());
        CHECK(frame.records().size() == 2); // Historical resources remain owned.
        host.leave_sensor();
    }
    CHECK(roots.size() == 32);
    CHECK(retired.size() == 96);
    CHECK(archive.runtime_prefab() == bridged);
    CHECK(*archive.document == source_before);
    host.stop(0);
    host.assets.reset();
    CHECK_FALSE(mesh.expired());
    retained_render.reset();
    CHECK(mesh.expired());
    take(host.scene->close());
}

TEST_CASE("prefab owner reload captures new saved content and fences old ephemeral identity",
          "[prefab-acceptance]") {
    Archive archive;
    auto invalid = *archive.document;
    invalid.entities.front().parent = id<AuthoredEntityId>(999);
    CHECK_FALSE(prepare_runtime_prefab(invalid));
    std::unique_ptr<Host> previous;
    std::vector<EntityReference> stale;
    std::vector<PhysicsBodyHandle> old_bodies;
    std::vector<std::shared_ptr<const ScriptEntityMutationReceipt>> historical;
    std::set<WorldEntityId> roots;
    for (unsigned generation = 1; generation <= 4; ++generation) {
        INFO("source and runtime reload " << generation);
        const auto expected_child_x = static_cast<double>(generation + 1);
        archive.document->entities[1].transform.translation.x = expected_child_x;
        archive.save();
        archive.write_source("scripts/actor.lua", actor_source(generation));
        auto host =
            std::make_unique<Host>(archive, generation, previous ? previous->grant.get() : nullptr);
        if (previous) {
            CHECK(take(host->scene->stats()).instance != take(previous->scene->stats()).instance);
            CHECK_FALSE(previous->runtime->update(*host->scene, *host->physics, 0.01));
            for (const auto& reference : stale)
                CHECK_FALSE(host->scene->resolve(reference));
            for (const auto body : old_bodies)
                CHECK_FALSE(host->physics->body_state(*host->scene, body));
            for (const auto& receipt : historical) {
                CHECK(receipt->state == ScriptEntityMutationState::applied);
                CHECK(receipt->created_entities.size() == 3);
                for (const auto& reference : receipt->created_entities)
                    CHECK_FALSE(host->scene->resolve(reference));
            }
        }
        // A live owner keeps immutable source/catalog snapshots even when the
        // trusted host later publishes another valid source and script file.
        archive.document->entities[1].transform.translation.x = expected_child_x + 100;
        archive.save();
        archive.write_source("scripts/actor.lua", "error('replacement was not captured')");
        const auto instances = host->spawn_pair(generation, expected_child_x);
        const auto frame = host->render();
        CHECK(frame.records().size() == 2);
        stale.push_back(*host->issuer);
        old_bodies.push_back(host->issuer_body);
        for (const auto& instance : instances) {
            REQUIRE(instance.receipt->root);
            CHECK(roots.insert(*instance.receipt->root).second);
            historical.push_back(instance.receipt);
            stale.insert(stale.end(), instance.receipt->created_entities.begin(),
                         instance.receipt->created_entities.end());
            old_bodies.push_back(instance.body);
            CHECK(instance.root.authority == region(generation).authority);
        }
        host->stop(2);
        const auto stopped_logs = take(host->runtime->take_logs());
        for (const auto& instance : instances)
            check_destroy_order(stopped_logs, instance);
        for (const auto& instance : instances)
            CHECK_FALSE(host->scene->resolve(instance.root));
        take(host->scene->close());
        previous = std::move(host);
    }
    CHECK(roots.size() == 8);
}
