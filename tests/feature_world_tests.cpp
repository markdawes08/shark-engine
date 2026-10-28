#include "shark/tools/feature_world.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace {
using namespace shark;
using namespace shark::tools;
namespace fs = std::filesystem;

std::string read(const fs::path& path) {
    std::ifstream stream{path, std::ios::binary};
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}
struct Fixture {
    fs::path scratch;
    ProjectDocument project;
    SceneDocument scene;
    Fixture()
        : project(decode_project_document(
                      read(fs::path{SHARK_FEATUREWORLD_DIRECTORY} / "FeatureWorld.sharkproject"))
                      .value()
                      .document),
          scene(decode_scene_document(
                    read(fs::path{SHARK_FEATUREWORLD_DIRECTORY} / "scenes/FeatureWorld.sharkscene"))
                    .value()
                    .document) {
        auto identity = generate_stable_id();
        REQUIRE(identity);
        scratch = fs::path{SHARK_FEATUREWORLD_SCRATCH} /
                  ("featureworld-test-" + identity.value().to_string());
        REQUIRE(fs::create_directory(scratch));
    }
    ~Fixture() {
        // Cases clean their own exact children. Preserve unexpected remnants.
        std::error_code ignored;
        fs::remove(scratch, ignored);
    }
};
SceneRunConfig config() {
    SceneRunConfig value;
    value.ticks = 3;
    value.tick_rate_hz = 20;
    value.revision = "featureworld-test";
    return value;
}
} // namespace

TEST_CASE("Saved FeatureWorld connects all current scene surfaces through the native runner",
          "[featureworld]") {
    Fixture fixture;
    const auto cases = feature_world_cases(fixture.scratch);
    const auto run = run_scene_cases(fixture.project, fixture.scene, config(), cases);
    REQUIRE(run);
    for (const auto& result : run.value().cases) {
        INFO(result.id);
        if (result.failure)
            INFO(result.failure->error.message);
        REQUIRE(result.status == SceneRunStatus::passed);
        REQUIRE(result.ticks_completed == 3);
        REQUIRE(result.assertions >= 10);
        REQUIRE(result.initial_revision->revision == 4);
    }
    REQUIRE(run.value().status == SceneRunStatus::passed);
    REQUIRE(run.value().cases.size() == 17);
    REQUIRE(run.value().cases[0].final_revision->revision == 4);
    REQUIRE(run.value().cases[1].final_revision->revision == 11);
    REQUIRE(run.value().cases[2].final_revision->revision == 7);
    REQUIRE(run.value().cases[3].final_revision->revision == 6);
    REQUIRE(run.value().cases[4].final_revision->revision == 4);
    REQUIRE(run.value().cases[5].final_revision == run.value().cases[5].initial_revision);
    REQUIRE(run.value().cases[6].final_revision == run.value().cases[6].initial_revision);
    REQUIRE(run.value().cases[7].final_revision == run.value().cases[7].initial_revision);
    REQUIRE(run.value().cases[8].final_revision == run.value().cases[8].initial_revision);
    REQUIRE(run.value().cases[9].final_revision == run.value().cases[9].initial_revision);
    REQUIRE(run.value().cases[10].final_revision == run.value().cases[10].initial_revision);
    REQUIRE(run.value().cases[11].final_revision == run.value().cases[11].initial_revision);
    REQUIRE(run.value().cases[12].final_revision == run.value().cases[12].initial_revision);
    REQUIRE(run.value().cases[13].final_revision->revision == 4);
    REQUIRE(run.value().cases[14].final_revision == run.value().cases[14].initial_revision);
    REQUIRE(run.value().cases[15].final_revision == run.value().cases[15].initial_revision);
    REQUIRE(run.value().cases[16].final_revision == run.value().cases[16].initial_revision);
    REQUIRE(run.value().cases[13].final_revision->scene_instance !=
            run.value().cases[13].initial_revision->scene_instance);
    REQUIRE(run.value().cases[4].final_revision->scene_instance !=
            run.value().cases[4].initial_revision->scene_instance);
    REQUIRE(encode_scene_run_report(run.value()));
    REQUIRE(fs::is_empty(fixture.scratch));
}

TEST_CASE("FeatureWorld catalog and callbacks can be reused with fresh case owners",
          "[featureworld]") {
    Fixture fixture;
    const auto catalog = feature_world_case_ids();
    const auto cases = feature_world_cases(fixture.scratch);
    REQUIRE(catalog.size() == cases.size());
    std::set<std::string> unique;
    for (std::size_t index = 0; index < catalog.size(); ++index) {
        REQUIRE(cases[index].id == catalog[index]);
        REQUIRE(unique.insert(cases[index].id).second);
    }
    const auto first = run_scene_cases(fixture.project, fixture.scene, config(), cases);
    const auto second = run_scene_cases(fixture.project, fixture.scene, config(), cases);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(first.value().status == SceneRunStatus::passed);
    REQUIRE(second.value().status == SceneRunStatus::passed);
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto& a = first.value().cases[index];
        const auto& b = second.value().cases[index];
        REQUIRE(a.assertions == b.assertions);
        REQUIRE(a.final_revision->revision == b.final_revision->revision);
        REQUIRE(a.initial_revision->scene_instance != b.initial_revision->scene_instance);
    }
    REQUIRE(fs::is_empty(fixture.scratch));
}

