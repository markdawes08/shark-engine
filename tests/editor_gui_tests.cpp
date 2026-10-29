#include "../editor/gui_adapter.hpp"
#include "gpu_shader_fixture.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace shark;
using namespace shark::client;
using namespace shark::editor;

template <typename T> T take(Result<T> result) {
    REQUIRE(result);
    return std::move(result).value();
}

void accepted(InputEventResult result) {
    REQUIRE(result == InputEventResult::accepted);
}

struct Controls {
    ImVec2 field;
    ImVec2 close;
    bool field_active;
};

class Gui final {
  public:
    Gui() : previous_(ImGui::GetCurrentContext()), context_(ImGui::CreateContext()) {
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
        REQUIRE(width > 0);
        REQUIRE(height > 0);
        io.Fonts->SetTexID(static_cast<ImTextureID>(1));
    }
    ~Gui() {
        ImGui::DestroyContext(context_);
        ImGui::SetCurrentContext(previous_);
    }
    Gui(const Gui&) = delete;
    Gui& operator=(const Gui&) = delete;

    void begin(InputSnapshot input, WindowExtent content = {640, 480},
               WindowExtent framebuffer = {640, 480}) {
        GraphicsWindowFrame frame;
        frame.state.content = content;
        frame.state.framebuffer = framebuffer;
        frame.input = std::move(input);
        feed_gui_input(frame, 1.0F / 60.0F);
        ImGui::NewFrame();
    }

    void text(std::array<char, 64>& value, bool focus = false) {
        ImGui::SetNextWindowPos({10, 10});
        ImGui::SetNextWindowSize({300, 180});
        ImGui::Begin("Text fixture", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        if (focus)
            ImGui::SetKeyboardFocusHere();
        ImGui::InputText("Name", value.data(), value.size());
        ImGui::End();
        ImGui::Render();
    }

    Controls controls(std::array<char, 64>& value, bool& open) {
        const auto center = [] {
            const auto minimum = ImGui::GetItemRectMin();
            const auto maximum = ImGui::GetItemRectMax();
            return ImVec2{(minimum.x + maximum.x) * 0.5F, (minimum.y + maximum.y) * 0.5F};
        };
        ImGui::SetNextWindowPos({10, 10});
        ImGui::SetNextWindowSize({300, 180});
        ImGui::Begin("Project controls fixture", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::BeginChild("Project controls", {0, 0}, ImGuiChildFlags_Borders);
        ImGui::BeginDisabled(open);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##project-file", value.data(), value.size());
        Controls observed{center(), {}, ImGui::IsItemActive()};
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!open);
        if (ImGui::Button("Close project"))
            open = false;
        observed.close = center();
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::End();
        ImGui::Render();
        return observed;
    }

    ImDrawData& triangle() {
        InputSnapshot input;
        input.focused = true;
        begin(input, {80, 40}, {160, 120});
        auto* list = ImGui::GetForegroundDrawList();
        list->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        list->PushClipRect({9.8F, 21.25F}, {55.1F, 60.2F}, false);
        list->AddTriangleFilled({12.25F, 23.5F}, {50.0F, 23.5F}, {12.25F, 50.0F},
                                IM_COL32(128, 64, 0, 128));
        list->PopClipRect();
        ImGui::Render();
        auto* data = ImGui::GetDrawData();
        REQUIRE(data);
        REQUIRE(data->CmdListsCount == 1);
        REQUIRE(data->TotalVtxCount == 3);
        REQUIRE(data->TotalIdxCount == 3);
        REQUIRE(data->CmdLists[0]->CmdBuffer.Size == 1);
        data->DisplayPos = {10, 20};
        return *data;
    }

  private:
    ImGuiContext* previous_;
    ImGuiContext* context_;
};
} // namespace

