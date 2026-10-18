#include <shark/client/graphics.hpp>

#include "../engine/client/src/graphics_backend.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using shark::Error;
using shark::ErrorCode;
using shark::Result;
using shark::client::AdapterKind;
using shark::client::GraphicsAdapter;
using shark::client::GraphicsConfig;
using shark::client::GraphicsDevice;
using shark::client::GraphicsDiagnostics;
using shark::client::GraphicsValidation;
using shark::client::WindowConfig;
using shark::client::detail::GraphicsBackend;
using shark::client::detail::GraphicsDiagnosticSink;

constexpr std::array<std::uint32_t, 10> required_limits{4096, 2048, 12,    4,         4,
                                                        16,   16,   16384, 134217728, 128};
constexpr std::string_view uuid_a = "0123456789abcdef0123456789abcdef";
constexpr std::string_view uuid_b = "1123456789abcdef0123456789abcdef";
constexpr std::string_view uuid_c = "2123456789abcdef0123456789abcdef";

GraphicsAdapter supported_adapter(std::string_view uuid = uuid_a,
                                  AdapterKind kind = AdapterKind::discrete) {
    GraphicsAdapter adapter;
    adapter.uuid = uuid;
    adapter.name = "Test graphics adapter";
    adapter.driver_name = "Deterministic CPU fixture";
    adapter.driver_info = "No native graphics initialized";
    adapter.vendor_id = 0x1234;
    adapter.device_id = 0x5678;
    adapter.driver_version = 7;
    adapter.api_version = (1U << 22U) | (3U << 12U);
    adapter.queue_family = 2;
    adapter.kind = kind;
    adapter.capabilities.features.fill(true);
    adapter.capabilities.formats.fill(true);
    adapter.capabilities.limits = required_limits;
    return adapter;
}

enum class Stage {
    window,
    instance,
    surface,
    enumerate,
    device,
    nvrhi,
    destroy_nvrhi,
    destroy_device,
    destroy_surface,
    destroy_instance,
    destroy_window,
};

constexpr std::array startup_stages{Stage::window,    Stage::instance, Stage::surface,
                                    Stage::enumerate, Stage::device,   Stage::nvrhi};
constexpr std::array cleanup_stages{Stage::destroy_nvrhi, Stage::destroy_device,
                                    Stage::destroy_surface, Stage::destroy_instance,
                                    Stage::destroy_window};

struct BackendState {
    std::array<Stage, 64> stages{};
    std::size_t stage_count{0};
    std::optional<Stage> fail_stage;
    bool throw_failure{false};
    bool throw_nonstandard{false};
    bool warning_on_instance{false};
    std::optional<Stage> error_stage;
    bool error_on_instance_destruction{false};
    std::size_t destructor_count{0};
    std::vector<GraphicsAdapter> adapters{supported_adapter()};
    std::optional<std::size_t> selected;
    GraphicsValidation validation{GraphicsValidation::off};
    WindowConfig window{};
    GraphicsDiagnosticSink* diagnostics{nullptr};

    void record(Stage stage) noexcept {
        if (stage_count < stages.size()) {
            stages[stage_count++] = stage;
        }
    }

    [[nodiscard]] std::vector<Stage> recorded() const {
        return {stages.begin(), stages.begin() + static_cast<std::ptrdiff_t>(stage_count)};
    }

    Result<void> attempt(Stage stage) {
        record(stage);
        if (error_stage == stage && diagnostics != nullptr) {
            diagnostics->record(true, "Fixture startup validation error");
        }
        if (fail_stage != stage) {
            return {};
        }
        if (throw_nonstandard) {
            throw 17;
        }
        if (throw_failure) {
            throw std::runtime_error("Injected graphics startup exception");
        }
        return Error{ErrorCode::execution_failed,
                     "Injected graphics startup failure",
                     {.operation = "test.graphics.start", .request_id = "retained-request"}};
    }
};

class TestBackend final : public GraphicsBackend {
  public:
    explicit TestBackend(BackendState& state) noexcept : state_(state) {}
    ~TestBackend() override {
        ++state_.destructor_count;
    }

