#include <shark/client/gui_draw.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

namespace {
using namespace shark::client;
GuiFrame triangle() {
    GuiFrame frame;
    frame.extent = {100, 60};
    frame.vertices = {{{0, 0}, {0, 0}, {1, 0, 0, 1}},
                      {{100, 0}, {1, 0}, {0, 1, 0, 1}},
                      {{50, 60}, {0.5F, 1}, {0, 0, 1, 1}}};
    frame.indices = {0, 1, 2};
    frame.draws = {{0, 3, {0, 0, 100, 60}}};
    return frame;
}
} // namespace

TEST_CASE("GUI snapshots bound uploads and reject invalid geometry and scissors", "[client][gui]") {
    auto frame = triangle();
    REQUIRE(validate_gui_frame(frame));
    SECTION("empty frame") {
        REQUIRE(validate_gui_frame(GuiFrame{{100, 60}, {}, {}, {}}));
        return;
    }
    SECTION("extent") {
        frame.extent.width = 4097;
    }
    SECTION("nonfinite position") {
        frame.vertices[0].position[0] = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("unbounded position") {
        frame.vertices[0].position[0] = 10000001.0F;
    }
    SECTION("UV") {
        frame.vertices[0].uv[0] = -0.1F;
    }
    SECTION("color") {
        frame.vertices[0].color[3] = 1.1F;
    }
    SECTION("vertex count") {
        frame.vertices.resize(gui_max_vertices + 1);
    }
    SECTION("index count") {
        frame.indices.resize(gui_max_indices + 3);
    }
    SECTION("draw count") {
        frame.draws.resize(gui_max_draws + 1, frame.draws[0]);
    }
    SECTION("index outside vertices") {
        frame.indices[2] = 3;
    }
    SECTION("partial triangle") {
        frame.indices.pop_back();
    }
    SECTION("draw past end") {
        frame.draws[0].first_index = 1;
    }
    SECTION("overflowing draw") {
        frame.draws[0].first_index = std::numeric_limits<std::uint32_t>::max();
    }
    SECTION("empty draw") {
        frame.draws[0].index_count = 0;
    }
    SECTION("partial draw triangle") {
        frame.draws[0].index_count = 2;
    }
    SECTION("negative clip") {
        frame.draws[0].clip[0] = -1;
    }
    SECTION("empty clip") {
        frame.draws[0].clip[2] = 0;
    }
    SECTION("outside clip") {
        frame.draws[0].clip[3] = 61;
    }
    const auto result = validate_gui_frame(frame);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == shark::ErrorCode::invalid_argument);
}

TEST_CASE("GUI atlas rejects malformed storage and foreign shader profiles", "[client][gui]") {
    GuiAtlas atlas;
    REQUIRE_FALSE(validate_gui_atlas(atlas));
    atlas.extent = {1, 1};
    atlas.rgba8 = {255, 255, 255, 255};
    // Correct pixels alone cannot bypass the typed offline GUI shader contract.
    REQUIRE_FALSE(validate_gui_atlas(atlas));
    atlas.extent.width = 2049;
    REQUIRE_FALSE(validate_gui_atlas(atlas));
}
