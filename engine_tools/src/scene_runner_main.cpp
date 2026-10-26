#include "shark/tools/feature_world.hpp"
#include "shark/tools/project_files.hpp"
#include "shark/tools/scene_runner.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
using namespace shark;
using namespace shark::tools;
namespace fs = std::filesystem;

Error cli_error(const char* message, ErrorCode code = ErrorCode::invalid_argument) {
    return {code, message, {.operation = "scene.runner.cli"}};
}
template <typename T> Result<T> integer(std::string_view text) {
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        return cli_error("Expected an in-range decimal integer");
    return value;
}
template <typename T> T fixture_id(char digit) {
    std::string text = "10000000-0000-4000-8000-000000000001";
    text.back() = digit;
    return T{StableId::parse(text).value()};
}

struct Options {
    SceneRunConfig config;
    std::string case_id{"foundation"};
    std::string output;
    std::string project;
    std::string workspace;
};
Result<Options> options(std::span<const std::string> args) {
    Options result;
    std::set<std::string> seen;
    if (args.size() > scene_run_maximum_inputs * 2 + 32)
        return cli_error("Too many runner arguments");
    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto& name = args[index];
        if (name != "--input" && !seen.insert(name).second)
            return cli_error("Duplicate option");
        if (index + 1 == args.size())
            return cli_error("Option requires a value");
        const auto& value = args[++index];
        if (value.size() > 4096)
            return cli_error("Option value exceeds its byte bound");
        if (name == "--seed") {
            auto number = integer<std::uint64_t>(value);
            if (!number)
                return number.error();
            result.config.seed = number.value();
        } else if (name == "--ticks" || name == "--hz") {
            auto number = integer<std::uint32_t>(value);
            if (!number)
                return number.error();
            (name == "--ticks" ? result.config.ticks : result.config.tick_rate_hz) = number.value();
        } else if (name == "--run-id")
            result.config.run_id = value;
        else if (name == "--revision")
            result.config.revision = value;
        else if (name == "--case")
            result.case_id = value;
        else if (name == "--output" && value.empty())
            return cli_error("Output path must not be empty", ErrorCode::invalid_path);
        else if (name == "--output")
            result.output = value;
        else if (name == "--project")
            result.project = value;
        else if (name == "--workspace")
            result.workspace = value;
        else if (name == "--mode") {
            if (value != "cpu")
                return cli_error(
                    "This worker is CPU-only; tools/scene_runner.py provides explicit GPU fixtures",
                    ErrorCode::capability_unavailable);
        } else if (name == "--input") {
            if (result.config.inputs.size() == scene_run_maximum_inputs)
                return cli_error("Too many input events");
            const auto first = value.find(':');
            const auto second = first == std::string::npos ? first : value.find(':', first + 1);
            if (first == std::string::npos || second == std::string::npos)
                return cli_error("Input must be TICK:offset:VALUE");
            auto tick = integer<std::uint64_t>(std::string_view{value}.substr(0, first));
            auto amount = integer<std::int32_t>(std::string_view{value}.substr(second + 1));
            if (!tick)
                return tick.error();
            if (!amount)
                return amount.error();
            const auto action = value.substr(first + 1, second - first - 1);
            if (action != "offset")
                return cli_error("Foundation input action must be offset");
            result.config.inputs.push_back({tick.value(), action, amount.value()});
        } else
            return cli_error("Unknown runner option");
    }
    const bool suite = result.case_id == "featureworld";
    const auto catalog = feature_world_case_ids();
    const bool feature =
        suite || std::find(catalog.begin(), catalog.end(), result.case_id) != catalog.end();
    if (!feature && result.case_id != "foundation" && result.case_id != "intentional-failure")
        return cli_error("Unknown runner case");
    if (feature && result.project.empty())
        return cli_error("FeatureWorld requires --project FILE");
    if (feature && !result.config.inputs.empty())
        return cli_error("FeatureWorld cases have no input actions; offset belongs to foundation");
    if ((suite || result.case_id == "featureworld.persistence" ||
         result.case_id == "featureworld.asset-identity") &&
        result.workspace.empty())
        return cli_error("FeatureWorld persistence requires --workspace EXISTING_DIRECTORY");
    if (!feature && (seen.contains("--project") || seen.contains("--workspace")))
        return cli_error("Project/workspace options require a FeatureWorld case");
    if (auto valid = validate_scene_run_config(result.config); !valid)
        return valid.error();
    return result;
}