    Result<void> create_window(const WindowConfig& config) override {
        state_.window = config;
        return state_.attempt(Stage::window);
    }
    Result<void> create_instance(GraphicsValidation validation,
                                 GraphicsDiagnosticSink& diagnostics) override {
        state_.validation = validation;
        state_.diagnostics = &diagnostics;
        if (state_.warning_on_instance) {
            diagnostics.record(false, "Fixture instance warning");
        }
        return state_.attempt(Stage::instance);
    }
    Result<void> create_surface() override {
        return state_.attempt(Stage::surface);
    }
    Result<std::vector<GraphicsAdapter>> enumerate_adapters() override {
        if (auto result = state_.attempt(Stage::enumerate); !result) {
            return result.error();
        }
        return state_.adapters;
    }
    Result<void> create_device(std::size_t adapter_index) override {
        state_.selected = adapter_index;
        return state_.attempt(Stage::device);
    }
    Result<void> create_nvrhi(GraphicsDiagnosticSink& diagnostics) override {
        state_.diagnostics = &diagnostics;
        return state_.attempt(Stage::nvrhi);
    }
    void destroy_nvrhi() noexcept override {
        state_.record(Stage::destroy_nvrhi);
    }
    void destroy_device() noexcept override {
        state_.record(Stage::destroy_device);
    }
    void destroy_surface() noexcept override {
        state_.record(Stage::destroy_surface);
    }
    void destroy_instance() noexcept override {
        state_.record(Stage::destroy_instance);
        if (state_.error_on_instance_destruction && state_.diagnostics != nullptr) {
            state_.diagnostics->record(true, "Fixture instance destruction error");
        }
    }
    void destroy_window() noexcept override {
        state_.record(Stage::destroy_window);
    }

  private:
    BackendState& state_;
};

Result<std::unique_ptr<GraphicsDevice>> create(BackendState& state,
                                               const GraphicsConfig& config = {}) {
    return shark::client::detail::create_graphics(config, std::make_unique<TestBackend>(state));
}

Result<std::vector<GraphicsAdapter>> inventory(BackendState& state,
                                               const GraphicsConfig& config = {}) {
    return shark::client::detail::inventory_graphics(config, std::make_unique<TestBackend>(state));
}

template <typename T> void check_error(const Result<T>& result, ErrorCode code) {
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().message.empty());
    CHECK_FALSE(result.error().context.operation.empty());
}

std::vector<Stage> expected_failed_start(std::size_t index) {
    std::vector<Stage> expected(startup_stages.begin(),
                                startup_stages.begin() + static_cast<std::ptrdiff_t>(index + 1));
    if (index >= 5) {
        expected.push_back(Stage::destroy_nvrhi);
    }
    if (index >= 4) {
        expected.push_back(Stage::destroy_device);
    }
    if (index >= 2) {
        expected.push_back(Stage::destroy_surface);
    }
    if (index >= 1) {
        expected.push_back(Stage::destroy_instance);
    }
    expected.push_back(Stage::destroy_window);
    return expected;
}

std::vector<Stage> complete_lifetime() {
    std::vector<Stage> result(startup_stages.begin(), startup_stages.end());
    result.insert(result.end(), cleanup_stages.begin(), cleanup_stages.end());
    return result;
}

} // namespace

TEST_CASE("graphics owner exposes only owned reports and is nonmovable", "[graphics-device]") {
    STATIC_REQUIRE(!std::is_copy_constructible_v<GraphicsDevice>);
    STATIC_REQUIRE(!std::is_copy_assignable_v<GraphicsDevice>);
    STATIC_REQUIRE(!std::is_move_constructible_v<GraphicsDevice>);
    STATIC_REQUIRE(!std::is_move_assignable_v<GraphicsDevice>);
    STATIC_REQUIRE(std::is_nothrow_destructible_v<GraphicsDevice>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<GraphicsDevice&>().adapter()),
                                  Result<GraphicsAdapter>>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<GraphicsDevice&>().diagnostics()),
                                  Result<GraphicsDiagnostics>>);
}

TEST_CASE("graphics UUIDs have one exact lowercase hexadecimal representation",
          "[graphics-device]") {
    CHECK(shark::client::valid_graphics_uuid(uuid_a));
    CHECK(shark::client::valid_graphics_uuid("00000000000000000000000000000000"));
    CHECK(shark::client::valid_graphics_uuid("ffffffffffffffffffffffffffffffff"));
    for (const auto text :
         {"", "0123456789abcdef", "0123456789abcdef0123456789abcdef0",
          "0123456789ABCDEF0123456789ABCDEF", "01234567-89ab-cdef-0123-456789abcdef",
          "g123456789abcdef0123456789abcdef"}) {
        CAPTURE(text);
        CHECK_FALSE(shark::client::valid_graphics_uuid(text));
    }
    auto nul = std::string(uuid_a);
    nul[3] = '\0';
    CHECK_FALSE(shark::client::valid_graphics_uuid(nul));
}