TEST_CASE("FeatureWorld rejects valid source drift before exercising or saving it",
          "[featureworld]") {
    Fixture fixture;
    fixture.scene.entities[2].name = Name{"unexpected authored edit"};
    const auto run = run_scene_cases(fixture.project, fixture.scene, config(),
                                     feature_world_cases(fixture.scratch));
    REQUIRE(run);
    REQUIRE(run.value().status == SceneRunStatus::failed);
    for (const auto& result : run.value().cases) {
        REQUIRE(result.failure);
        REQUIRE(result.failure->kind == SceneRunFailureKind::assertion);
        REQUIRE(result.failure->phase == SceneRunPhase::setup);
        REQUIRE(result.ticks_completed == 0);
        REQUIRE(result.initial_revision == result.final_revision);
    }
    REQUIRE(fs::is_empty(fixture.scratch));
}

TEST_CASE(
    "FeatureWorld persistence failure preserves the baseline and an unrelated scratch sentinel",
    "[featureworld]") {
    Fixture fixture;
    const auto sentinel = fixture.scratch / "keep.txt";
    {
        std::ofstream stream{sentinel};
        stream << "keep";
    }
    auto cases = feature_world_cases(sentinel); // Existing file is not a scratch directory.
    const std::array selected{cases[4]};
    const auto run = run_scene_cases(fixture.project, fixture.scene, config(), selected);
    REQUIRE(run);
    REQUIRE(run.value().status == SceneRunStatus::failed);
    const auto& result = run.value().cases.front();
    REQUIRE(result.failure);
    REQUIRE(result.failure->phase == SceneRunPhase::tick);
    REQUIRE(result.failure->tick == 0);
    REQUIRE(result.initial_revision == result.final_revision);
    REQUIRE(read(sentinel) == "keep");
    const std::array pure_texture{cases[11]};
    const auto texture_run =
        run_scene_cases(fixture.project, fixture.scene, config(), pure_texture);
    REQUIRE(texture_run);
    REQUIRE(texture_run.value().status == SceneRunStatus::passed);
    REQUIRE(texture_run.value().cases.front().initial_revision ==
            texture_run.value().cases.front().final_revision);
    REQUIRE(read(sentinel) == "keep");
    REQUIRE(fs::remove(sentinel));
    REQUIRE(fs::is_empty(fixture.scratch));
}

TEST_CASE("FeatureWorld source keeps explicit versions stable IDs global anchor and hierarchy",
          "[featureworld]") {
    Fixture fixture;
    REQUIRE(fixture.project.project.value().to_string() == "20000000-0000-4000-8000-000000000001");
    REQUIRE(fixture.scene.scene == fixture.project.startup_scene);
    REQUIRE(fixture.scene.world_origin);
    REQUIRE(fixture.scene.world_origin->cell.x == 1234567890);
    REQUIRE(fixture.scene.world_origin->offset[1] == 1023.875);
    REQUIRE(fixture.scene.entities.size() == 4);
    REQUIRE(fixture.scene.entities[0].camera == Camera{});
    REQUIRE(fixture.scene.entities[3].mesh_renderer);
    REQUIRE(fixture.scene.entities[3].mesh_renderer->asset.value().to_string() ==
            "00000000-0000-4000-8000-000000000013");
    REQUIRE(fixture.scene.entities[3].mesh_renderer->mesh_index == 0);
    REQUIRE(fixture.scene.entities[3].mesh_renderer->visible);
    REQUIRE(fixture.scene.entities[2].parent == fixture.scene.entities[1].id);
    const auto project = encode_project_document(fixture.project);
    const auto scene = encode_scene_document(fixture.scene);
    REQUIRE(project);
    REQUIRE(scene);
    REQUIRE(decode_project_document(project.value()).value().document == fixture.project);
    REQUIRE(decode_scene_document(scene.value()).value().document == fixture.scene);
    fixture.scene.entities[0].parent = fixture.scene.entities[2].id;
    const auto invalid = run_scene_cases(fixture.project, fixture.scene, config(),
                                         feature_world_cases(fixture.scratch));
    REQUIRE_FALSE(invalid);
    REQUIRE(fs::is_empty(fixture.scratch));
}
