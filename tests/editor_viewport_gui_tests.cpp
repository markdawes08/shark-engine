#include "../editor/gui_adapter.hpp"
#include "../editor/workspace_panel.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>

#include <limits>

namespace {
using namespace shark;
using namespace shark::client;
using namespace shark::editor;

void accepted(InputEventResult result) {
    REQUIRE(result == InputEventResult::accepted);
}
bool stationary(const ViewportPointerActions& value) {
    return value.motion.orbit_x == 0 && value.motion.orbit_y == 0 && value.motion.pan_x == 0 &&
           value.motion.pan_y == 0 && value.motion.dolly == 0;
}

class ImageContext final {
  public:
    ImageContext() : previous_(ImGui::GetCurrentContext()), context_(ImGui::CreateContext()) {
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
    ~ImageContext() {
        ImGui::DestroyContext(context_);
        ImGui::SetCurrentContext(previous_);
    }
    ImageContext(const ImageContext&) = delete;
    ImageContext& operator=(const ImageContext&) = delete;

    ViewportPointerActions draw(ViewportPointerRouter& router, InputSnapshot input) {
        GraphicsWindowFrame frame;
        frame.state.content = {1200, 800};
        frame.state.framebuffer = {2400, 1200};
        frame.input = input;
        feed_gui_input(frame, 1.0F / 60.0F);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({80, 60});
        ImGui::SetNextWindowSize({1000, 700});
        ImGui::Begin("Viewport rectangle fixture", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetCursorScreenPos({180, 140});
        const auto scale = ImGui::GetIO().DisplayFramebufferScale;
        // The allocated 640x360 target, not the larger available panel, defines
        // this image. Input remains logical coordinates with anisotropic DPI.
        ImGui::Image(static_cast<ImTextureID>(2), {640.0F / scale.x, 360.0F / scale.y});
        const auto minimum = ImGui::GetItemRectMin();
        const auto maximum = ImGui::GetItemRectMax();
        rectangle = {minimum.x, minimum.y, maximum.x - minimum.x, maximum.y - minimum.y};
        const auto actions = router.route(input, rectangle, ImGui::IsItemHovered(), false);
        ImGui::End();
        ImGui::Render();
        return actions;
    }

    ViewportRectangle rectangle;

  private:
    ImGuiContext* previous_;
    ImGuiContext* context_;
};
} // namespace

TEST_CASE("Viewport pointer routing cancels capture boundaries and requires fresh inside presses",
          "[editor-gui]") {
    const ViewportRectangle image{100, 200, 400, 300};
    ViewportPointerRouter router;
    InputState input;
    accepted(input.on_focus(true));
    accepted(input.on_cursor(150, 250));
    accepted(input.on_mouse_button(MouseButton::right, ButtonAction::press));
    CHECK(stationary(router.route(input.capture(), image, true, false)));
    accepted(input.on_cursor(170, 240));
    auto actions = router.route(input.capture(), image, true, false);
    CHECK(actions.motion.orbit_x == 20);
    CHECK(actions.motion.orbit_y == -10);
    CHECK_FALSE(actions.pick);

    // The active image is a half-open rectangle; returning with a held button
    // cannot restart a drag that left it.
    accepted(input.on_cursor(500, 240));
    CHECK(stationary(router.route(input.capture(), image, true, false)));
    accepted(input.on_cursor(170, 240));
    CHECK(stationary(router.route(input.capture(), image, true, false)));
    accepted(input.on_mouse_button(MouseButton::right, ButtonAction::release));
    static_cast<void>(router.route(input.capture(), image, true, false));
    accepted(input.on_mouse_button(MouseButton::middle, ButtonAction::press));
    CHECK(stationary(router.route(input.capture(), image, true, false)));
    accepted(input.on_cursor(175, 255));
    actions = router.route(input.capture(), image, true, false);
    CHECK(actions.motion.pan_x == 5);
    CHECK(actions.motion.pan_y == 15);

    // Modal/text blocking, image relocation, Escape, focus loss and explicit
    // tab/camera cancellation all stop the same local pointer interaction.
    CHECK(stationary(router.route(input.capture(), image, true, true)));
    accepted(input.on_cursor(180, 260));
    CHECK(stationary(router.route(input.capture(), image, true, false)));
    accepted(input.on_mouse_button(MouseButton::middle, ButtonAction::release));
    static_cast<void>(router.route(input.capture(), image, true, false));
    accepted(input.on_mouse_button(MouseButton::middle, ButtonAction::press));
    static_cast<void>(router.route(input.capture(), image, true, false));
    const ViewportRectangle moved{110, 200, 400, 300};
    CHECK(stationary(router.route(input.capture(), moved, true, false)));
    accepted(input.on_cursor(190, 270));
    CHECK(stationary(router.route(input.capture(), moved, true, false)));
    accepted(input.on_key(Key::escape, ButtonAction::press));
    CHECK(stationary(router.route(input.capture(), moved, true, false)));
    accepted(input.on_key(Key::escape, ButtonAction::release));
    accepted(input.on_focus(false));
    CHECK(stationary(router.route(input.capture(), moved, true, false)));
    accepted(input.on_focus(true));
    accepted(input.on_cursor(200, 280));
    accepted(input.on_mouse_button(MouseButton::right, ButtonAction::press));
    static_cast<void>(router.route(input.capture(), moved, true, false));
    router.cancel();
    accepted(input.on_cursor(220, 290));
    CHECK(stationary(router.route(input.capture(), moved, true, false)));
    accepted(input.on_mouse_button(MouseButton::right, ButtonAction::release));
    static_cast<void>(router.route(input.capture(), moved, true, false));
    accepted(input.on_scroll(0, 0.5));
    CHECK(router.route(input.capture(), moved, true, false).motion.dolly == 0.5);
    accepted(input.on_scroll(0, 1));
    CHECK(stationary(router.route(input.capture(), moved, false, false)));
    auto invalid = input.capture();
    invalid.cursor.x = std::numeric_limits<double>::infinity();
    CHECK(stationary(router.route(invalid, moved, true, false)));
}

TEST_CASE("Viewport image routing uses actual logical image bounds at nonuniform DPI",
          "[editor-gui]") {
    ImageContext gui;
    ViewportPointerRouter router;
    InputState input;
    accepted(input.on_focus(true));
    accepted(input.on_cursor(340, 260));
    static_cast<void>(gui.draw(router, input.capture()));
    accepted(input.on_mouse_button(MouseButton::left, ButtonAction::press));
    accepted(input.on_mouse_button(MouseButton::left, ButtonAction::release));
    const auto selected = gui.draw(router, input.capture());
    CHECK(gui.rectangle.x == 180);
    CHECK(gui.rectangle.y == 140);
    CHECK(gui.rectangle.width == 320);
    CHECK(gui.rectangle.height == 240);
    CHECK(selected.pick);
    const auto uv = viewport_coordinates(gui.rectangle, selected.x, selected.y);
    REQUIRE(uv);
    REQUIRE(uv.value());
    CHECK(uv.value()->u == 0.5);
    CHECK(uv.value()->v == 0.5);
    static_cast<void>(gui.draw(router, input.capture()));
    accepted(input.on_cursor(600, 260));
    accepted(input.on_mouse_button(MouseButton::left, ButtonAction::press));
    CHECK_FALSE(gui.draw(router, input.capture()).pick);
}