TEST_CASE("Editor input edits Unicode text and cancels queued taps at focus loss", "[editor-gui]") {
    Gui gui;
    InputState input;
    accepted(input.on_focus(true));
    accepted(input.on_cursor(24, 24));
    std::array<char, 64> value{};
    gui.begin(input.capture());
    gui.text(value, true);
    gui.begin(input.capture());
    gui.text(value);
    accepted(input.on_text(U'A'));
    accepted(input.on_text(U'\u96ea'));
    gui.begin(input.capture());
    gui.text(value);
    CHECK(std::string{value.data()} == "A\xe9\x9b\xaa");

    accepted(input.on_cursor(400, 300));
    accepted(input.on_mouse_button(MouseButton::left, ButtonAction::press));
    accepted(input.on_mouse_button(MouseButton::left, ButtonAction::release));
    gui.begin(input.capture());
    CHECK(ImGui::IsMouseClicked(ImGuiMouseButton_Left));
    CHECK(ImGui::GetIO().MousePos.x == 400.0F);
    CHECK(ImGui::GetIO().MousePos.y == 300.0F);
    ImGui::Render();
    gui.begin(input.capture());
    CHECK(ImGui::IsMouseReleased(ImGuiMouseButton_Left));
    CHECK_FALSE(ImGui::IsMouseDown(ImGuiMouseButton_Left));
    ImGui::Render();

    accepted(input.on_key(Key::left_control, ButtonAction::press));
    accepted(input.on_key(Key::s, ButtonAction::press));
    accepted(input.on_mouse_button(MouseButton::left, ButtonAction::press));
    gui.begin(input.capture());
    CHECK(ImGui::IsKeyDown(ImGuiKey_S));
    CHECK(ImGui::GetIO().KeyCtrl);
    CHECK(ImGui::IsMouseDown(ImGuiMouseButton_Left));
    ImGui::Render();

    // Release/press coalesces to held=true. ImGui trickles the new press into
    // its next frame; Shark focus loss must cancel that still-queued positive.
    accepted(input.on_mouse_button(MouseButton::left, ButtonAction::release));
    accepted(input.on_mouse_button(MouseButton::left, ButtonAction::press));
    gui.begin(input.capture());
    REQUIRE(ImGui::IsMouseReleased(ImGuiMouseButton_Left));
    ImGui::Render();
    accepted(input.on_focus(false));
    accepted(input.on_focus(true));
    const auto regained = input.capture();
    REQUIRE(regained.focus_lost);
    REQUIRE(regained.focus_gained);
    gui.begin(regained);
    CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_S));
    CHECK_FALSE(ImGui::GetIO().KeyCtrl);
    CHECK_FALSE(ImGui::IsMouseDown(ImGuiMouseButton_Left));
    CHECK_FALSE(ImGui::IsMouseClicked(ImGuiMouseButton_Left));
    ImGui::Render();
    gui.begin(input.capture());
    CHECK_FALSE(ImGui::IsMouseDown(ImGuiMouseButton_Left));
    CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_S));
    ImGui::Render();
    CHECK(std::string{value.data()} == "A\xe9\x9b\xaa");
}

TEST_CASE("Editor save shortcut needs focus a fresh press and no text or modal capture",
          "[editor-gui]") {
    InputState input;
    accepted(input.on_focus(true));
    accepted(input.on_key(Key::left_control, ButtonAction::press));
    accepted(input.on_key(Key::s, ButtonAction::press));
    const auto pressed = input.capture();
    CHECK(editor_shortcut(pressed, Key::s, false, false));
    CHECK_FALSE(editor_shortcut(pressed, Key::s, true, false));
    CHECK_FALSE(editor_shortcut(pressed, Key::s, false, true));
    auto unfocused = pressed;
    unfocused.focused = false;
    CHECK_FALSE(editor_shortcut(unfocused, Key::s, false, false));
    CHECK_FALSE(editor_shortcut(input.capture(), Key::s, false, false));
    accepted(input.on_key(Key::s, ButtonAction::repeat));
    CHECK_FALSE(editor_shortcut(input.capture(), Key::s, false, false));

    accepted(input.on_key(Key::s, ButtonAction::release));
    accepted(input.on_key(Key::left_control, ButtonAction::release));
    static_cast<void>(input.capture());
    accepted(input.on_key(Key::s, ButtonAction::press));
    CHECK_FALSE(editor_shortcut(input.capture(), Key::s, false, false));
    accepted(input.on_key(Key::s, ButtonAction::release));
    accepted(input.on_key(Key::right_control, ButtonAction::press));
    static_cast<void>(input.capture());
    accepted(input.on_key(Key::s, ButtonAction::press));
    const auto right_control = input.capture();
    CHECK(editor_shortcut(right_control, Key::s, false, false));
    auto alt = right_control;
    alt.keys[static_cast<std::size_t>(Key::left_alt) - 1].held = true;
    CHECK_FALSE(editor_shortcut(alt, Key::s, false, false));

    accepted(input.on_key(Key::s, ButtonAction::release));
    accepted(input.on_key(Key::right_control, ButtonAction::release));
    static_cast<void>(input.capture());
    accepted(input.on_key(Key::left_control, ButtonAction::press));
    accepted(input.on_key(Key::s, ButtonAction::press));
    accepted(input.on_key(Key::s, ButtonAction::release));
    accepted(input.on_key(Key::left_control, ButtonAction::release));
    auto fast_chord = input.capture();
    REQUIRE_FALSE(fast_chord.key(Key::left_control).held);
    CHECK(editor_shortcut(fast_chord, Key::s, false, false));
    fast_chord.keys[static_cast<std::size_t>(Key::left_alt) - 1] = {false, true, true, false};
    CHECK_FALSE(editor_shortcut(fast_chord, Key::s, false, false));
    fast_chord.keys[static_cast<std::size_t>(Key::left_alt) - 1] = {};
    fast_chord.keys[static_cast<std::size_t>(Key::left_super) - 1] = {false, true, true, false};
    CHECK_FALSE(editor_shortcut(fast_chord, Key::s, false, false));
}

