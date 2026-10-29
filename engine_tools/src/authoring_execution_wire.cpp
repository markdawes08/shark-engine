#include "authoring_execution_wire.hpp"
#include "authoring_asset_wire.hpp"
#include "authoring_wire.hpp"
#include "utf8.hpp"

#include <charconv>
#include <cmath>
#include <set>

namespace shark::tools::authoring_execution_wire {
namespace {
using Json = nlohmann::json;
struct Failure {
    Error error;
};
[[noreturn]] void fail(const char* message) {
    throw Failure{
        {ErrorCode::invalid_argument, message, {.operation = "authoring.execution.decode"}}};
}
template <typename T> T take(Result<T> value) {
    if (!value)
        throw Failure{std::move(value).error()};
    return std::move(value).value();
}
template <typename T, typename F> Result<T> boundary(F&& f) {
    try {
        return f();
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot decode execution request",
                     {.operation = "authoring.execution.decode"}};
    }
}
void fields(const Json& value, std::initializer_list<std::string_view> allowed) {
    auto result = authoring_asset_wire::require_fields(value, allowed);
    if (!result)
        throw Failure{std::move(result).error()};
}
const Json& required(const Json& value, const char* field) {
    if (!value.contains(field))
        fail("Required execution parameter is missing");
    return value.at(field);
}
std::string text(const Json& value, std::size_t maximum) {
    if (!value.is_string())
        fail("Expected a bounded UTF-8 string");
    auto result = value.get<std::string>();
    if (result.empty() || result.size() > maximum || !shark::detail::valid_utf8(result, true))
        fail("Expected a nonempty bounded UTF-8 string");
    return result;
}
std::filesystem::path path(const Json& value) {
    const auto result = text(value, 4096);
    return std::filesystem::path{
        std::u8string{reinterpret_cast<const char8_t*>(result.data()), result.size()}};
}
std::uint64_t decimal(const Json& value) {
    const auto result = text(value, 20);
    if (result.size() > 1 && result.front() == '0')
        fail("Expected a canonical uint64 decimal string");
    std::uint64_t number{};
    const auto parsed = std::from_chars(result.data(), result.data() + result.size(), number);
    if (result.front() < '0' || result.front() > '9' || parsed.ec != std::errc{} ||
        parsed.ptr != result.data() + result.size())
        fail("Expected a canonical uint64 decimal string");
    return number;
}
std::uint32_t count(const Json& value, std::size_t maximum, bool nonzero = true) {
    const auto result = take(authoring_asset_wire::parse_index(value, maximum, "/params"));
    if (nonzero && result == 0)
        fail("Expected a positive bounded count");
    return static_cast<std::uint32_t>(result);
}
Json object(Json properties, Json required_fields = Json::array()) {
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required_fields)},
            {"additionalProperties", false}};
}
Json integer(std::uint32_t maximum, std::uint32_t minimum = 1) {
    return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}
Json string(std::size_t maximum) {
    return {{"type", "string"}, {"minLength", 1}, {"maxLength", maximum}};
}
Json decimal_schema() {
    return {{"type", "string"},
            {"pattern", "^(0|[1-9][0-9]{0,19})$"},
            {"maximum_decimal", "18446744073709551615"}};
}
Json revision_schema() {
    return object({{"scene_instance", decimal_schema()}, {"revision", decimal_schema()}},
                  {"scene_instance", "revision"});
}
} // namespace

