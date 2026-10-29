#include "gui_adapter.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace shark::editor {
namespace {
using namespace client;

ImGuiKey gui_key(Key key) noexcept {
    const auto offset = [](Key value, Key first) {
        return static_cast<int>(value) - static_cast<int>(first);
    };
    if (key >= Key::a && key <= Key::z)
        return static_cast<ImGuiKey>(ImGuiKey_A + offset(key, Key::a));
    if (key >= Key::digit_0 && key <= Key::digit_9)
        return static_cast<ImGuiKey>(ImGuiKey_0 + offset(key, Key::digit_0));
    if (key >= Key::f1 && key <= Key::f24)
        return static_cast<ImGuiKey>(ImGuiKey_F1 + offset(key, Key::f1));
    if (key >= Key::keypad_0 && key <= Key::keypad_9)
        return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + offset(key, Key::keypad_0));
    switch (key) {
    case Key::space:
        return ImGuiKey_Space;
    case Key::apostrophe:
        return ImGuiKey_Apostrophe;
    case Key::comma:
        return ImGuiKey_Comma;
    case Key::minus:
        return ImGuiKey_Minus;
    case Key::period:
        return ImGuiKey_Period;
    case Key::slash:
        return ImGuiKey_Slash;
    case Key::semicolon:
        return ImGuiKey_Semicolon;
    case Key::equal:
        return ImGuiKey_Equal;
    case Key::left_bracket:
        return ImGuiKey_LeftBracket;
    case Key::right_bracket:
        return ImGuiKey_RightBracket;
    case Key::backslash:
        return ImGuiKey_Backslash;
    case Key::grave_accent:
        return ImGuiKey_GraveAccent;
    case Key::escape:
        return ImGuiKey_Escape;
    case Key::enter:
        return ImGuiKey_Enter;
    case Key::tab:
        return ImGuiKey_Tab;
    case Key::backspace:
        return ImGuiKey_Backspace;
    case Key::insert:
        return ImGuiKey_Insert;
    case Key::delete_key:
        return ImGuiKey_Delete;
    case Key::right:
        return ImGuiKey_RightArrow;
    case Key::left:
        return ImGuiKey_LeftArrow;
    case Key::down:
        return ImGuiKey_DownArrow;
    case Key::up:
        return ImGuiKey_UpArrow;
    case Key::page_up:
        return ImGuiKey_PageUp;
    case Key::page_down:
        return ImGuiKey_PageDown;
    case Key::home:
        return ImGuiKey_Home;
    case Key::end:
        return ImGuiKey_End;
    case Key::caps_lock:
        return ImGuiKey_CapsLock;
    case Key::scroll_lock:
        return ImGuiKey_ScrollLock;
    case Key::num_lock:
        return ImGuiKey_NumLock;
    case Key::print_screen:
        return ImGuiKey_PrintScreen;
    case Key::pause:
        return ImGuiKey_Pause;
    case Key::keypad_decimal:
        return ImGuiKey_KeypadDecimal;
    case Key::keypad_divide:
        return ImGuiKey_KeypadDivide;
    case Key::keypad_multiply:
        return ImGuiKey_KeypadMultiply;
    case Key::keypad_subtract:
        return ImGuiKey_KeypadSubtract;
    case Key::keypad_add:
        return ImGuiKey_KeypadAdd;
    case Key::keypad_enter:
        return ImGuiKey_KeypadEnter;
    case Key::keypad_equal:
        return ImGuiKey_KeypadEqual;
    case Key::left_shift:
        return ImGuiKey_LeftShift;
    case Key::right_shift:
        return ImGuiKey_RightShift;
    case Key::left_control:
        return ImGuiKey_LeftCtrl;
    case Key::right_control:
        return ImGuiKey_RightCtrl;
    case Key::left_alt:
        return ImGuiKey_LeftAlt;
    case Key::right_alt:
        return ImGuiKey_RightAlt;
    case Key::left_super:
        return ImGuiKey_LeftSuper;
    case Key::right_super:
        return ImGuiKey_RightSuper;
    case Key::menu:
        return ImGuiKey_Menu;
    default:
        return ImGuiKey_None;
    }
}

template <class F> void button_events(ButtonState state, F&& emit) {
    if (state.pressed && state.released) {
        emit(!state.held);
        emit(state.held);
    } else if (state.pressed) {
        emit(true);
    } else if (state.released) {
        emit(false);
    }
}

float linear_channel(unsigned value) {
    const auto encoded = static_cast<float>(value) / 255.0F;
    return encoded <= 0.04045F ? encoded / 12.92F : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
}
} // namespace

