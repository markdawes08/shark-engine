#include "shark/tools/authoring_protocol.hpp"

#include <nlohmann/json.hpp>

#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace {
using namespace shark;
using namespace shark::tools;
using Json = nlohmann::json;

constexpr std::string_view help =
    "SharkAuthor - local CPU project authoring\n"
    "Usage: SharkAuthor --stdio|--request [--root DIRECTORY] [--project RELATIVE_PATH]\n"
    "       SharkAuthor --help|--version\n"
    "--stdio   Read bounded UTF-8 JSON-RPC requests, one per LF-terminated line.\n"
    "--request Read one bounded JSON-RPC request from stdin through EOF.\n"
    "--root    Fix the project root for this process; omitted permits discovery only.\n"
    "--project Open this source project before processing requests (requires --root).\n"
    "Discover methods with protocol.discover and protocol_version:1. No TTY is needed.\n"
    "Machine stdout contains only JSON responses; startup errors go to stderr.\n"
    "Exit: 0 completed, 1 one-shot request failed, 2 startup/framing/I/O failure.\n";

bool output(std::ostream& stream, std::string_view text) {
    stream << text << '\n' << std::flush;
    return static_cast<bool>(stream);
}

std::string diagnostic(int rpc_code, const Error& error, std::string_view reason = {}) {
    const auto& value = error.context;
    Json context{{"operation", value.operation},
                 {"request_id", value.request_id},
                 {"path", value.path},
                 {"entity_id", value.entity_id},
                 {"asset_id", value.asset_id}};
    Json data{{"code", error_code_name(error.code)},
              {"context", std::move(context)},
              {"protocol_version", authoring_protocol_version}};
    if (!reason.empty())
        data["reason"] = reason;
    Json detail{{"code", rpc_code}, {"message", error.message}, {"data", std::move(data)}};
    Json response{{"jsonrpc", "2.0"},
                  {"protocol_version", authoring_protocol_version},
                  {"id", nullptr},
                  {"error", std::move(detail)}};
    return response.dump();
}

int failure(std::ostream& stream, std::string_view reason, std::string_view message) {
    const Error error{
        ErrorCode::invalid_argument, std::string(message), {.operation = "authoring.transport"}};
    static_cast<void>(output(stream, diagnostic(-32600, error, reason)));
    return 2;
}

int service_failure(const Error& error) {
    static_cast<void>(output(std::cerr, diagnostic(-32603, error)));
    return 2;
}

