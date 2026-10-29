#include "../editor/gizmo_widget.hpp"
#include "../editor/gui_adapter.hpp"
#include "../editor/viewport_gizmo.hpp"
#include "gpu_shader_fixture.hpp"

#include <shark/tools/project_files.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <imgui.h>

#include <filesystem>
#include <numbers>

namespace {
using namespace shark;
using namespace shark::editor;
using namespace shark::tools;
using namespace shark::client;

template <typename T> T take(Result<T> result) {
    INFO((result ? std::string{} : result.error().message));
    INFO((result ? std::string{} : result.error().context.operation));
    REQUIRE(result);
    return std::move(result).value();
}
void accepted(Result<void> result) {
    REQUIRE(result);
}
void accepted(InputEventResult result) {
    REQUIRE(result == InputEventResult::accepted);
}
StableId stable(std::string_view text) {
    return take(StableId::parse(text));
}

struct ProjectFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
                                 ("shark-gizmo-gui-" + take(generate_stable_id()).to_string());
    AuthoredEntityId camera{stable("17c00000-0000-4000-8000-000000000001")};
    AuthoredEntityId entity{stable("17c00000-0000-4000-8000-000000000002")};
    AuthoredEntityId other{stable("17c00000-0000-4000-8000-000000000003")};
    ProjectFixture() {
        REQUIRE(std::filesystem::create_directory(root));
        auto files = take(ProjectFiles::open(root));
        ProjectDocument project{ProjectId{stable("17c00000-0000-4000-8000-000000000004")},
                                WorldId{stable("17c00000-0000-4000-8000-000000000005")},
                                {"Gizmo GUI"},
                                SceneId{stable("17c00000-0000-4000-8000-000000000006")},
                                {{SceneId{stable("17c00000-0000-4000-8000-000000000006")},
                                  take(ProjectPath::parse("Scene.sharkscene"))}}};
        SceneDocument scene{project.project, project.startup_scene, {}, {}};
        SourceEntity view{camera, {}, {"View"}};
        view.camera = Camera{CameraProjection::orthographic, 1, 8, 1, 10};
        scene.entities.push_back(view);
        SourceEntity selected{entity, {}, {"Selected"}};
        selected.transform.translation = {0, 0, -4};
        selected.mesh_renderer = MeshRenderer{builtin_cube_asset_id()};
        scene.entities.push_back(selected);
        scene.entities.push_back(SourceEntity{other, {}, {"Other"}});
        static_cast<void>(take(files->write_scene(project, scene, {})));
        static_cast<void>(take(
            files->write_project(take(ProjectPath::parse("Project.sharkproject")), project, {})));
    }
    ~ProjectFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};
