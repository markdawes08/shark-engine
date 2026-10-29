#include "authoring_export.hpp"

#include "export_content_stage.hpp"
#include "source_json.hpp"

#include <array>
#include <chrono>
#include <fstream>
#include <optional>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace shark::tools {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

#ifdef _WIN32
constexpr std::size_t maximum_message_bytes = 1024 * 1024;
struct Failure {
    Error error;
};
std::string utf8(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
fs::path native_path(const std::string& value) {
    return fs::path{std::u8string{reinterpret_cast<const char8_t*>(value.data()), value.size()}};
}
[[noreturn]] void fail(const char* message, ErrorCode code = ErrorCode::execution_failed,
                       const fs::path& path = {}) {
    throw Failure{{code, message, {.operation = "authoring.export", .path = utf8(path)}}};
}
template <typename T> T take(Result<T> value) {
    if (!value)
        throw Failure{std::move(value).error()};
    return std::move(value).value();
}
void accept(Result<void> value) {
    if (!value)
        throw Failure{std::move(value).error()};
}
void no_links(const fs::path& path) {
    fs::path current;
    for (const auto& part : path) {
        current /= part;
        std::error_code error;
        const auto status = fs::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory)
            continue;
        if (error)
            fail("Cannot inspect export path", ErrorCode::io_failure, current);
        const auto attributes = GetFileAttributesW(current.c_str());
        if (fs::is_symlink(status) || (attributes != INVALID_FILE_ATTRIBUTES &&
                                       (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0))
            fail("Export paths must not contain links", ErrorCode::invalid_path, current);
    }
}
fs::path fresh_destination(const fs::path& output, const fs::path& project_root) {
    const auto absolute = fs::absolute(output).lexically_normal();
    no_links(absolute);
    if (absolute.filename().empty() || fs::exists(absolute))
        fail("Export output must be a fresh named directory", ErrorCode::conflict, absolute);
    static_cast<void>(take(ProjectPath::parse(utf8(absolute.filename()))));
    const auto parent = fs::canonical(absolute.parent_path());
    if (!fs::is_directory(parent))
        fail("Export parent must exist", ErrorCode::invalid_path, parent);
    for (auto ancestor = parent; !ancestor.empty();) {
        if (fs::equivalent(ancestor, project_root))
            fail("Export output must be outside the source project root",
                 ErrorCode::path_outside_root, absolute);
        const auto next = ancestor.parent_path();
        if (next == ancestor)
            break;
        ancestor = next;
    }
    return parent / absolute.filename();
}
struct Scratch {
    fs::path parent;
    fs::path path;
    explicit Scratch(const fs::path& directory) : parent(fs::canonical(directory)) {
        path = parent / (".runtime-export-" + take(generate_stable_id()).to_string());
        if (!fs::create_directory(path))
            fail("Cannot create owned export scratch", ErrorCode::conflict, path);
    }
    void clean() {
        no_links(path);
        if (fs::canonical(path).parent_path() != parent ||
            !path.filename().wstring().starts_with(L".runtime-export-"))
            fail("Refusing cleanup outside owned export scratch", ErrorCode::invalid_path, path);
        for (const auto& entry : fs::recursive_directory_iterator(path))
            no_links(entry.path());
        fs::remove_all(path);
        path.clear();
    }
    ~Scratch() {
        if (!path.empty()) {
            try {
                clean();
            } catch (...) { /* Preserve unexpected cleanup inputs. */
            }
        }
    }
};
void write_json(const fs::path& path, const Json& value) {
    const auto bytes = value.dump();
    if (bytes.size() > maximum_message_bytes)
        fail("Export control record exceeds its bound");
    if (fs::exists(path))
        fail("Export control file already exists", ErrorCode::conflict, path);
    std::ofstream file{path, std::ios::binary};
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file)
        fail("Cannot write export control record", ErrorCode::io_failure, path);
}