TEST_CASE("graphics profile checks every required feature format and exact limit",
          "[graphics-device]") {
    const auto baseline = supported_adapter();
    CHECK(shark::client::missing_graphics_requirements(baseline).empty());
    std::vector<std::string> all_missing;
    for (std::size_t index = 0; index < baseline.capabilities.features.size(); ++index) {
        auto adapter = baseline;
        adapter.capabilities.features[index] = false;
        auto missing = shark::client::missing_graphics_requirements(adapter);
        CAPTURE(index);
        REQUIRE(missing.size() == 1);
        CHECK_FALSE(missing.front().empty());
        all_missing.push_back(missing.front());
    }
    for (std::size_t index = 0; index < baseline.capabilities.formats.size(); ++index) {
        auto adapter = baseline;
        adapter.capabilities.formats[index] = false;
        auto missing = shark::client::missing_graphics_requirements(adapter);
        CAPTURE(index);
        REQUIRE(missing.size() == 1);
        CHECK_FALSE(missing.front().empty());
        all_missing.push_back(missing.front());
    }
    for (std::size_t index = 0; index < required_limits.size(); ++index) {
        auto adapter = baseline;
        adapter.capabilities.limits[index] = required_limits[index] - 1;
        auto missing = shark::client::missing_graphics_requirements(adapter);
        CAPTURE(index);
        REQUIRE(missing.size() == 1);
        CHECK_FALSE(missing.front().empty());
        all_missing.push_back(missing.front());
        adapter.capabilities.limits[index] = required_limits[index];
        CHECK(shark::client::missing_graphics_requirements(adapter).empty());
        adapter.capabilities.limits[index] = std::numeric_limits<std::uint32_t>::max();
        CHECK(shark::client::missing_graphics_requirements(adapter).empty());
    }
    auto absent = baseline;
    absent.capabilities = {};
    CHECK(shark::client::missing_graphics_requirements(absent) == all_missing);
    std::sort(all_missing.begin(), all_missing.end());
    CHECK(std::adjacent_find(all_missing.begin(), all_missing.end()) == all_missing.end());
}

TEST_CASE("graphics rejects invalid selectors and validation policy before backend startup",
          "[graphics-device]") {
    for (const auto text :
         {"short", "0123456789ABCDEF0123456789ABCDEF", "0123456789abcdef0123456789abcdef0"}) {
        BackendState state;
        GraphicsConfig config;
        config.adapter_uuid = text;
        check_error(create(state, config), ErrorCode::invalid_argument);
        CHECK(state.recorded().empty());
        CHECK(state.destructor_count == 1);
        check_error(inventory(state, config), ErrorCode::invalid_argument);
        CHECK(state.recorded().empty());
        CHECK(state.destructor_count == 2);
    }
    BackendState state;
    GraphicsConfig config;
    config.validation = static_cast<GraphicsValidation>(99);
    check_error(create(state, config), ErrorCode::invalid_argument);
    CHECK(state.recorded().empty());
    check_error(inventory(state, config), ErrorCode::invalid_argument);
    CHECK(state.recorded().empty());
}

TEST_CASE("graphics unavailable backend returns an owned capability error", "[graphics-device]") {
    check_error(shark::client::detail::create_graphics({}, nullptr),
                ErrorCode::capability_unavailable);
    check_error(shark::client::detail::inventory_graphics({}, nullptr),
                ErrorCode::capability_unavailable);
    if (!shark::client::native_graphics_available()) {
        check_error(GraphicsDevice::create(), ErrorCode::capability_unavailable);
        check_error(GraphicsDevice::inventory(), ErrorCode::capability_unavailable);
    }
}

TEST_CASE("graphics owner initializes in order and closes each resource once in reverse",
          "[graphics-device]") {
    BackendState state;
    GraphicsConfig config;
    config.window.title = "Owned graphics fixture";
    config.window.content = {480, 320};
    config.window.visible = false;
    {
        auto result = create(state, config);
        REQUIRE(result);
        auto graphics = std::move(result).value();
        CHECK(state.recorded() == std::vector<Stage>(startup_stages.begin(), startup_stages.end()));
        CHECK(state.validation == GraphicsValidation::required);
        CHECK(state.window.title == config.window.title);
        CHECK(state.window.content.width == 480);
        CHECK(state.window.content.height == 320);
        CHECK_FALSE(state.window.visible);
        REQUIRE(state.selected.has_value());
        CHECK(*state.selected == 0);
        REQUIRE(graphics->close());
        CHECK(state.recorded() == complete_lifetime());
        REQUIRE(graphics->close());
        CHECK(state.recorded() == complete_lifetime());
    }
    CHECK(state.recorded() == complete_lifetime());
    CHECK(state.destructor_count == 1);
}

