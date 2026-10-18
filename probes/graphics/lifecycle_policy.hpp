#pragma once

namespace shark::probe {

// The same guard is used before real presentation. A minimized or zero-sized
// framebuffer must not acquire an image or create a zero-extent swapchain.
constexpr bool drawableFramebuffer(int width, int height, bool minimized) noexcept {
    return width > 0 && height > 0 && !minimized;
}

} // namespace shark::probe