int run(const std::vector<std::string>& arguments) {
    if (arguments.empty() || (arguments.size() == 1 && arguments.front() == "--help")) {
        std::cout << help << std::flush;
        return std::cout ? 0 : 2;
    }
    if (arguments.size() == 1 && arguments.front() == "--version") {
        return output(std::cout, "SharkAuthor protocol 1 (JSON-RPC 2.0)") ? 0 : 2;
    }
    std::optional<bool> stream_mode;
    std::optional<std::string> root;
    std::optional<std::string> project;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument == "--stdio" || argument == "--request") {
            if (stream_mode)
                return failure(std::cerr, "arguments", "Select exactly one transport mode");
            stream_mode = argument == "--stdio";
        } else if (argument == "--root" || argument == "--project") {
            auto& destination = argument == "--root" ? root : project;
            if (destination || ++index == arguments.size() || arguments[index].empty())
                return failure(std::cerr, "arguments", "Expected one nonempty path per option");
            destination = arguments[index];
        } else {
            return failure(std::cerr, "arguments", "Unknown authoring option; use --help");
        }
    }
    if (!stream_mode || (project && !root))
        return failure(std::cerr, "arguments", "A mode is required and --project requires --root");
    std::filesystem::path native_root;
    if (root)
        native_root = std::filesystem::path{
            std::u8string{reinterpret_cast<const char8_t*>(root->data()), root->size()}};
    auto created = AuthoringProtocol::create(native_root);
    if (!created)
        return service_failure(created.error());
    auto session = std::move(created).value();
    if (project) {
        const Json request = {{"jsonrpc", "2.0"},
                              {"protocol_version", authoring_protocol_version},
                              {"id", nullptr},
                              {"method", "project.open"},
                              {"params", {{"path", *project}}}};
        auto opened = session->handle(request.dump());
        if (!opened)
            return service_failure(opened.error());
        if (opened.value().failed) {
            static_cast<void>(output(std::cerr, opened.value().json));
            return 2;
        }
    }
    const auto dispatch = [&session](std::string_view request) {
        auto reply = session->handle(request);
        if (!reply)
            return service_failure(reply.error());
        if (!reply.value().json.empty() && !output(std::cout, reply.value().json))
            return 2;
        return reply.value().failed ? 1 : 0;
    };
    std::string request;
    request.reserve(authoring_protocol_maximum_request_bytes + 2);
    if (!*stream_mode) {
        char byte;
        while (std::cin.get(byte)) {
            if (request.size() == authoring_protocol_maximum_request_bytes)
                return failure(std::cout, "frame_too_large", "Request exceeds 65536 bytes");
            request.push_back(byte);
        }
        if (std::cin.bad())
            return failure(std::cerr, "input_failure", "Cannot read request input");
        return dispatch(request);
    }
    char byte;
    while (std::cin.get(byte)) {
        if (byte == '\n') {
            if (!request.empty() && request.back() == '\r')
                request.pop_back();
            const auto result = dispatch(request);
            if (result == 2)
                return result;
            request.clear();
        } else {
            request.push_back(byte);
            if (request.size() > authoring_protocol_maximum_request_bytes &&
                !(request.size() == authoring_protocol_maximum_request_bytes + 1 && byte == '\r'))
                return failure(std::cout, "frame_too_large", "Request exceeds 65536 bytes");
        }
    }
    if (std::cin.bad())
        return failure(std::cerr, "input_failure", "Cannot read protocol input");
    if (!request.empty())
        return failure(std::cout, "truncated_frame",
                       "Protocol EOF requires a complete LF-terminated frame");
    return 0;
}

int guarded_run(const std::vector<std::string>& arguments) noexcept {
    try {
#ifdef _WIN32
        if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1)
            return failure(std::cerr, "binary_stream", "Cannot initialize binary protocol streams");
#else
        std::signal(SIGPIPE, SIG_IGN);
#endif
        return run(arguments);
    } catch (...) {
        std::cerr
            << "{\"jsonrpc\":\"2.0\",\"protocol_version\":1,\"id\":null,\"error\":{\"code\":-32603,"
               "\"message\":\"Authoring host failed\",\"data\":{\"code\":\"execution_failed\","
               "\"context\":{\"operation\":\"authoring.transport\",\"request_id\":\"\","
               "\"path\":\"\",\"entity_id\":\"\",\"asset_id\":\"\"},\"protocol_version\":1}}}\n"
            << std::flush;
        return 2;
    }
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc > 7)
            return failure(std::cerr, "arguments", "Too many authoring arguments");
        std::vector<std::string> arguments;
        for (int index = 1; index < argc; ++index) {
            const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1,
                                                    nullptr, 0, nullptr, nullptr);
            if (length <= 0 || length > 4097)
                return failure(std::cerr, "arguments", "Argument is not bounded valid Unicode");
            std::string text(static_cast<std::size_t>(length), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1, text.data(),
                                    length, nullptr, nullptr) != length)
                return failure(std::cerr, "arguments", "Cannot convert Unicode argument");
            text.pop_back();
            arguments.push_back(std::move(text));
        }
        return guarded_run(arguments);
    } catch (...) {
        return 2;
    }
}
#else
int main(int argc, char** argv) {
    try {
        if (argc > 7)
            return failure(std::cerr, "arguments", "Too many authoring arguments");
        std::vector<std::string> arguments;
        for (int index = 1; index < argc; ++index) {
            if (std::char_traits<char>::length(argv[index]) > 4096)
                return failure(std::cerr, "arguments", "Argument exceeds 4096 bytes");
            arguments.emplace_back(argv[index]);
        }
        return guarded_run(arguments);
    } catch (...) {
        return 2;
    }
}
#endif
