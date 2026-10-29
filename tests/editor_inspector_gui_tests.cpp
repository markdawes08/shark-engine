#include "../editor/gui_adapter.hpp"
#include "../editor/inspector_widget.hpp"

#include <shark/tools/project_files.hpp>

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>

#include <filesystem>

namespace {
using namespace shark;
using namespace shark::editor;
using namespace shark::tools;

template <typename T> T take(Result<T> result) {
    REQUIRE(result);
    return std::move(result).value();
}
StableId id(std::string_view value) {
    return take(StableId::parse(value));
}

struct ProjectFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
                                 ("shark-inspector-gui-" + take(generate_stable_id()).to_string());
    AuthoredEntityId first{id("16c00000-0000-4000-8000-000000000001")};
    AuthoredEntityId second{id("16c00000-0000-4000-8000-000000000002")};
    ProjectFixture() {
        REQUIRE(std::filesystem::create_directory(root));
        auto files = take(ProjectFiles::open(root));
        ProjectDocument project{ProjectId{id("16c00000-0000-4000-8000-000000000003")},
                                WorldId{id("16c00000-0000-4000-8000-000000000004")},
                                {"Inspector GUI"},
                                SceneId{id("16c00000-0000-4000-8000-000000000005")},
                                {{SceneId{id("16c00000-0000-4000-8000-000000000005")},
                                  take(ProjectPath::parse("Scene.sharkscene"))}}};
        SceneDocument scene{project.project, project.startup_scene, {}, {}};
        SourceEntity value{first, {}, {"Original"}};
        value.camera = Camera{};
        value.mesh_renderer =
            MeshRenderer{AssetId{id("16c00000-0000-4000-8000-000000000006")}, 4, true};
        scene.entities.push_back(std::move(value));
        scene.entities.push_back(SourceEntity{second, {}, {"Second"}});
        static_cast<void>(take(files->write_scene(project, scene, std::nullopt)));
        static_cast<void>(take(files->write_project(
            take(ProjectPath::parse("Project.sharkproject")), project, std::nullopt)));
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
    void draw(InspectorWidget& widget, bool blocked) {
        client::GraphicsWindowFrame frame;
        frame.state.content = {700, 1000};
        frame.state.framebuffer = {700, 1000};
        frame.input.focused = true;
        feed_gui_input(frame, 1.0F / 60.0F);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({700, 1000});
        ImGui::Begin("Inspector", nullptr, ImGuiWindowFlags_NoSavedSettings);
        CHECK_FALSE(widget.draw(blocked));
        ImGui::End();
        ImGui::Render();
        const auto captured = take(capture_gui_draw(*ImGui::GetDrawData(), {700, 1000}));
        CHECK_FALSE(captured.draws.empty());
    }

  private:
    ImGuiContext* previous_;
    ImGuiContext* context_;
};
} // namespace

TEST_CASE("Inspector widget preserves blocked and stale drafts without touching the scene",
          "[editor-gui]") {
    ProjectFixture fixture;
    auto session = take(ProjectSession::create(fixture.root, "Project.sharkproject"));
    ScenePanel scene{*session};
    REQUIRE(scene.refresh());
    REQUIRE(scene.select(fixture.first));
    InspectorPanel inspector{scene};
    REQUIRE(inspector.refresh());
    inspector.draft()->name.value = "Draft ## literal";
    inspector.draft()->transform.scale = {-2, 3, 4};
    InspectorWidget widget{inspector, scene};
    const auto revision = scene.revision();
    const auto history = take(scene.history(revision));
    HeadlessGui gui;
    gui.draw(widget, true);
    CHECK(widget.has_changes());
    CHECK(inspector.draft()->name.value == "Draft ## literal");
    CHECK_FALSE(session->state().dirty);
    CHECK(scene.revision() == revision);
    CHECK(take(scene.inspect(fixture.first, revision)).name.value == "Original");
    REQUIRE(scene.select(fixture.second));
    gui.draw(widget, false);
    CHECK(inspector.stale());
    CHECK(inspector.draft()->selection.id == fixture.first);
    CHECK(inspector.draft()->name.value == "Draft ## literal");
    CHECK(take(scene.history(revision)).undo_entries == history.undo_entries);
    CHECK_FALSE(session->state().dirty);
    widget.discard_draft();
    CHECK_FALSE(widget.has_changes());
    CHECK_FALSE(inspector.draft());
    gui.draw(widget, false);
    CHECK_FALSE(session->state().dirty);
}