class Handle final {
  public:
    HANDLE value{nullptr};
    Handle() = default;
    ~Handle() {
        reset();
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset(HANDLE replacement = nullptr) noexcept {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
        value = replacement;
    }
};

// Windows CRT argument quoting; no shell is involved.
std::wstring quote(const fs::path& path) {
    std::wstring result{L"\""};
    std::size_t slashes = 0;
    for (const auto value : path.native()) {
        if (value == L'\\') {
            ++slashes;
            continue;
        }
        result.append(value == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result.push_back(value);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

class Child final {
  public:
    Child(const fs::path& python, const fs::path& tool, const fs::path& request,
          const fs::path& cwd)
        : deadline_(std::chrono::steady_clock::now() + std::chrono::seconds{60}) {
        Handle input_read, output_write, error_write;
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        if (!CreatePipe(&input_read.value, &input_.value, &security, 65536) ||
            !CreatePipe(&output_.value, &output_write.value, &security, 65536) ||
            !CreatePipe(&error_.value, &error_write.value, &security, 65536) ||
            !SetHandleInformation(input_.value, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(output_.value, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(error_.value, HANDLE_FLAG_INHERIT, 0))
            fail("Cannot create bounded export helper pipes");
        job_.reset(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job_.value || !SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation,
                                                    &limits, sizeof(limits)))
            fail("Cannot create owned export process tree");
        SIZE_T size = 0;
        static_cast<void>(InitializeProcThreadAttributeList(nullptr, 1, 0, &size));
        std::vector<std::byte> storage(size);
        auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size))
            fail("Cannot initialize export helper handle inventory");
        struct AttributeGuard {
            PPROC_THREAD_ATTRIBUTE_LIST value;
            ~AttributeGuard() {
                DeleteProcThreadAttributeList(value);
            }
        } guard{attributes};
        std::array handles{input_read.value, output_write.value, error_write.value};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       handles.data(), sizeof(handles), nullptr, nullptr))
            fail("Cannot restrict inherited export helper handles");
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input_read.value;
        startup.StartupInfo.hStdOutput = output_write.value;
        startup.StartupInfo.hStdError = error_write.value;
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION info{};
        auto command =
            quote(python) + L" " + quote(tool) + L" authoring-export --request " + quote(request);
        if (command.size() >= 32767 ||
            !CreateProcessW(python.c_str(), command.data(), nullptr, nullptr, TRUE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                            nullptr, cwd.c_str(), &startup.StartupInfo, &info))
            fail("Cannot launch configured export helper", ErrorCode::execution_failed, python);
        process_.reset(info.hProcess);
        Handle thread;
        thread.reset(info.hThread);
        if (!AssignProcessToJobObject(job_.value, process_.value) ||
            ResumeThread(thread.value) == DWORD(-1)) {
            static_cast<void>(TerminateProcess(process_.value, 2));
            static_cast<void>(WaitForSingleObject(process_.value, 1000));
            fail("Cannot admit export helper into its owned process tree");
        }
    }
    ~Child() {
        input_.reset();
        if (job_.value)
            static_cast<void>(TerminateJobObject(job_.value, 2));
        if (process_.value)
            static_cast<void>(WaitForSingleObject(process_.value, 1000));
    }
    void send(std::string_view command) {
        remaining();
        DWORD written = 0;
        if (command.size() > 16 || WaitForSingleObject(process_.value, 0) != WAIT_TIMEOUT ||
            !WriteFile(input_.value, command.data(), static_cast<DWORD>(command.size()), &written,
                       nullptr) ||
            written != command.size())
            fail("Export helper stopped before its owner command");
    }
    Json response() {
        while (true) {
            collect();
            const auto newline = pending_.find('\n');
            if (newline != std::string::npos) {
                auto line = pending_.substr(0, newline);
                pending_.erase(0, newline + 1);
                return take(detail::parse_source_json(line, maximum_message_bytes, 8192));
            }
            if (WaitForSingleObject(process_.value, 0) == WAIT_OBJECT_0)
                fail("Export helper exited without its complete response");
            remaining();
            // Wait on process exit while periodically consuming bounded pipes;
            // no native worker or authoring scheduler is introduced.
            static_cast<void>(WaitForSingleObject(process_.value, 10));
        }
    }
    void finish(DWORD expected) {
        input_.reset();
        while (WaitForSingleObject(process_.value, 0) == WAIT_TIMEOUT) {
            collect();
            remaining();
            static_cast<void>(WaitForSingleObject(process_.value, 10));
        }
        collect();
        DWORD code = 0;
        if (!GetExitCodeProcess(process_.value, &code) || code != expected || !pending_.empty() ||
            !diagnostics_.empty())
            fail("Export helper completion or machine streams are inconsistent");
    }

  private:
    void remaining() const {
        if (std::chrono::steady_clock::now() >= deadline_)
            fail("Export helper exceeded its 60-second deadline");
    }
    void read(HANDLE pipe, std::string& destination, std::size_t maximum) {
        std::array<char, 8192> bytes{};
        while (true) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE)
                    return;
                fail("Cannot inspect export helper output");
            }
            if (!available)
                return;
            DWORD count = 0;
            const auto requested =
                available < bytes.size() ? available : static_cast<DWORD>(bytes.size());
            if (!ReadFile(pipe, bytes.data(), requested, &count, nullptr))
                fail("Cannot read export helper output");
            if (count > maximum - destination.size())
                fail("Export helper output exceeds its bound");
            destination.append(bytes.data(), count);
        }
    }
    void collect() {
        read(output_.value, pending_, maximum_message_bytes);
        read(error_.value, diagnostics_, 65536);
    }
    Handle job_, process_, input_, output_, error_;
    std::chrono::steady_clock::time_point deadline_;
    std::string pending_, diagnostics_;
};
bool report(const Json& value) {
    return value.is_object() && value.value("format", std::string{}) == "shark.client-export";
}
void expect(const Json& value, std::string_view status) {
    if (!value.is_object() ||
        value.value("format", std::string{}) != "shark.authoring-export-helper" ||
        value.value("schema_version", 0) != 1 || value.value("status", std::string{}) != status)
        fail("Unexpected export helper protocol state");
    const auto count = status == "INPUTS" ? std::size_t{5} : std::size_t{3};
    if (value.size() != count)
        fail("Unexpected export helper response fields");
}
void validate_report(const Json& value) {
    if (!report(value) || !value.contains("published") || !value["published"].is_boolean() ||
        value.value("schema_version", 0) != 1 || value.value("renderer_initialized", true) ||
        value.value("qualification", std::string{}) != "NOT_RUN")
        fail("Invalid export helper completion report");
    const auto status = value.value("status", std::string{});
    const auto published = value["published"].get<bool>();
    if ((status != "PASS" && status != "FAIL" && status != "ERROR") ||
        (status == "PASS" && !published) || (status == "ERROR" && published))
        fail("Inconsistent export helper publication outcome");
}
#endif
} // namespace