struct Workspace {
    ProjectFixture fixture;
    std::unique_ptr<ProjectSession> session{
        take(ProjectSession::create(fixture.root, "Project.sharkproject"))};
    ScenePanel panel{*session};
    GizmoWidget widget{panel};
    InputState input;
    Workspace() {
        accepted(panel.refresh());
        accepted(panel.select(fixture.entity));
        accepted(input.on_focus(true));
        accepted(input.on_cursor(340, 260));
    }
    AuthoringViewportRequest request() const {
        AuthoringViewportRequest result{fixture.camera, panel.revision()};
        result.viewport = {640, 360};
        result.target = {"windows-client", AssetAudience::client};
        return result;
    }
    RenderScene snapshot() {
        return take(session->viewport_snapshot(request()));
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
    GizmoFrameResult draw(Workspace& work, const RenderScene& scene, bool blocked = false,
                          const GizmoPreviewValidator& validate = {}) {
        GraphicsWindowFrame frame;
        frame.state.content = {1200, 800};
        frame.state.framebuffer = {2400, 1200};
        frame.input = work.input.capture();
        feed_gui_input(frame, 1.0F / 60.0F);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({80, 60});
        ImGui::SetNextWindowSize({1000, 700});
        ImGui::Begin("Gizmo fixture", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        work.widget.controls(blocked);
        ImGui::SetCursorScreenPos(image_position);
        const auto scale = ImGui::GetIO().DisplayFramebufferScale;
        ImGui::Image(static_cast<ImTextureID>(2), {640.0F / scale.x, 360.0F / scale.y});
        const auto minimum = ImGui::GetItemRectMin();
        const auto maximum = ImGui::GetItemRectMax();
        rectangle = {minimum.x, minimum.y, maximum.x - minimum.x, maximum.y - minimum.y};
        auto result = work.widget.overlay(frame.input, rectangle, scene, ImGui::IsItemHovered(),
                                          blocked, validate);
        ImGui::End();
        ImGui::Render();
        const auto& vertex = shark::test::shaders::scene_vs_words;
        const auto& pixel = shark::test::shaders::scene_ps_words;
        const auto viewport = take(make_scene_viewport_request(
            scene, {ShaderStage::vertex, "vsScene", {vertex.begin(), vertex.end()}},
            {ShaderStage::pixel, "psScene", {pixel.begin(), pixel.end()}}));
        const auto captured =
            take(capture_gui_draw(*ImGui::GetDrawData(), {2400, 1200}, &viewport));
        CHECK_FALSE(captured.draws.empty());
        return result;
    }
    void warm(Workspace& work, const RenderScene& scene) {
        static_cast<void>(draw(work, scene));
        static_cast<void>(draw(work, scene));
        CHECK(rectangle.x == 180);
        CHECK(rectangle.y == 140);
        CHECK(rectangle.width == 320);
        CHECK(rectangle.height == 240);
    }
    ViewportRectangle rectangle;
    ImVec2 image_position{180, 140};

  private:
    ImGuiContext* previous_;
    ImGuiContext* context_;
};

void press(Workspace& work, double x = 380, double y = 260) {
    accepted(work.input.on_cursor(x, y));
    accepted(work.input.on_mouse_button(MouseButton::left, ButtonAction::press));
}
} // namespace

TEST_CASE("Gizmo widget consumes image handles and commits one snapped undo group",
          "[editor-gui]") {
    Workspace work;
    accepted(work.widget.configure(TransformOperation::translate, TransformSpace::world,
                                   {.translation = 1}));
    const auto revision = work.panel.revision();
    const auto scene = work.snapshot();
    HeadlessGui gui;
    accepted(work.input.on_cursor(50, 50));
    gui.warm(work, scene);
    std::size_t previews = 0;
    const GizmoPreviewValidator validate = [&](const AuthoringTransformPreview& preview) {
        auto request = work.request();
        request.transform_preview = preview;
        const auto snapshot = work.session->viewport_snapshot(request);
        if (!snapshot)
            return Result<void>{snapshot.error()};
        CHECK(snapshot.value().revision() == revision);
        REQUIRE(snapshot.value().transform_preview());
        CHECK(snapshot.value().transform_preview()->local == preview.local);
        REQUIRE(snapshot.value().records().size() == 1);
        CHECK(transform_matrices_equivalent(snapshot.value().records()[0].world_matrix,
                                            take(transform_matrix(preview.local))));
        CHECK_FALSE(work.session->state().dirty);
        CHECK(take(work.panel.history(revision)).undo_entries == 0);
        ++previews;
        return Result<void>{};
    };
    // Enter and press in the same frame, after the previous up frame outside
    // the image. A genuine fresh press must not require an extra hovered frame.
    press(work);
    auto result = gui.draw(work, scene, false, validate);
    REQUIRE(result.consumed);
    REQUIRE_FALSE(result.error);
    REQUIRE(work.widget.preview());
    accepted(work.input.on_cursor(402.5, 260));
    result = gui.draw(work, scene, false, validate);
    REQUIRE_FALSE(result.error);
    CHECK(work.widget.preview()->local.translation.x == Catch::Approx(1));
    accepted(work.input.on_cursor(425, 260));
    result = gui.draw(work, scene, false, validate);
    REQUIRE_FALSE(result.error);
    CHECK(work.widget.preview()->local.translation.x == Catch::Approx(2));
    CHECK(work.panel.revision() == revision);
    accepted(work.input.on_mouse_button(MouseButton::left, ButtonAction::release));
    result = gui.draw(work, scene, false, validate);
    REQUIRE(result.consumed);
    REQUIRE(result.receipt);
    CHECK(result.receipt->changed);
    CHECK(previews == 4);
    CHECK_FALSE(work.widget.preview());
    CHECK_FALSE(work.widget.has_changes());
    const auto after = take(work.panel.inspect(work.fixture.entity, work.panel.revision()));
    CHECK(after.transform.translation.x == Catch::Approx(2));
    CHECK(after.transform.translation.y == 0);
    CHECK(after.transform.translation.z == -4);
    const auto history = take(work.panel.history(work.panel.revision()));
    REQUIRE(history.next_undo);
    CHECK(history.undo_entries == 1);
    CHECK(history.next_undo->command_count == 1);
    static_cast<void>(take(work.panel.undo(history.next_undo->ticket, history.revision)));
    CHECK(take(work.panel.inspect(work.fixture.entity, work.panel.revision()))
              .transform.translation.x == 0);
}

TEST_CASE("Gizmo widget cancellation never resumes a held gesture or writes a preview",
          "[editor-gui]") {
    Workspace work;
    accepted(work.widget.configure(TransformOperation::translate, TransformSpace::local));
    const auto scene = work.snapshot();
    const auto revision = work.panel.revision();
    HeadlessGui gui;
    gui.warm(work, scene);
    press(work);
    REQUIRE(gui.draw(work, scene).consumed);
    accepted(work.input.on_cursor(425, 260));
    REQUIRE(gui.draw(work, scene).consumed);
    REQUIRE(work.widget.preview());
    bool blocked = false;
    bool repress = false;
    std::optional<RenderScene> changed_camera;
    SECTION("Escape") {
        accepted(work.input.on_key(Key::escape, ButtonAction::press));
    }
    SECTION("Focus loss") {
        accepted(work.input.on_focus(false));
    }
    SECTION("Modal or text ownership") {
        blocked = true;
    }
    SECTION("Selection away and back") {
        accepted(work.panel.select(work.fixture.other));
        accepted(work.panel.select(work.fixture.entity));
    }
    SECTION("Explicit tab image or close cancellation") {
        work.widget.cancel();
    }
    SECTION("Release and repress between snapshots") {
        accepted(work.input.on_mouse_button(MouseButton::left, ButtonAction::release));
        accepted(work.input.on_mouse_button(MouseButton::left, ButtonAction::press));
        repress = true;
    }
    SECTION("Image relocation") {
        gui.image_position.x += 10;
    }
    SECTION("Display camera changes") {
        auto request = work.request();
        request.view_camera =
            RenderViewCamera{Camera{CameraProjection::orthographic, 1, 9, 1, 10}, math::Mat4{1}};
        changed_camera = take(work.session->viewport_snapshot(request));
    }
    const auto cancelled = gui.draw(work, changed_camera ? *changed_camera : scene, blocked);
    if (repress)
        CHECK(cancelled.consumed);
    CHECK_FALSE(cancelled.receipt);
    CHECK_FALSE(work.widget.preview());
    CHECK_FALSE(work.widget.has_changes());
    CHECK(work.panel.revision() == revision);
    CHECK_FALSE(work.session->state().dirty);
    CHECK(take(work.panel.history(revision)).undo_entries == 0);
    // A held button with no fresh press cannot recreate the discarded intent.
    accepted(work.input.on_focus(true));
    accepted(work.input.on_cursor(430, 260));
    const auto continued = gui.draw(work, scene);
    CHECK_FALSE(continued.receipt);
    CHECK_FALSE(work.widget.preview());
    if (repress) {
        accepted(work.input.on_mouse_button(MouseButton::left, ButtonAction::release));
        CHECK_FALSE(gui.draw(work, scene).receipt);
        press(work);
        const auto restarted = gui.draw(work, scene);
        CHECK(restarted.consumed);
        CHECK_FALSE(restarted.error);
        REQUIRE(work.widget.preview());
        work.widget.cancel();
        CHECK(take(work.panel.history(revision)).undo_entries == 0);
    }
}

TEST_CASE("Gizmo widget rotation rings and scale handles use the shared transform path",
          "[editor-gui]") {
    Workspace work;
    HeadlessGui gui;
    const auto scene = work.snapshot();
    auto operation = TransformOperation::scale;
    tools::TransformSnap snap;
    SECTION("World rotation with degree-equivalent snapping") {
        operation = TransformOperation::rotate;
        snap.rotation_radians = std::numbers::pi / 12;
    }
    SECTION("Local signed scale") {
        snap.scale = .5;
    }
    accepted(work.widget.configure(operation, TransformSpace::local, snap));
    gui.warm(work, scene);
    const auto observation = take(work.panel.observe_transform(*work.panel.selection()));
    const auto geometry = take(
        make_viewport_gizmo(scene, observation, operation, TransformSpace::local, gui.rectangle));
    const auto axis = operation == TransformOperation::rotate ? std::size_t{2} : std::size_t{0};
    REQUIRE(geometry.axes()[axis].visible);
    const auto start =
        geometry.axes()[axis].points[operation == TransformOperation::rotate ? 0 : 1];
    press(work, start.x, start.y);
    REQUIRE_FALSE(gui.draw(work, scene).error);
    if (operation == TransformOperation::rotate) {
        const auto end = geometry.axes()[axis].points[16];
        accepted(work.input.on_cursor(end.x, end.y));
    } else {
        // Two handle lengths back gives a cumulative factor of -1, retaining
        // supported reflection rather than clamping scale to positive values.
        accepted(work.input.on_cursor(start.x - 2 * (start.x - geometry.pivot().x), start.y));
    }
    REQUIRE_FALSE(gui.draw(work, scene).error);
    accepted(work.input.on_mouse_button(MouseButton::left, ButtonAction::release));
    const auto result = gui.draw(work, scene);
    REQUIRE(result.receipt);
    const auto transform =
        take(work.panel.inspect(work.fixture.entity, work.panel.revision())).transform;
    if (operation == TransformOperation::rotate) {
        const auto matrix = take(transform_matrix(transform));
        CHECK(matrix[0][0] == Catch::Approx(0).margin(1e-10));
        CHECK(matrix[0][1] == Catch::Approx(1));
        CHECK(matrix[1][0] == Catch::Approx(-1));
    } else {
        CHECK(transform.scale.x == Catch::Approx(-1));
        CHECK(transform.scale.y == 1);
        CHECK(transform.scale.z == 1);
    }
    CHECK(transform.translation.z == -4);
    CHECK(take(work.panel.history(work.panel.revision())).undo_entries == 1);
}

TEST_CASE("Gizmo widget render rejection and mode changes cannot commit an older candidate",
          "[editor-gui]") {
    Workspace work;
    accepted(work.widget.configure(TransformOperation::translate, TransformSpace::world));
    const auto scene = work.snapshot();
    const auto revision = work.panel.revision();
    HeadlessGui gui;
    gui.warm(work, scene);
    press(work);
    REQUIRE_FALSE(gui.draw(work, scene).error);
    accepted(work.input.on_cursor(425, 260));
    REQUIRE_FALSE(gui.draw(work, scene).error);
    REQUIRE(work.widget.preview());
    SECTION("Rejected render validation at release") {
        accepted(work.input.on_mouse_button(MouseButton::left, ButtonAction::release));
        const auto result = gui.draw(work, scene, false, [](const AuthoringTransformPreview&) {
            return Result<void>{Error{ErrorCode::invalid_argument,
                                      "Render candidate rejected",
                                      {.operation = "test.gizmo.render"}}};
        });
        CHECK(result.consumed);
        REQUIRE(result.error);
        CHECK(result.error->context.operation == "test.gizmo.render");
        CHECK_FALSE(result.receipt);
    }
    SECTION("Changing tool discards the previous gesture") {
        accepted(work.widget.configure(TransformOperation::scale, TransformSpace::local));
        accepted(work.input.on_mouse_button(MouseButton::left, ButtonAction::release));
        CHECK_FALSE(gui.draw(work, scene).receipt);
    }
    CHECK_FALSE(work.widget.preview());
    CHECK_FALSE(work.widget.has_changes());
    CHECK(work.panel.revision() == revision);
    CHECK_FALSE(work.session->state().dirty);
    CHECK(take(work.panel.history(revision)).undo_entries == 0);
}