TEST_CASE("Editor mouse focus permits replacing a field with a fast Ctrl A chord after close",
          "[editor-gui]") {
    Gui gui;
    InputState input;
    accepted(input.on_focus(true));
    std::array<char, 64> value{};
    constexpr std::string_view original = "Before close.sharkproject";
    std::copy(original.begin(), original.end(), value.begin());
    bool open = true;
    const auto frame = [&] {
        gui.begin(input.capture());
        return gui.controls(value, open);
    };
    static_cast<void>(frame());
    auto controls = frame();
    const auto click = [&](ImVec2 position) {
        accepted(input.on_cursor(position.x, position.y));
        accepted(input.on_mouse_button(MouseButton::left, ButtonAction::press));
        accepted(input.on_mouse_button(MouseButton::left, ButtonAction::release));
        static_cast<void>(frame());
        return frame();
    };
    controls = click(controls.close);
    REQUIRE_FALSE(open);
    controls = frame();
    controls = click(controls.field);
    REQUIRE(controls.field_active);

    SECTION("modifier remains held across frames") {
        accepted(input.on_key(Key::left_control, ButtonAction::press));
        accepted(input.on_key(Key::a, ButtonAction::press));
        static_cast<void>(frame());
        accepted(input.on_key(Key::a, ButtonAction::release));
        accepted(input.on_key(Key::left_control, ButtonAction::release));
        static_cast<void>(frame());
    }
    SECTION("complete chord coalesces into one snapshot") {
        accepted(input.on_key(Key::left_control, ButtonAction::press));
        accepted(input.on_key(Key::a, ButtonAction::press));
        accepted(input.on_key(Key::a, ButtonAction::release));
        accepted(input.on_key(Key::left_control, ButtonAction::release));
        static_cast<void>(frame());
        // ImGui deliberately trickles successive press/release events over
        // frames. Drain their bounded sequence without clocks or sleeps.
        static_cast<void>(frame());
        static_cast<void>(frame());
    }
    accepted(input.on_text(U'R'));
    accepted(input.on_text(U'\u96ea'));
    controls = frame();
    CHECK(controls.field_active);
    CHECK(std::string{value.data()} == "R\xe9\x9b\xaa");
    CHECK_FALSE(ImGui::GetIO().KeyCtrl);
    CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_A));
}

