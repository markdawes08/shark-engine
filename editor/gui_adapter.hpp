#pragma once

#include <shark/client/graphics.hpp>
#include <shark/client/gui_draw.hpp>

struct ImDrawData;

namespace shark::editor {

// The application owns the ImGui context; native callbacks still belong only to
// Shark's window. No native key/window/device handle crosses this adapter.
void feed_gui_input(const client::GraphicsWindowFrame& frame, float elapsed_seconds);
[[nodiscard]] Result<client::GuiFrame> capture_gui_draw(const ImDrawData& data,
                                                        client::WindowExtent extent);
[[nodiscard]] bool editor_shortcut(const client::InputSnapshot& input, client::Key key,
                                   bool text_editing, bool modal) noexcept;

} // namespace shark::editor