Result<AuthoringArtifactResult>
export_authoring_scene(const PreparedClientExport& candidate, const fs::path& project_root,
                       const fs::path& python, const fs::path& tool,
                       const AuthoringExportRequest& request,
                       const std::function<Result<void>()>& validate) {
#ifdef _WIN32
    bool publication_granted = false;
    std::optional<Json> completion;
    try {
        if (!validate)
            fail("Export requires final owner validation", ErrorCode::invalid_argument);
        const auto executable = take(checked_export_file(python));
        const auto script = take(checked_export_file(tool));
        const auto output = fresh_destination(request.output, project_root);
        if (request.notices.size() > client_export_maximum_sources)
            fail("Export notices exceed the source inventory bound", ErrorCode::invalid_argument);
        Scratch scratch{output.parent_path()};
        Json notices = Json::array();
        for (const auto& [license, path] : request.notices)
            notices.push_back({{"license", license}, {"path", utf8(fs::absolute(path))}});
        const auto request_file = scratch.path / "request.json";
        const auto content_directory = scratch.path / "content";
        const auto content_record = scratch.path / "content.json";
        write_json(request_file, {{"format", "shark.authoring-export-request"},
                                  {"schema_version", 1},
                                  {"project_root", utf8(fs::absolute(project_root))},
                                  {"build_dir", utf8(fs::absolute(request.build_dir))},
                                  {"crt_dir", utf8(fs::absolute(request.crt_dir))},
                                  {"shaders", utf8(fs::absolute(request.shaders))},
                                  {"output", utf8(output)},
                                  {"content_directory", utf8(content_directory)},
                                  {"content_record", utf8(content_record)},
                                  {"notices", std::move(notices)}});
        {
            Child child{executable, script, request_file, scratch.path};
            auto response = child.response();
            if (!report(response)) {
                expect(response, "INPUTS");
                const auto stage = take(stage_client_export(
                    candidate, project_root, native_path(response.at("vertex").get<std::string>()),
                    native_path(response.at("pixel").get<std::string>()), content_directory));
                write_json(content_record, stage.content);
                child.send("ASSEMBLE\n");
                response = child.response();
                if (!report(response)) {
                    expect(response, "READY_TO_PUBLISH");
                    accept(validate());
                    accept(validate_export_content_stage(stage));
                    publication_granted = true;
                    child.send("PUBLISH\n");
                    response = child.response();
                }
            }
            validate_report(response);
            if (response.at("published").get<bool>() && !publication_granted)
                fail("Export helper claimed publication without owner validation");
            completion = std::move(response);
            const auto passed = completion->at("status") == "PASS";
            child.finish(passed ? 0U : (completion->at("published").get<bool>() ? 1U : 2U));
        }
        scratch.clean();
        return AuthoringArtifactResult{completion->dump(), completion->at("published").get<bool>(),
                                       completion->at("status") == "PASS"};
    } catch (...) {
        Error error{ErrorCode::execution_failed,
                    "Export adapter failed",
                    {.operation = "authoring.export", .path = utf8(request.output)}};
        try {
            throw;
        } catch (Failure& failed) {
            error = std::move(failed.error);
        } catch (const fs::filesystem_error&) {
            error.code = ErrorCode::io_failure;
            error.message = "Export filesystem operation failed";
        } catch (...) {
        }
        if (completion) {
            try {
                const auto published = completion->at("published").get<bool>();
                (*completion)["status"] = published ? "FAIL" : "ERROR";
                (*completion)["error"] = {{"code", error_code_name(error.code)},
                                          {"message", error.message}};
                return AuthoringArtifactResult{completion->dump(), published, false};
            } catch (...) {
            }
        }
        if (publication_granted) {
            error.message =
                "Package publication outcome is unknown; inspect the destination before retrying";
            error.context.path = utf8(request.output);
        }
        return error;
    }
#else
    static_cast<void>(candidate);
    static_cast<void>(project_root);
    static_cast<void>(python);
    static_cast<void>(tool);
    static_cast<void>(request);
    static_cast<void>(validate);
    return Error{ErrorCode::capability_unavailable,
                 "Standalone export requires configured native Windows x64 tooling",
                 {.operation = "authoring.export"}};
#endif
}
} // namespace shark::tools