TEST_CASE("graphics destructor unwinds a live owner and permits repeated lifetimes",
          "[graphics-device]") {
    for (std::size_t index = 0; index < 32; ++index) {
        BackendState state;
        {
            auto graphics = create(state);
            REQUIRE(graphics);
        }
        CAPTURE(index);
        CHECK(state.recorded() == complete_lifetime());
        CHECK(state.destructor_count == 1);
    }
}

TEST_CASE("graphics startup failures unwind attempted phases and preserve owned errors",
          "[graphics-device]") {
    for (std::size_t index = 0; index < startup_stages.size(); ++index) {
        BackendState state;
        state.fail_stage = startup_stages[index];
        auto result = create(state);
        CAPTURE(index);
        check_error(result, ErrorCode::execution_failed);
        CHECK(result.error().context.operation == "test.graphics.start");
        CHECK(result.error().context.request_id == "retained-request");
        CHECK(result.error().message == "Injected graphics startup failure");
        CHECK(state.recorded() == expected_failed_start(index));
        CHECK(state.destructor_count == 1);
    }
}

TEST_CASE("graphics catches standard and nonstandard exceptions at every startup phase",
          "[graphics-device]") {
    for (const bool standard : {false, true}) {
        for (std::size_t index = 0; index < startup_stages.size(); ++index) {
            BackendState state;
            state.fail_stage = startup_stages[index];
            state.throw_failure = standard;
            state.throw_nonstandard = !standard;
            CAPTURE(index, standard);
            check_error(create(state), ErrorCode::execution_failed);
            CHECK(state.recorded() == expected_failed_start(index));
            CHECK(state.destructor_count == 1);
        }
    }
}

TEST_CASE("graphics startup errors retain teardown validation without replacing the cause",
          "[graphics-device]") {
    for (const auto index : {std::size_t{3}, std::size_t{4}, std::size_t{5}}) {
        BackendState state;
        state.fail_stage = startup_stages[index];
        state.error_on_instance_destruction = true;
        const auto check_cause = [](const Error& error) {
            CHECK(error.code == ErrorCode::execution_failed);
            CHECK(error.context.operation == "test.graphics.start");
            CHECK(error.context.request_id == "retained-request");
            CHECK(error.message.starts_with("Injected graphics startup failure"));
            CHECK(error.message.find("teardown validation errors: 1") != std::string::npos);
            CHECK(error.message.find("Fixture instance destruction error") != std::string::npos);
        };
        CAPTURE(index);
        if (index == 3) {
            auto result = inventory(state);
            REQUIRE_FALSE(result);
            check_cause(result.error());
        } else {
            auto result = create(state);
            REQUIRE_FALSE(result);
            check_cause(result.error());
        }
        CHECK(state.recorded() == expected_failed_start(index));
        CHECK(state.destructor_count == 1);
    }
}

TEST_CASE("graphics rejects callback errors before advancing startup", "[graphics-device]") {
    for (std::size_t index = 1; index < startup_stages.size(); ++index) {
        BackendState state;
        state.error_stage = startup_stages[index];
        CAPTURE(index);
        check_error(create(state), ErrorCode::execution_failed);
        CHECK(state.recorded() == expected_failed_start(index));
        CHECK(state.destructor_count == 1);
    }
}

TEST_CASE("graphics inventory stops before device creation and returns independent reports",
          "[graphics-device]") {
    BackendState state;
    state.adapters.push_back(supported_adapter(uuid_b, AdapterKind::integrated));
    state.adapters[1].capabilities.features[2] = false;
    auto result = inventory(state);
    REQUIRE(result);
    REQUIRE(result.value().size() == 2);
    CHECK(result.value()[1].capabilities.features[2] == false);
    CHECK_FALSE(state.selected.has_value());
    CHECK(state.recorded() == expected_failed_start(3));
    CHECK(state.destructor_count == 1);
    state.adapters[0].name = "Changed backend storage";
    CHECK(result.value()[0].name == "Test graphics adapter");
    CHECK(result.value()[0].uuid == uuid_a);
}

