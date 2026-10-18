#include "lifecycle_policy.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Presentation suspends for zero framebuffer extents and minimize", "[graphics-probe]") {
    using shark::probe::drawableFramebuffer;
    REQUIRE_FALSE(drawableFramebuffer(0, 64, false));
    REQUIRE_FALSE(drawableFramebuffer(64, 0, false));
    REQUIRE_FALSE(drawableFramebuffer(0, 0, false));
    REQUIRE_FALSE(drawableFramebuffer(64, 64, true));
    REQUIRE_FALSE(drawableFramebuffer(-1, 64, false));
    REQUIRE(drawableFramebuffer(64, 64, false));
    REQUIRE(drawableFramebuffer(128, 96, false));
}