Result<AuthoringRunRequest> parse_run(const Json& value) {
    return boundary<AuthoringRunRequest>([&] {
        fields(value, {"expected_revision", "seed", "ticks", "hz", "run_id", "revision",
                       "expected_entities"});
        AuthoringRunRequest request{
            take(authoring_wire::parse_revision(required(value, "expected_revision"))), {}};
        if (value.contains("seed"))
            request.config.seed = decimal(value.at("seed"));
        if (value.contains("ticks"))
            request.config.ticks = count(value.at("ticks"), scene_run_maximum_ticks);
        if (value.contains("hz"))
            request.config.tick_rate_hz = count(value.at("hz"), 1000);
        if (value.contains("run_id"))
            request.config.run_id = text(value.at("run_id"), 64);
        if (value.contains("revision"))
            request.config.revision = text(value.at("revision"), 64);
        if (value.contains("expected_entities"))
            request.expected_entities = count(value.at("expected_entities"), 4096, false);
        return request;
    });
}
Result<ClientExportRequest> parse_content(const Json& value) {
    return boundary<ClientExportRequest>([&] {
        return ClientExportRequest{
            AuthoredEntityId{take(StableId::parse(text(required(value, "camera_id"), 36)))},
            take(authoring_wire::parse_revision(required(value, "expected_revision"))),
            take(authoring_asset_wire::parse_catalog_revision(required(value, "expected_catalog"))),
            take(authoring_asset_wire::parse_cache_revision(required(value, "expected_cache"))),
            take(authoring_asset_wire::parse_target(required(value, "target")))};
    });
}
Result<AuthoringExportRequest> parse_export(const Json& value) {
    return boundary<AuthoringExportRequest>([&] {
        fields(value, {"expected_revision", "expected_catalog", "expected_cache", "camera_id",
                       "target", "build_dir", "crt_dir", "shaders", "output", "notices"});
        AuthoringExportRequest result{path(required(value, "build_dir")),
                                      path(required(value, "crt_dir")),
                                      path(required(value, "shaders")),
                                      path(required(value, "output")),
                                      {}};
        if (value.contains("notices")) {
            const auto& notices = value.at("notices");
            if (!notices.is_array() || notices.size() > client_export_maximum_sources)
                fail("Invalid bounded content notice inventory");
            std::set<std::string> seen;
            for (const auto& notice : notices) {
                fields(notice, {"license", "path"});
                auto license = text(required(notice, "license"), 128);
                if (!seen.insert(license).second)
                    fail("Duplicate content notice license");
                result.notices.emplace_back(std::move(license), path(required(notice, "path")));
            }
        }
        return result;
    });
}
Result<AuthoringCaptureRequest> parse_capture(const Json& value) {
    return boundary<AuthoringCaptureRequest>([&] {
        fields(value, {"expected_revision", "expected_catalog", "expected_cache", "camera_id",
                       "target", "output", "width", "height", "local_origin"});
        AuthoringCaptureRequest result{path(required(value, "output"))};
        if (value.contains("width"))
            result.viewport.width = count(value.at("width"), authoring_capture_maximum_extent);
        if (value.contains("height"))
            result.viewport.height = count(value.at("height"), authoring_capture_maximum_extent);
        if (std::uint64_t{result.viewport.width} * result.viewport.height >
            authoring_capture_maximum_pixels)
            fail("Capture exceeds one million pixels");
        if (value.contains("local_origin")) {
            const auto& origin = value.at("local_origin");
            if (!origin.is_array() || origin.size() != 3)
                fail("local_origin must have three finite coordinates");
            for (int i = 0; i < 3; ++i) {
                if (!origin.at(static_cast<std::size_t>(i)).is_number())
                    fail("local_origin must be numeric");
                const auto number = origin.at(static_cast<std::size_t>(i)).get<double>();
                if (!std::isfinite(number) || std::abs(number) > render_coordinate_limit)
                    fail("local_origin exceeds local coordinate bounds");
                result.origin.scene_local_offset[i] = number;
            }
        }
        return result;
    });
}