TEST_CASE("graphics inventory unwinds failing and throwing startup phases", "[graphics-device]") {
    for (const bool throwing : {false, true}) {
        for (std::size_t index = 0; index < 4; ++index) {
            BackendState state;
            state.fail_stage = startup_stages[index];
            state.throw_failure = throwing;
            CAPTURE(index, throwing);
            check_error(inventory(state), ErrorCode::execution_failed);
            CHECK(state.recorded() == expected_failed_start(index));
            CHECK(state.destructor_count == 1);
        }
    }
}

TEST_CASE("graphics inventory rejects selectors rather than silently ignoring them",
          "[graphics-device]") {
    BackendState state;
    GraphicsConfig config;
    config.adapter_uuid = uuid_a;
    check_error(inventory(state, config), ErrorCode::invalid_argument);
    CHECK(state.recorded().empty());
    CHECK(state.destructor_count == 1);
}

TEST_CASE("graphics inventory rejects validation errors during startup and cleanup",
          "[graphics-device]") {
    for (std::size_t index = 1; index < 4; ++index) {
        BackendState state;
        state.error_stage = startup_stages[index];
        CAPTURE(index);
        check_error(inventory(state), ErrorCode::execution_failed);
        CHECK(state.recorded() == expected_failed_start(index));
        CHECK(state.destructor_count == 1);
    }
    BackendState state;
    state.error_on_instance_destruction = true;
    check_error(inventory(state), ErrorCode::execution_failed);
    CHECK(state.recorded() == expected_failed_start(3));
    CHECK(state.destructor_count == 1);
}

TEST_CASE("graphics automatic selection prefers supported hardware and deterministic UUID ties",
          "[graphics-device]") {
    BackendState state;
    state.adapters = {supported_adapter(uuid_c, AdapterKind::discrete),
                      supported_adapter(uuid_b, AdapterKind::integrated),
                      supported_adapter(uuid_a, AdapterKind::discrete)};
    do {
        BackendState current;
        current.adapters = state.adapters;
        auto result = create(current);
        REQUIRE(result);
        auto selected = result.value()->adapter();
        REQUIRE(selected);
        CHECK(selected.value().uuid == uuid_a);
        REQUIRE(current.selected.has_value());
        CHECK(current.adapters[*current.selected].uuid == uuid_a);
    } while (std::next_permutation(
        state.adapters.begin(), state.adapters.end(),
        [](const auto& left, const auto& right) { return left.uuid > right.uuid; }));

    state.adapters = {supported_adapter(uuid_a), supported_adapter(uuid_b, AdapterKind::integrated),
                      supported_adapter(uuid_c, AdapterKind::other)};
    state.adapters[0].capabilities.features[0] = false;
    auto result = create(state);
    REQUIRE(result);
    REQUIRE(state.selected.has_value());
    CHECK(*state.selected == 1);
}

TEST_CASE("graphics automatic selection excludes software and reports absent hardware",
          "[graphics-device]") {
    BackendState state;
    state.adapters = {supported_adapter(uuid_a, AdapterKind::software),
                      supported_adapter(uuid_b, AdapterKind::other)};
    {
        auto result = create(state);
        REQUIRE(result);
        REQUIRE(state.selected.has_value());
        CHECK(*state.selected == 1);
    }
    state = {};
    state.adapters = {supported_adapter(uuid_a, AdapterKind::software)};
    check_error(create(state), ErrorCode::capability_unavailable);
    CHECK_FALSE(state.selected.has_value());
    CHECK(state.recorded() == expected_failed_start(3));

    state = {};
    state.adapters = {supported_adapter(uuid_a, AdapterKind::software), supported_adapter(uuid_b)};
    GraphicsConfig config;
    config.adapter_uuid = uuid_a;
    check_error(create(state, config), ErrorCode::capability_unavailable);
    CHECK_FALSE(state.selected.has_value());
    CHECK(state.recorded() == expected_failed_start(3));
}

TEST_CASE("graphics explicit adapter selection never falls back", "[graphics-device]") {
    GraphicsConfig config;
    config.adapter_uuid = uuid_b;
    BackendState state;
    state.adapters = {supported_adapter(uuid_a),
                      supported_adapter(uuid_b, AdapterKind::integrated)};
    {
        auto result = create(state, config);
        REQUIRE(result);
        REQUIRE(state.selected.has_value());
        CHECK(*state.selected == 1);
    }
    state = {};
    check_error(create(state, config), ErrorCode::capability_unavailable);
    CHECK_FALSE(state.selected.has_value());
    CHECK(state.recorded() == expected_failed_start(3));

    state = {};
    state.adapters.push_back(supported_adapter(uuid_b));
    state.adapters[1].capabilities.features[1] = false;
    const auto missing = shark::client::missing_graphics_requirements(state.adapters[1]);
    REQUIRE(missing.size() == 1);
    auto result = create(state, config);
    check_error(result, ErrorCode::capability_unavailable);
    CHECK(result.error().message.find(missing.front()) != std::string::npos);
    CHECK_FALSE(state.selected.has_value());
    CHECK(state.recorded() == expected_failed_start(3));
}