void feed_gui_input(const GraphicsWindowFrame& frame, float elapsed_seconds) {
    auto& io = ImGui::GetIO();
    const auto& input = frame.input;
    io.DisplaySize = ImVec2(static_cast<float>(frame.state.content.width),
                            static_cast<float>(frame.state.content.height));
    io.DisplayFramebufferScale =
        ImVec2(frame.state.content.width ? static_cast<float>(frame.state.framebuffer.width) /
                                               static_cast<float>(frame.state.content.width)
                                         : 1.0F,
               frame.state.content.height ? static_cast<float>(frame.state.framebuffer.height) /
                                                static_cast<float>(frame.state.content.height)
                                          : 1.0F);
    io.DeltaTime =
        std::isfinite(elapsed_seconds) ? std::clamp(elapsed_seconds, 0.001F, 0.25F) : 1.0F / 60.0F;
    if (input.focus_lost || !input.focused) {
        // ImGui intentionally coalesces false/true focus events for viewport
        // transitions. Shark focus loss is instead a cancellation boundary,
        // including input still waiting in ImGui's quick-tap event queue.
        io.ClearEventsQueue();
        io.ClearInputKeys();
        io.ClearInputMouse();
        io.AddFocusEvent(false);
    }
    io.AddFocusEvent(input.focused);
    if (!input.focused)
        return;
    if (input.cursor.valid)
        io.AddMousePosEvent(static_cast<float>(input.cursor.x), static_cast<float>(input.cursor.y));
    else
        io.AddMousePosEvent(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
    struct Modifier {
        ImGuiKey gui;
        Key left;
        Key right;
    };
    constexpr std::array modifiers{Modifier{ImGuiMod_Ctrl, Key::left_control, Key::right_control},
                                   Modifier{ImGuiMod_Shift, Key::left_shift, Key::right_shift},
                                   Modifier{ImGuiMod_Alt, Key::left_alt, Key::right_alt},
                                   Modifier{ImGuiMod_Super, Key::left_super, Key::right_super}};
    // A snapshot coalesces transitions, so a complete chord may have both
    // edges with held=false. Keep its modifier press around the button events,
    // then restore the observed final state before independent text input.
    for (const auto modifier : modifiers) {
        const auto left = input.key(modifier.left);
        const auto right = input.key(modifier.right);
        io.AddKeyEvent(modifier.gui, left.held || left.pressed || right.held || right.pressed);
    }
    for (std::size_t index = 0; index < input.keys.size(); ++index) {
        const auto key = gui_key(static_cast<Key>(index + 1));
        if (key != ImGuiKey_None)
            button_events(input.keys[index], [&](bool down) { io.AddKeyEvent(key, down); });
    }
    for (int index = 0; index < 5; ++index)
        button_events(input.mouse_buttons[static_cast<std::size_t>(index)],
                      [&](bool down) { io.AddMouseButtonEvent(index, down); });
    io.AddMouseWheelEvent(static_cast<float>(input.scroll.x), static_cast<float>(input.scroll.y));
    for (const auto modifier : modifiers)
        io.AddKeyEvent(modifier.gui,
                       input.key(modifier.left).held || input.key(modifier.right).held);
    for (std::size_t index = 0; index < std::min(input.text_size, input.text.size()); ++index)
        io.AddInputCharacter(static_cast<unsigned>(input.text[index]));
}

bool editor_shortcut(const InputSnapshot& input, Key key, bool text_editing, bool modal) noexcept {
    const auto active = [&](Key modifier) {
        const auto state = input.key(modifier);
        return state.held || state.pressed;
    };
    return input.focused && !text_editing && !modal && input.key(key).pressed &&
           (active(Key::left_control) || active(Key::right_control)) && !active(Key::left_alt) &&
           !active(Key::right_alt) && !active(Key::left_super) && !active(Key::right_super);
}

Result<GuiFrame> capture_gui_draw(const ImDrawData& data, WindowExtent extent) {
    const auto failure = []() -> Result<GuiFrame> {
        return Error{ErrorCode::invalid_argument,
                     "Editor UI draw exceeds its bounded supported profile",
                     {.operation = "editor.gui.capture"}};
    };
    try {
        if (!data.Valid || extent.width == 0 || extent.height == 0 || extent.width > 4096 ||
            extent.height > 4096 || !std::isfinite(data.DisplayPos.x) ||
            !std::isfinite(data.DisplayPos.y) || !std::isfinite(data.DisplaySize.x) ||
            !std::isfinite(data.DisplaySize.y) || data.DisplaySize.x <= 0 ||
            data.DisplaySize.y <= 0 || !std::isfinite(data.FramebufferScale.x) ||
            !std::isfinite(data.FramebufferScale.y) || data.FramebufferScale.x <= 0 ||
            data.FramebufferScale.y <= 0 || data.TotalVtxCount < 0 || data.TotalIdxCount < 0 ||
            static_cast<std::size_t>(data.TotalVtxCount) > gui_max_vertices ||
            static_cast<std::size_t>(data.TotalIdxCount) > gui_max_indices ||
            data.CmdListsCount < 0 ||
            static_cast<std::size_t>(data.CmdListsCount) > gui_max_draws ||
            data.CmdListsCount != data.CmdLists.Size ||
            (data.CmdListsCount > 0 && !data.CmdLists.Data))
            return failure();
        std::size_t vertices = 0;
        std::size_t indices = 0;
        std::size_t commands = 0;
        for (int index = 0; index < data.CmdListsCount; ++index) {
            const auto* list = data.CmdLists[index];
            if (!list || list->VtxBuffer.Size < 0 || list->IdxBuffer.Size < 0 ||
                list->CmdBuffer.Size < 0 || (list->VtxBuffer.Size > 0 && !list->VtxBuffer.Data) ||
                (list->IdxBuffer.Size > 0 && !list->IdxBuffer.Data) ||
                (list->CmdBuffer.Size > 0 && !list->CmdBuffer.Data) ||
                static_cast<std::size_t>(list->VtxBuffer.Size) > gui_max_vertices - vertices ||
                static_cast<std::size_t>(list->IdxBuffer.Size) > gui_max_indices - indices ||
                static_cast<std::size_t>(list->CmdBuffer.Size) > gui_max_draws - commands)
                return failure();
            vertices += static_cast<std::size_t>(list->VtxBuffer.Size);
            indices += static_cast<std::size_t>(list->IdxBuffer.Size);
            commands += static_cast<std::size_t>(list->CmdBuffer.Size);
        }
        if (vertices != static_cast<std::size_t>(data.TotalVtxCount) ||
            indices != static_cast<std::size_t>(data.TotalIdxCount))
            return failure();
        GuiFrame result;
        result.extent = extent;
        result.vertices.reserve(static_cast<std::size_t>(data.TotalVtxCount));
        result.indices.reserve(static_cast<std::size_t>(data.TotalIdxCount));
        for (int list_index = 0; list_index < data.CmdListsCount; ++list_index) {
            const auto& list = *data.CmdLists[list_index];
            const auto base = static_cast<std::uint32_t>(result.vertices.size());
            for (const auto& vertex : list.VtxBuffer) {
                GuiVertex value;
                value.position = {(vertex.pos.x - data.DisplayPos.x) * data.FramebufferScale.x,
                                  (vertex.pos.y - data.DisplayPos.y) * data.FramebufferScale.y};
                value.uv = {vertex.uv.x, vertex.uv.y};
                value.color = {linear_channel((vertex.col >> IM_COL32_R_SHIFT) & 255U),
                               linear_channel((vertex.col >> IM_COL32_G_SHIFT) & 255U),
                               linear_channel((vertex.col >> IM_COL32_B_SHIFT) & 255U),
                               static_cast<float>((vertex.col >> IM_COL32_A_SHIFT) & 255U) /
                                   255.0F};
                result.vertices.push_back(value);
            }
            for (const auto& command : list.CmdBuffer) {
                if (command.UserCallback || command.GetTexID() != static_cast<ImTextureID>(1) ||
                    command.IdxOffset > static_cast<unsigned>(list.IdxBuffer.Size) ||
                    command.ElemCount >
                        static_cast<unsigned>(list.IdxBuffer.Size) - command.IdxOffset ||
                    command.VtxOffset > static_cast<unsigned>(list.VtxBuffer.Size))
                    return failure();
                const float x0 = (command.ClipRect.x - data.DisplayPos.x) * data.FramebufferScale.x;
                const float y0 = (command.ClipRect.y - data.DisplayPos.y) * data.FramebufferScale.y;
                const float x1 = (command.ClipRect.z - data.DisplayPos.x) * data.FramebufferScale.x;
                const float y1 = (command.ClipRect.w - data.DisplayPos.y) * data.FramebufferScale.y;
                if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) ||
                    !std::isfinite(y1))
                    return failure();
                GuiDraw draw;
                draw.clip = {static_cast<std::int32_t>(std::clamp(
                                 std::floor(x0), 0.0F, static_cast<float>(extent.width))),
                             static_cast<std::int32_t>(std::clamp(
                                 std::floor(y0), 0.0F, static_cast<float>(extent.height))),
                             static_cast<std::int32_t>(
                                 std::clamp(std::ceil(x1), 0.0F, static_cast<float>(extent.width))),
                             static_cast<std::int32_t>(std::clamp(
                                 std::ceil(y1), 0.0F, static_cast<float>(extent.height)))};
                if (draw.clip[0] >= draw.clip[2] || draw.clip[1] >= draw.clip[3] ||
                    command.ElemCount == 0)
                    continue;
                if (command.ElemCount > gui_max_indices - result.indices.size())
                    return failure();
                draw.first_index = static_cast<std::uint32_t>(result.indices.size());
                draw.index_count = command.ElemCount;
                for (unsigned index = 0; index < command.ElemCount; ++index) {
                    const auto local =
                        static_cast<std::uint64_t>(
                            list.IdxBuffer[static_cast<int>(command.IdxOffset + index)]) +
                        command.VtxOffset;
                    if (local >= static_cast<unsigned>(list.VtxBuffer.Size))
                        return failure();
                    result.indices.push_back(base + static_cast<std::uint32_t>(local));
                }
                result.draws.push_back(draw);
            }
        }
        if (auto valid = validate_gui_frame(result); !valid)
            return valid.error();
        return result;
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot capture editor UI draw values",
                     {.operation = "editor.gui.capture"}};
    }
}

} // namespace shark::editor