Json methods(bool export_available, bool capture_available) {
    Json result = Json::array();
    const Json revision = revision_schema();
    const Json example_revision{{"scene_instance", "1"}, {"revision", "0"}};
    const auto add = [&](const char* name, Json params, Json output, Json example) {
        result.push_back({{"name", name},
                          {"execution_context", "cpu_owner_thread"},
                          {"requires_open_project", true},
                          {"requires_project_root", false},
                          {"params", std::move(params)},
                          {"result", std::move(output)},
                          {"example",
                           {{"jsonrpc", "2.0"},
                            {"protocol_version", 1},
                            {"id", "example"},
                            {"method", name},
                            {"params", std::move(example)}}}});
    };
    auto run_token = string(64);
    run_token["pattern"] = "^[A-Za-z0-9._-]{1,64}$";
    add("scene.run",
        object({{"expected_revision", revision},
                {"seed", decimal_schema()},
                {"ticks", integer(scene_run_maximum_ticks)},
                {"hz", integer(1000)},
                {"run_id", run_token},
                {"revision", run_token},
                {"expected_entities", integer(4096, 0)}},
               {"expected_revision"}),
        object({{"observed_revision", revision},
                {"dirty", {{"type", "boolean"}}},
                {"entity_count", integer(4096, 0)},
                {"source_digest", string(64)},
                {"report",
                 {{"type", "object"},
                  {"description", "Native shark.scene-run v1 report; inspect PASS/FAIL status. "
                                  "Static integrity, no gameplay or Lua."}}}},
               {"observed_revision", "dirty", "entity_count", "source_digest", "report"}),
        {{"expected_revision", example_revision}, {"ticks", 3}, {"seed", "1337"}});
    Json properties{
        {"expected_revision", revision},
        {"expected_catalog",
         {{"type", "object"},
          {"description", "Current catalog UUID scope and uint64 decimal value"}}},
        {"expected_cache",
         {{"type", "object"},
          {"description", "Current cache UUID scope and uint64 decimal value"}}},
        {"camera_id", {{"type", "string"}, {"format", "uuid"}}},
        {"target", object({{"id", string(64)}, {"side", {{"const", "client"}}}}, {"id", "side"})},
        {"output", string(4096)}};
    const Json required_fields = {"expected_revision", "expected_catalog", "expected_cache",
                                  "camera_id",         "target",           "output"};
    const Json example{
        {"expected_revision", example_revision},
        {"expected_catalog", {{"scope", "30000000-0000-4000-8000-000000000001"}, {"value", "0"}}},
        {"expected_cache", {{"scope", "30000000-0000-4000-8000-000000000002"}, {"value", "0"}}},
        {"camera_id", "20000000-0000-4000-8000-000000000004"},
        {"target", {{"id", "development"}, {"side", "client"}}},
        {"output", "C:/Exports/fresh-output"}};
    if (export_available) {
        auto props = properties;
        auto needed = required_fields;
        auto sample = example;
        for (const auto* field : {"build_dir", "crt_dir", "shaders"}) {
            props[field] = string(4096);
            needed.push_back(field);
            sample[field] = std::string("C:/SharkInputs/") + field;
        }
        props["notices"] = {{"type", "array"},
                            {"maxItems", 128},
                            {"items", object({{"license", string(128)}, {"path", string(4096)}},
                                             {"license", "path"})}};
        add("scene.export", object(props, needed),
            {{"type", "object"},
             {"description", "Existing shark.client-export v1 package report. Check status and "
                             "published; qualification remains NOT_RUN."}},
            sample);
    }
    if (capture_available) {
        auto props = properties;
        props["width"] = integer(authoring_capture_maximum_extent);
        props["height"] = integer(authoring_capture_maximum_extent);
        props["local_origin"] = {{"type", "array"},
                                 {"minItems", 3},
                                 {"maxItems", 3},
                                 {"items",
                                  {{"type", "number"},
                                   {"minimum", -render_coordinate_limit},
                                   {"maximum", render_coordinate_limit}}}};
        add("scene.capture", object(props, required_fields),
            {{"type", "object"},
             {"description", "shark.authoring-capture v1; immutable linear RGBA8/depth files, "
                             "source revision and actual device diagnostics"}},
            example);
    }
    return result;
}
} // namespace shark::tools::authoring_execution_wire