Result<void> publish(const fs::path& output, std::string_view text) {
    const auto failure = [&](const char* message, ErrorCode code) {
        auto result = cli_error(message, code);
        result.context.operation = "scene.runner.publish";
        const auto bytes = output.u8string();
        result.context.path.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return result;
    };
    std::error_code error;
    const auto absolute = fs::absolute(output, error);
    if (error || absolute.filename().empty())
        return failure("Invalid result path", ErrorCode::invalid_path);
    if (fs::exists(absolute, error) || error)
        return failure("Result path already exists or cannot be inspected", ErrorCode::conflict);
    auto identity = generate_stable_id();
    if (!identity)
        return identity.error();
    const auto stage =
        absolute.parent_path() / (".shark-run-stage-" + identity.value().to_string());
    if (!fs::create_directory(stage, error))
        return failure("Cannot create result staging directory", ErrorCode::io_failure);
    struct Cleanup {
        fs::path directory;
        ~Cleanup() {
            std::error_code ignored;
            fs::remove(directory / "result.json", ignored);
            fs::remove(directory, ignored);
        }
    } cleanup{stage};
    {
        std::ofstream stream{stage / "result.json", std::ios::binary};
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.close();
        if (!stream)
            return failure("Cannot write complete run result", ErrorCode::io_failure);
    }
    // Same-directory filesystem, atomic no-replace publication on NTFS/ext4.
    // Unsupported hard links fail explicitly; no overwrite/rename fallback.
    fs::create_hard_link(stage / "result.json", absolute, error);
    if (error)
        return failure("Cannot publish run result without replacing another file",
                       ErrorCode::io_failure);
    return {};
}

int emit_error(const Error& error) {
    auto encoded = encode_scene_run_error(error);
    if (encoded)
        std::cout << encoded.value();
    else
        std::cout << "{\"format\":\"shark.scene-run\",\"schema_version\":1,\"status\":\"ERROR\","
                     "\"mode\":\"none\",\"renderer_initialized\":false,\"error\":{\"code\":"
                     "\"execution_failed\",\"message\":\"Cannot encode runner error\"}}\n";
    std::cout.flush();
    return 2;
}

