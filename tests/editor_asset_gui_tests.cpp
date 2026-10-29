#include "../editor/asset_widget.hpp"
#include "../editor/console_widget.hpp"
#include "../editor/gui_adapter.hpp"

#include <shark/tools/project_files.hpp>

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {
using namespace shark;
using namespace shark::editor;
using namespace shark::tools;

template <typename T> T take(Result<T> value) {
    REQUIRE(value);
    return std::move(value).value();
}
StableId identity(std::string_view value) {
    return take(StableId::parse(value));
}
struct ProjectFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
                                 ("shark-assets-gui-" + take(generate_stable_id()).to_string());
    AssetId asset{identity("16d00000-0000-4000-8000-000000000006")};
    ProjectFixture() {
        REQUIRE(std::filesystem::create_directory(root));
        auto files = take(ProjectFiles::open(root));
        const SceneId scene_id{identity("16d00000-0000-4000-8000-000000000005")};
        ProjectDocument project{ProjectId{identity("16d00000-0000-4000-8000-000000000003")},
                                WorldId{identity("16d00000-0000-4000-8000-000000000004")},
                                {"Assets GUI"},
                                scene_id,
                                {{scene_id, take(ProjectPath::parse("Scene.sharkscene"))}}};
        SceneDocument scene{project.project, project.startup_scene, {}, {}};
        static_cast<void>(take(files->write_scene(project, scene, std::nullopt)));
        static_cast<void>(take(files->write_project(
            take(ProjectPath::parse("Project.sharkproject")), project, std::nullopt)));
        std::ofstream source(root / "Literal##asset.txt", std::ios::binary);
        source << "Independently authored GUI fixture bytes";
        source.close();
        REQUIRE(source.good());
        AssetMetadata metadata{project.project,
                               asset,
                               "bytes",
                               "Shark authored offline test",
                               "MIT",
                               "shark.bytes",
                               AssetAudience::shared};
        static_cast<void>(take(files->write_asset_metadata(
            take(ProjectPath::parse("Literal##asset.txt.sharkasset")), metadata, std::nullopt)));
    }
    ~ProjectFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

class HeadlessGui {
  public:
    HeadlessGui() : previous_(ImGui::GetCurrentContext()), context_(ImGui::CreateContext()) {
        REQUIRE(context_);
        ImGui::SetCurrentContext(context_);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        REQUIRE(pixels);
        io.Fonts->SetTexID(static_cast<ImTextureID>(1));
    }
    ~HeadlessGui() {
        ImGui::DestroyContext(context_);
        ImGui::SetCurrentContext(previous_);
    }
    void draw(AssetWidget& widget, bool blocked, ProjectSession* console_session = nullptr) {
        client::GraphicsWindowFrame frame;
        frame.state.content = {1000, 1600};
        frame.state.framebuffer = {1000, 1600};
        frame.input.focused = true;
        feed_gui_input(frame, 1.0F / 60.0F);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({1000, 1600});
        ImGui::Begin("Assets", nullptr, ImGuiWindowFlags_NoSavedSettings);
        CHECK_FALSE(widget.draw(blocked));
        ImGui::End();
        if (console_session) {
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({1000, 1600});
            ImGui::Begin("Console", nullptr, ImGuiWindowFlags_NoSavedSettings);
            console_.draw(*console_session);
            ImGui::End();
        }
        ImGui::Render();
        const auto frame_data =
            take(capture_gui_draw(*ImGui::GetDrawData(), frame.state.framebuffer));
        CHECK_FALSE(frame_data.draws.empty());
    }

  private:
    ImGuiContext* previous_;
    ImGuiContext* context_;
    ConsoleWidget console_;
};
} // namespace

TEST_CASE("Asset widget observation neither publishes ready jobs nor acknowledges terminal results",
          "[editor-gui]") {
    ProjectFixture fixture;
    auto session = take(ProjectSession::create(fixture.root, "Project.sharkproject"));
    AssetPanel panel{*session};
    const std::array<std::string_view, 1> inventory{"Literal##asset.txt.sharkasset"};
    REQUIRE(panel.refresh_catalog());
    REQUIRE(panel.refresh_inventory(inventory));
    REQUIRE(panel.refresh_catalog());
    REQUIRE(panel.refresh_cache());
    REQUIRE(panel.refresh_importers());
    REQUIRE(panel.select(fixture.asset));
    const auto revision = session->state().revision;
    const auto job = take(panel.submit_selected({"windows-client", AssetAudience::client}));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    for (;;) {
        REQUIRE(panel.refresh_jobs());
        REQUIRE(panel.jobs().size() == 1);
        if (panel.jobs()[0].state == AssetImportJobState::ready)
            break;
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    AssetWidget widget{panel, *session};
    HeadlessGui gui;
    gui.draw(widget, true);
    gui.draw(widget, false, session.get());
    REQUIRE(panel.refresh_jobs());
    REQUIRE(panel.jobs().size() == 1);
    CHECK(panel.jobs()[0].state == AssetImportJobState::ready);
    REQUIRE(panel.refresh_cache());
    REQUIRE(panel.cache());
    CHECK(panel.cache()->bindings == 0);
    CHECK_FALSE(widget.has_changes());
    CHECK_FALSE(session->state().dirty);
    CHECK(session->state().revision == revision);

    const auto drained = take(panel.drain());
    REQUIRE(drained.drained == 1);
    REQUIRE(panel.refresh_jobs());
    REQUIRE(panel.jobs().size() == 1);
    CHECK(panel.jobs()[0].state == AssetImportJobState::succeeded);
    gui.draw(widget, false);
    REQUIRE(panel.refresh_jobs());
    CHECK(panel.jobs().size() == 1);
    CHECK(panel.jobs()[0].id == job.id);
    const auto acknowledged = take(panel.take_job(job.id));
    CHECK(acknowledged.state == AssetImportJobState::succeeded);
    REQUIRE(widget.discard_draft());
    CHECK_FALSE(session->state().dirty);
    CHECK(session->state().revision == revision);
}