TEST_CASE("graphics metadata bounds reject malformed inventories before device creation",
          "[graphics-device]") {
    for (std::size_t field = 0; field < 10; ++field) {
        BackendState state;
        switch (field) {
        case 0:
            state.adapters[0].uuid = "invalid";
            break;
        case 1:
            state.adapters[0].name.assign(257, 'n');
            break;
        case 2:
            state.adapters[0].driver_name.assign(257, 'd');
            break;
        case 3:
            state.adapters[0].driver_info.assign(257, 'i');
            break;
        case 4:
            state.adapters[0].kind = static_cast<AdapterKind>(99);
            break;
        case 5:
            state.adapters.push_back(state.adapters[0]);
            break;
        case 6:
            state.adapters[0].name.clear();
            break;
        case 7:
            state.adapters[0].name.assign("invalid\0name", 12);
            break;
        case 8:
            state.adapters[0].driver_name.assign("invalid\0name", 12);
            break;
        default:
            state.adapters[0].driver_info.assign("invalid\0info", 12);
            break;
        }
        CAPTURE(field);
        auto result = create(state);
        REQUIRE_FALSE(result);
        CHECK_FALSE(result.error().message.empty());
        CHECK_FALSE(result.error().context.operation.empty());
        CHECK_FALSE(state.selected.has_value());
        CHECK(state.recorded() == expected_failed_start(3));
        state.stages = {};
        state.stage_count = 0;
        auto enumerated = inventory(state);
        REQUIRE_FALSE(enumerated);
        CHECK_FALSE(enumerated.error().message.empty());
        CHECK(state.recorded() == expected_failed_start(3));
    }
}

TEST_CASE("graphics adapter count bounds and metadata maximum are explicit", "[graphics-device]") {
    BackendState state;
    state.adapters.clear();
    check_error(create(state), ErrorCode::capability_unavailable);
    CHECK_FALSE(state.selected.has_value());
    CHECK(state.recorded() == expected_failed_start(3));

    state = {};
    state.adapters.clear();
    constexpr std::string_view digits = "0123456789abcdef";
    for (std::size_t index = 0; index < 65; ++index) {
        std::string uuid(32, '0');
        uuid[30] = digits[index / 16];
        uuid[31] = digits[index % 16];
        state.adapters.push_back(supported_adapter(uuid));
    }
    auto excessive = inventory(state);
    REQUIRE_FALSE(excessive);
    CHECK_FALSE(excessive.error().message.empty());
    CHECK(state.recorded() == expected_failed_start(3));

    state.adapters.pop_back();
    state.stage_count = 0;
    auto maximum = inventory(state);
    REQUIRE(maximum);
    CHECK(maximum.value().size() == 64);
    CHECK(state.recorded() == expected_failed_start(3));

    state = {};
    state.adapters[0].name.assign(256, 'n');
    state.adapters[0].driver_name.assign(256, 'd');
    state.adapters[0].driver_info.assign(256, 'i');
    auto result = create(state);
    REQUIRE(result);
    auto selected = result.value()->adapter();
    REQUIRE(selected);
    CHECK(selected.value().name.size() == 256);
    CHECK(selected.value().driver_name.size() == 256);
    CHECK(selected.value().driver_info.size() == 256);
}

TEST_CASE("graphics adapter metadata requires bounded well formed UTF8", "[graphics-device]") {
    for (const auto malformed : {"\x80", "\xC0\xAF", "\xE2\x82", "\xED\xA0\x80", "\xF4\x90\x80\x80",
                                 "\xF8\x88\x80\x80\x80"}) {
        for (std::size_t field = 0; field < 3; ++field) {
            BackendState state;
            if (field == 0) {
                state.adapters[0].name = malformed;
            } else if (field == 1) {
                state.adapters[0].driver_name = malformed;
            } else {
                state.adapters[0].driver_info = malformed;
            }
            CAPTURE(field);
            check_error(create(state), ErrorCode::execution_failed);
            CHECK_FALSE(state.selected.has_value());
            CHECK(state.recorded() == expected_failed_start(3));
        }
    }
    BackendState state;
    state.adapters[0].name = "Unicode GPU \xCE\xBB \xE6\xB5\x8B\xE8\xAF\x95 \xF0\x9F\xA6\x88";
    state.adapters[0].driver_name.clear();
    state.adapters[0].driver_info.clear();
    auto result = create(state);
    REQUIRE(result);
    const auto selected = result.value()->adapter();
    REQUIRE(selected);
    CHECK(selected.value().name == state.adapters[0].name);
    CHECK(selected.value().driver_name.empty());
    CHECK(selected.value().driver_info.empty());
}