int run(std::span<const std::string> args) {
    if (args.size() == 1 && args.front() == "--help") {
        std::cout
            << "Shark SceneRunner: deterministic CPU scene cases\n"
               "--case foundation|intentional-failure --mode cpu --seed UINT64\n"
               "--ticks 1..10000 --hz 1..1000 --input TICK:offset:INT32 (repeat, tick ordered)\n"
               "--run-id TOKEN --revision TOKEN --output NEW_FILE\n"
               "--case featureworld|featureworld.CASE --project FILE --workspace "
               "EXISTING_DIRECTORY\n"
               "--list-cases emits the versioned native case catalog as JSON.\n"
               "CPU/GPU test supervisor: python tools/scene_runner.py --help\n"
               "Exit: 0 PASS, 1 case failure, 2 arguments/execution/publication failure.\n"
               "CPU execution does not initialize a renderer. FeatureWorld inputs are saved source "
               "files.\n";
        return std::cout ? 0 : 2;
    }
    if (args.size() == 1 && args.front() == "--version") {
        std::cout << "Shark SceneRunner 0.1.0 (result schema 1)\n";
        return std::cout ? 0 : 2;
    }
    if (args.size() == 1 && args.front() == "--list-cases") {
        std::cout << "{\"format\":\"shark.scene-cases\",\"schema_version\":1,\"mode\":\"cpu\","
                     "\"cases\":[\"foundation\",\"intentional-failure\"";
        for (const auto entry : feature_world_case_ids())
            std::cout << ",\"" << entry << "\"";
        std::cout << "],\"suites\":{\"featureworld\":[";
        bool first = true;
        for (const auto entry : feature_world_case_ids()) {
            if (!first)
                std::cout << ',';
            std::cout << '\"' << entry << '\"';
            first = false;
        }
        std::cout << "]}}\n";
        std::cout.flush();
        return std::cout ? 0 : 2;
    }
    auto parsed = options(args);
    if (!parsed)
        return emit_error(parsed.error());
    const auto& opt = parsed.value();
    const auto project_id = fixture_id<ProjectId>('1');
    const auto scene_id = fixture_id<SceneId>('3');
    const auto entity_id = fixture_id<AuthoredEntityId>('4');
    ProjectDocument project{project_id,
                            fixture_id<WorldId>('2'),
                            Name{"Runner foundation"},
                            scene_id,
                            {{scene_id, ProjectPath::parse("foundation.sharkscene").value()}}};
    SceneDocument scene{project_id, scene_id, {}, {}};
    SceneRunCase test{.id = opt.case_id};
    test.setup = [entity_id](SceneRunContext& context) -> Result<void> {
        auto status = context.owner().status();
        if (!status)
            return status.error();
        auto reply = context.owner().execute(CreateEntityCommand{entity_id, Name{"Runner entity"}},
                                             status.value().revision);
        if (!reply)
            return reply.error();
        if (!reply.value().outcome)
            return reply.value().outcome.error();
        return context.require(reply.value().outcome.value().changed,
                               "Foundation entity was not created", entity_id);
    };
    test.tick = [entity_id, intentional = opt.case_id == "intentional-failure"](
                    SceneRunContext& context, const SceneRunTick& tick) -> Result<void> {
        if (intentional)
            return context.require(false, "Intentional scene assertion", entity_id);
        auto random = context.random_u64();
        if (!random)
            return random.error();
        std::int64_t offset = 0;
        for (const auto& event : tick.inputs)
            offset += event.value;
        Transform transform;
        transform.translation.x = static_cast<double>(tick.index) + static_cast<double>(offset);
        transform.translation.y = static_cast<double>(random.value() % 1024);
        auto status = context.owner().status();
        if (!status)
            return status.error();
        auto reply = context.owner().execute(SetTransformCommand{entity_id, transform},
                                             status.value().revision);
        if (!reply)
            return reply.error();
        if (!reply.value().outcome)
            return reply.value().outcome.error();
        auto capture = context.owner().capture(reply.value().observed_revision);
        if (!capture)
            return capture.error();
        return context.require(capture.value().scene.entities.size() == 1 &&
                                   capture.value().scene.entities.front().transform == transform,
                               "Tick transform differs from deterministic input", entity_id);
    };
    std::vector<SceneRunCase> cases{test};
    std::unique_ptr<ProjectFiles> files;
    if (!opt.project.empty()) {
        const auto path = fs::path{std::u8string{
            reinterpret_cast<const char8_t*>(opt.project.data()), opt.project.size()}};
        std::error_code error;
        const auto absolute = fs::absolute(path, error);
        if (error)
            return emit_error(cli_error("Invalid project file path", ErrorCode::invalid_path));
        auto opened = ProjectFiles::open(absolute.parent_path());
        if (!opened)
            return emit_error(opened.error());
        files = std::move(opened).value();
        const auto filename = absolute.filename().u8string();
        auto relative = ProjectPath::parse(
            std::string_view{reinterpret_cast<const char*>(filename.data()), filename.size()});
        if (!relative)
            return emit_error(relative.error());
        auto loaded_project = files->read_project(relative.value());
        if (!loaded_project)
            return emit_error(loaded_project.error());
        project = std::move(loaded_project).value().source.document;
        auto loaded_scene = files->read_scene(project, project.startup_scene);
        if (!loaded_scene)
            return emit_error(loaded_scene.error());
        scene = std::move(loaded_scene).value().source.document;
        const auto scratch = fs::path{std::u8string{
            reinterpret_cast<const char8_t*>(opt.workspace.data()), opt.workspace.size()}};
        cases = feature_world_cases(scratch);
        if (opt.case_id != "featureworld")
            std::erase_if(cases,
                          [&](const SceneRunCase& entry) { return entry.id != opt.case_id; });
    }
    auto report = run_scene_cases(project, scene, opt.config, cases);
    if (!report)
        return emit_error(report.error());
    auto encoded = encode_scene_run_report(report.value(), opt.output);
    if (!encoded)
        return emit_error(encoded.error());
    if (!opt.output.empty()) {
        const auto& path = opt.output;
        auto written = publish(
            fs::path{std::u8string{reinterpret_cast<const char8_t*>(path.data()), path.size()}},
            encoded.value());
        if (!written)
            return emit_error(written.error());
    }
    std::cout << encoded.value();
    std::cout.flush();
    if (!std::cout)
        return 2;
    return report.value().status == SceneRunStatus::passed ? 0 : 1;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> args;
        for (int index = 1; index < argc; ++index) {
            const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1,
                                                  nullptr, 0, nullptr, nullptr);
            if (size <= 0)
                return emit_error(cli_error("Argument is not valid Unicode"));
            std::string text(static_cast<std::size_t>(size), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1, text.data(),
                                    size, nullptr, nullptr) != size)
                return emit_error(cli_error("Cannot convert Unicode argument"));
            text.pop_back();
            args.push_back(std::move(text));
        }
        return run(args);
    } catch (...) {
        return emit_error(cli_error("Runner failed", ErrorCode::execution_failed));
    }
}
#else
int main(int argc, char** argv) {
    try {
        std::vector<std::string> args;
        for (int index = 1; index < argc; ++index)
            args.emplace_back(argv[index]);
        return run(args);
    } catch (...) {
        return emit_error(cli_error("Runner failed", ErrorCode::execution_failed));
    }
}
#endif