TEST_CASE("Editor draw capture converts logical positions clips and sRGB colors", "[editor-gui]") {
    Gui gui;
    auto& data = gui.triangle();
    const auto frame = take(capture_gui_draw(data, {160, 120}));
    REQUIRE(frame.vertices.size() == 3);
    REQUIRE(frame.draws.size() == 1);
    CHECK(frame.indices == std::vector<std::uint32_t>{0, 1, 2});
    CHECK(frame.vertices[0].position == std::array<float, 2>{4.5F, 10.5F});
    CHECK(frame.vertices[1].position == std::array<float, 2>{80.0F, 10.5F});
    CHECK(frame.vertices[2].position == std::array<float, 2>{4.5F, 90.0F});
    CHECK(frame.vertices[0].color[0] == Catch::Approx(0.215860500).margin(0.000001));
    CHECK(frame.vertices[0].color[1] == Catch::Approx(0.051269458).margin(0.000001));
    CHECK(frame.vertices[0].color[2] == 0.0F);
    CHECK(frame.vertices[0].color[3] == Catch::Approx(0.501960784).margin(0.000001));
    CHECK(frame.draws[0].clip == std::array<std::int32_t, 4>{0, 3, 91, 120});
    CHECK(frame.draws[0].first_index == 0);
    CHECK(frame.draws[0].index_count == 3);
    CHECK(validate_gui_frame(frame));
}

TEST_CASE("Editor draw capture rejects unsupported and oversized owned draw data", "[editor-gui]") {
    Gui gui;
    auto& data = gui.triangle();
    auto& list = *data.CmdLists[0];
    auto& command = list.CmdBuffer[0];
    SECTION("unknown texture") {
        command.TextureId = static_cast<ImTextureID>(3);
    }
    SECTION("callbacks are never executed") {
        command.UserCallback = [](const ImDrawList*, const ImDrawCmd*) {
            FAIL("Callback executed");
        };
    }
    SECTION("declared vertex bound") {
        data.TotalVtxCount = static_cast<int>(gui_max_vertices + 1);
    }
    SECTION("actual lists must match declared totals") {
        --data.TotalVtxCount;
    }
    SECTION("vertex offset cannot wrap") {
        command.VtxOffset = std::numeric_limits<unsigned>::max();
    }
    SECTION("vertex coordinates must be finite") {
        list.VtxBuffer[0].pos.x = std::numeric_limits<float>::infinity();
    }
    SECTION("framebuffer scale must be positive") {
        data.FramebufferScale.y = 0.0F;
    }
    SECTION("overlapping command ranges cannot exceed output bounds") {
        command.ElemCount = 99;
        const auto repeated = command;
        list.IdxBuffer.resize(99);
        for (int index = 0; index < list.IdxBuffer.Size; ++index)
            list.IdxBuffer[index] = static_cast<ImDrawIdx>(index % 3);
        data.TotalIdxCount = list.IdxBuffer.Size;
        list.CmdBuffer.resize(4000);
        std::fill(list.CmdBuffer.begin(), list.CmdBuffer.end(), repeated);
    }
    const auto captured = capture_gui_draw(data, {160, 120});
    REQUIRE_FALSE(captured);
    CHECK(captured.error().code == ErrorCode::invalid_argument);
    CHECK_FALSE(captured.error().message.empty());
    CHECK_FALSE(captured.error().context.operation.empty());
}

TEST_CASE("Editor viewport images retain their scene request with the GUI frame", "[editor-gui]") {
    Gui gui;
    auto& data = gui.triangle();
    data.CmdLists[0]->CmdBuffer[0].TextureId = static_cast<ImTextureID>(2);
    REQUIRE_FALSE(capture_gui_draw(data, {160, 120}));
    SceneViewportRequest request;
    request.extent = {64, 48};
    request.scene_revision = {7, 2};
    const auto& vertex = shark::test::shaders::scene_vs_words;
    const auto& pixel = shark::test::shaders::scene_ps_words;
    request.vertex_shader = {ShaderStage::vertex, "vsScene", {vertex.begin(), vertex.end()}};
    request.pixel_shader = {ShaderStage::pixel, "psScene", {pixel.begin(), pixel.end()}};
    auto captured = take(capture_gui_draw(data, {160, 120}, &request));
    REQUIRE(captured.viewport);
    REQUIRE(captured.draws.size() == 1);
    CHECK(captured.draws[0].texture == GuiTexture::scene_viewport);
    CHECK(captured.viewport->scene_revision == SceneRevision{7, 2});
    request.extent = {0, 48};
    CHECK(captured.viewport->extent == WindowExtent{64, 48});
    REQUIRE_FALSE(capture_gui_draw(data, {160, 120}, &request));
}