TEST_CASE("graphics selected reports own their values and remain available after close",
          "[graphics-device]") {
    BackendState state;
    auto result = create(state);
    REQUIRE(result);
    auto graphics = std::move(result).value();
    state.adapters.clear();
    auto first = graphics->adapter();
    REQUIRE(first);
    CHECK(first.value().uuid == uuid_a);
    CHECK(first.value().vendor_id == 0x1234);
    CHECK(first.value().device_id == 0x5678);
    CHECK(first.value().driver_version == 7);
    CHECK(first.value().queue_family == 2);
    first.value().name = "Caller edited value";
    auto second = graphics->adapter();
    REQUIRE(second);
    CHECK(second.value().name == "Test graphics adapter");
    REQUIRE(graphics->close());
    auto closed = graphics->adapter();
    REQUIRE(closed);
    CHECK(closed.value().uuid == uuid_a);
    CHECK(closed.value().name == "Test graphics adapter");
}

TEST_CASE("graphics methods reject a different thread before touching the backend",
          "[graphics-device]") {
    BackendState state;
    auto result = create(state);
    REQUIRE(result);
    auto graphics = std::move(result).value();
    std::optional<Result<GraphicsAdapter>> selected;
    std::optional<Result<GraphicsDiagnostics>> diagnostics;
    std::optional<Result<void>> closed;
    std::thread worker([&] {
        selected.emplace(graphics->adapter());
        diagnostics.emplace(graphics->diagnostics());
        closed.emplace(graphics->close());
    });
    worker.join();
    REQUIRE(selected.has_value());
    REQUIRE(diagnostics.has_value());
    REQUIRE(closed.has_value());
    check_error(*selected, ErrorCode::conflict);
    check_error(*diagnostics, ErrorCode::conflict);
    check_error(*closed, ErrorCode::conflict);
    CHECK(state.recorded() == std::vector<Stage>(startup_stages.begin(), startup_stages.end()));
    REQUIRE(graphics->close());
    CHECK(state.recorded() == complete_lifetime());
    std::thread destroy_closed([owned = std::move(graphics)]() mutable { owned.reset(); });
    destroy_closed.join();
    CHECK(state.recorded() == complete_lifetime());
    CHECK(state.destructor_count == 1);
}

TEST_CASE("graphics validation can be explicitly disabled but defaults to required",
          "[graphics-device]") {
    CHECK(GraphicsConfig{}.validation == GraphicsValidation::required);
    BackendState state;
    GraphicsConfig config;
    config.validation = GraphicsValidation::off;
    auto result = create(state, config);
    REQUIRE(result);
    CHECK(state.validation == GraphicsValidation::off);
}

TEST_CASE("graphics warning diagnostics are cumulative owned values and do not fail cleanup",
          "[graphics-device]") {
    BackendState state;
    state.warning_on_instance = true;
    auto result = create(state);
    REQUIRE(result);
    auto graphics = std::move(result).value();
    auto first = graphics->diagnostics();
    REQUIRE(first);
    CHECK(first.value().warnings == 1);
    CHECK(first.value().errors == 0);
    REQUIRE(first.value().messages.size() == 1);
    first.value().messages[0] = "Edited snapshot";
    state.diagnostics->record(false, "Another warning");
    REQUIRE(graphics->close());
    auto after = graphics->diagnostics();
    REQUIRE(after);
    CHECK(after.value().warnings == 2);
    CHECK(after.value().errors == 0);
    CHECK(after.value().dropped == 0);
    REQUIRE(after.value().messages.size() == 2);
    CHECK(after.value().messages[0] == "Fixture instance warning");
    CHECK(after.value().messages[1] == "Another warning");
    REQUIRE(graphics->close());
}

