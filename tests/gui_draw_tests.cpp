#include <shark/client/gui_draw.hpp>

#include "../engine/client/src/gui_viewport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>

namespace shark::client::test {
SceneViewportRequest scene_viewport_request_fixture();
}

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
    SECTION("unknown texture") {
        frame.draws[0].texture = static_cast<GuiTexture>(99);
    }
    SECTION("missing scene texture") {
        frame.draws[0].texture = GuiTexture::scene_viewport;
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

TEST_CASE("GUI scene images carry a validated viewport in the same owned frame", "[client][gui]") {
    auto frame = triangle();
    frame.draws[0].texture = GuiTexture::scene_viewport;
    frame.viewport = shark::client::test::scene_viewport_request_fixture();
    REQUIRE(validate_gui_frame(frame));
    SECTION("scene can be removed while atlas controls remain") {
        frame.viewport.reset();
        frame.draws[0].texture = GuiTexture::atlas;
        REQUIRE(validate_gui_frame(frame));
        return;
    }
    SECTION("target admission remains bounded") {
        frame.viewport->extent.width = scene_viewport_maximum_extent + 1;
    }
    SECTION("nested invalid geometry rejects the complete GUI frame") {
        frame.viewport->indices[0] = static_cast<std::uint32_t>(frame.viewport->vertices.size());
    }
    SECTION("nested shader must retain the scene profile") {
        frame.viewport->vertex_shader.entry_point = "vsGui";
    }
    REQUIRE_FALSE(validate_gui_frame(frame));
}

TEST_CASE("GUI viewport reuse compares camera content and ownership beyond scene revision",
          "[client][gui]") {
    std::optional<SceneViewportRequest> original =
        shark::client::test::scene_viewport_request_fixture();
    auto changed = original;
    REQUIRE(shark::client::detail::same_gui_viewport(original, changed));
    SECTION("another camera in the same committed scene") {
        changed->draws[0].transform[12] = 2;
        REQUIRE(changed->scene_revision == original->scene_revision);
    }
    SECTION("changed prepared mesh with the same source revision") {
        changed->vertices[0].position[0] = -0.5F;
    }
    SECTION("resized panel") {
        ++changed->extent.width;
    }
    SECTION("new scene incarnation") {
        ++changed->scene_revision.scene_instance;
    }
    SECTION("image removed") {
        changed.reset();
    }
    REQUIRE_FALSE(shark::client::detail::same_gui_viewport(original, changed));
    REQUIRE(shark::client::detail::same_gui_viewport(std::nullopt, std::nullopt));
}