TEST_CASE("graphics close retains callback errors including instance destruction",
          "[graphics-device]") {
    for (const bool during_cleanup : {false, true}) {
        BackendState state;
        state.error_on_instance_destruction = during_cleanup;
        auto result = create(state);
        REQUIRE(result);
        auto graphics = std::move(result).value();
        if (!during_cleanup) {
            state.diagnostics->record(true, "Fixture device error");
        }
        CAPTURE(during_cleanup);
        check_error(graphics->close(), ErrorCode::execution_failed);
        CHECK(state.recorded() == complete_lifetime());
        auto diagnostics = graphics->diagnostics();
        REQUIRE(diagnostics);
        CHECK(diagnostics.value().errors == 1);
        CHECK(diagnostics.value().warnings == 0);
        REQUIRE(diagnostics.value().messages.size() == 1);
        CHECK(diagnostics.value().messages[0] ==
              (during_cleanup ? "Fixture instance destruction error" : "Fixture device error"));
        check_error(graphics->close(), ErrorCode::execution_failed);
        CHECK(state.recorded() == complete_lifetime());
    }
}

TEST_CASE("graphics diagnostic sink bounds count text and retention without borrowed strings",
          "[graphics-device]") {
    GraphicsDiagnosticSink sink;
    auto empty = sink.snapshot();
    CHECK(empty.errors == 0);
    CHECK(empty.warnings == 0);
    CHECK(empty.dropped == 0);
    CHECK(empty.messages.empty());
    std::string mutable_text = "Owned diagnostic";
    sink.record(false, mutable_text.c_str());
    mutable_text = "Modified source";
    const std::string long_message(1024, 'x');
    sink.record(true, long_message.c_str());
    for (std::size_t index = 2; index < 80; ++index) {
        sink.record(index % 2 == 0, "Bounded message");
    }
    auto snapshot = sink.snapshot();
    CHECK(snapshot.warnings == 40);
    CHECK(snapshot.errors == 40);
    CHECK(snapshot.dropped >= 16);
    REQUIRE(snapshot.messages.size() == 64);
    CHECK(snapshot.messages[0] == "Owned diagnostic");
    CHECK(snapshot.messages[1].starts_with("xxxxxxxx"));
    CHECK(std::all_of(snapshot.messages.begin(), snapshot.messages.end(),
                      [](const auto& text) { return text.size() <= 511; }));
    snapshot.messages[0] = "Changed snapshot";
    CHECK(sink.snapshot().messages[0] == "Owned diagnostic");
}

TEST_CASE("graphics diagnostic callbacks tolerate null text and concurrent bounded writers",
          "[graphics-device]") {
    GraphicsDiagnosticSink null_sink;
    null_sink.record(true, nullptr);
    auto null_snapshot = null_sink.snapshot();
    CHECK(null_snapshot.errors == 1);
    REQUIRE(null_snapshot.messages.size() == 1);
    CHECK(null_snapshot.messages[0].size() <= 511);

    GraphicsDiagnosticSink sink;
    std::array<std::thread, 8> writers;
    for (std::size_t index = 0; index < writers.size(); ++index) {
        writers[index] = std::thread([&, index] {
            for (std::size_t count = 0; count < 1000; ++count) {
                sink.record(index % 2 == 0, "Concurrent immutable diagnostic");
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    const auto snapshot = sink.snapshot();
    CHECK(snapshot.errors == 4000);
    CHECK(snapshot.warnings == 4000);
    CHECK(snapshot.dropped == 7936);
    REQUIRE(snapshot.messages.size() == 64);
    CHECK(std::all_of(snapshot.messages.begin(), snapshot.messages.end(),
                      [](const auto& text) { return text == "Concurrent immutable diagnostic"; }));
}

TEST_CASE("graphics snapshots escape native bytes into bounded readable UTF8 diagnostics",
          "[graphics-device]") {
    GraphicsDiagnosticSink sink;
    std::array<char, 4> native{'A', static_cast<char>(0xFF), '\n', '\0'};
    sink.record(true, native.data());
    native[0] = 'B';
    std::string oversized(1024, static_cast<char>(0xFF));
    sink.record(false, oversized.c_str());
    const auto snapshot = sink.snapshot();
    CHECK(snapshot.errors == 1);
    CHECK(snapshot.warnings == 1);
    CHECK(snapshot.dropped >= 1);
    REQUIRE(snapshot.messages.size() == 2);
    CHECK(snapshot.messages[0] == "A\\xff\\x0a");
    CHECK(snapshot.messages[1].size() <= 511 * 4);
    CHECK(snapshot.messages[1].starts_with("\\xff\\xff"));
    CHECK(std::all_of(snapshot.messages[1].begin(), snapshot.messages[1].end(),
                      [](char value) { return value >= 32 && value < 127; }));
}
