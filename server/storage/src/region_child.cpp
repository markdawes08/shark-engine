#include "shark/server/region_child.hpp"

#include "shark/stable_id.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <thread>
#include <type_traits>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace shark::server {
namespace {
using Clock = std::chrono::steady_clock;
struct Failure {
    Error error;
};
[[noreturn]] void fail(ErrorCode code, const char* message) {
    throw Failure{{code, message, {.operation = "server.region-child"}}};
}
template <class T> T take(Result<T> value) {
    if (!value)
        throw Failure{std::move(value).error()};
    return std::move(value).value();
}
template <class T, class F> Result<T> boundary(F&& function) {
    try {
        if constexpr (std::is_void_v<T>) {
            function();
            return {};
        } else
            return function();
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Region child operation failed; confirm exit before replacing it",
                     {.operation = "server.region-child"}};
    }
}
void timeout_valid(std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0 || timeout > std::chrono::seconds{60})
        fail(ErrorCode::invalid_argument,
             "Region child deadline must be within one millisecond and sixty seconds");
}
int remaining(Clock::time_point deadline) {
    const auto duration =
        std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (duration <= 0)
        fail(ErrorCode::execution_failed, "Region child RPC deadline expired; stream is uncertain");
    return static_cast<int>(std::min<std::int64_t>(duration, 60000));
}
#ifdef _WIN32
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE && value != nullptr)
            CloseHandle(value);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE release() {
        const auto handle = value;
        value = INVALID_HANDLE_VALUE;
        return handle;
    }
};
std::wstring wide(std::string_view text) {
    if (text.size() > 32768 || text.find('\0') != std::string_view::npos)
        fail(ErrorCode::invalid_argument, "Region child startup argument is invalid");
    if (text.empty())
        return {};
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0)
        fail(ErrorCode::invalid_argument, "Region child startup argument is not UTF-8");
    std::wstring value(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), value.data(), count) != count)
        fail(ErrorCode::invalid_argument, "Cannot encode region child startup argument");
    return value;
}
std::wstring quote_argument(std::wstring_view argument) {
    std::wstring result{L"\""};
    std::size_t slashes{};
    for (const auto character : argument) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}
struct Pipes {
    Handle input;
    Handle output;
    Handle child_input;
    Handle child_output;
};
void pipe_pair(Handle& parent, Handle& child, bool parent_reads) {
    const auto identity = take(generate_stable_id()).to_string();
    const auto name = L"\\\\.\\pipe\\shark-region-" + std::to_wstring(GetCurrentProcessId()) +
                      L"-" + wide(identity);
    parent.value = CreateNamedPipeW(name.c_str(),
                                    (parent_reads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) |
                                        FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                        PIPE_REJECT_REMOTE_CLIENTS,
                                    1, 65536, 65536, 0, nullptr);
    if (parent.value == INVALID_HANDLE_VALUE)
        fail(ErrorCode::io_failure, "Cannot create private region stdio pipe");
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.value)
        fail(ErrorCode::io_failure, "Cannot allocate region pipe event");
    OVERLAPPED connection{};
    connection.hEvent = event.value;
    const auto connected = ConnectNamedPipe(parent.value, &connection);
    const auto connection_error = connected ? ERROR_SUCCESS : GetLastError();
    if (!connected && connection_error != ERROR_IO_PENDING &&
        connection_error != ERROR_PIPE_CONNECTED)
        fail(ErrorCode::io_failure, "Cannot connect private region stdio pipe");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    child.value = CreateFileW(name.c_str(), parent_reads ? GENERIC_WRITE : GENERIC_READ, 0,
                              &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (child.value == INVALID_HANDLE_VALUE) {
        CancelIoEx(parent.value, &connection);
        DWORD ignored{};
        GetOverlappedResult(parent.value, &connection, &ignored, TRUE);
        fail(ErrorCode::io_failure, "Cannot open inherited region stdio pipe");
    }
    if (connection_error == ERROR_IO_PENDING) {
        if (WaitForSingleObject(event.value, 5000) != WAIT_OBJECT_0) {
            CancelIoEx(parent.value, &connection);
            DWORD ignored{};
            GetOverlappedResult(parent.value, &connection, &ignored, TRUE);
            fail(ErrorCode::io_failure, "Private region pipe connection failed");
        }
        DWORD ignored{};
        if (!GetOverlappedResult(parent.value, &connection, &ignored, FALSE))
            fail(ErrorCode::io_failure, "Private region pipe connection failed");
    }
}
DWORD transfer(HANDLE pipe, std::byte* data, DWORD count, bool reading,
               Clock::time_point deadline) {
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.value)
        fail(ErrorCode::io_failure, "Cannot allocate region RPC event");
    OVERLAPPED operation{};
    operation.hEvent = event.value;
    DWORD transferred{};
    const auto started = reading ? ReadFile(pipe, data, count, &transferred, &operation)
                                 : WriteFile(pipe, data, count, &transferred, &operation);
    if (!started) {
        if (GetLastError() != ERROR_IO_PENDING)
            fail(ErrorCode::io_failure, "Region child pipe closed or failed");
        DWORD wait{};
        try {
            wait = WaitForSingleObject(event.value, static_cast<DWORD>(remaining(deadline)));
        } catch (...) {
            CancelIoEx(pipe, &operation);
            GetOverlappedResult(pipe, &operation, &transferred, TRUE);
            throw;
        }
        if (wait != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &operation);
            GetOverlappedResult(pipe, &operation, &transferred, TRUE);
            fail(ErrorCode::execution_failed,
                 "Region child RPC deadline expired; stream is uncertain");
        }
        if (!GetOverlappedResult(pipe, &operation, &transferred, FALSE))
            fail(ErrorCode::io_failure, "Region child pipe completion failed");
    }
    if (transferred == 0)
        fail(ErrorCode::io_failure, "Region child pipe reached EOF");
    return transferred;
}
#else
struct Descriptor {
    int value{-1};
    explicit Descriptor(int descriptor = -1) : value(descriptor) {}
    ~Descriptor() {
        if (value >= 0)
            ::close(value);
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int release() {
        const auto descriptor = value;
        value = -1;
        return descriptor;
    }
};
void make_pipe(Descriptor& input, Descriptor& output) {
    int descriptors[2];
    if (::pipe2(descriptors, O_CLOEXEC) != 0)
        fail(ErrorCode::io_failure, "Cannot create region stdio pipe");
    input.value = descriptors[0];
    output.value = descriptors[1];
    for (auto* descriptor : {&input, &output})
        if (descriptor->value < 3) {
            const auto replacement = ::fcntl(descriptor->value, F_DUPFD_CLOEXEC, 3);
            if (replacement < 0)
                fail(ErrorCode::io_failure, "Cannot isolate region stdio descriptor");
            ::close(descriptor->value);
            descriptor->value = replacement;
        }
}
struct SigpipeBlock {
    sigset_t selected{}, previous{};
    bool pending{false};
    SigpipeBlock() {
        sigemptyset(&selected);
        sigaddset(&selected, SIGPIPE);
        if (pthread_sigmask(SIG_BLOCK, &selected, &previous) != 0)
            fail(ErrorCode::io_failure, "Cannot bound closed region pipe delivery");
        sigset_t prior{};
        sigpending(&prior);
        pending = sigismember(&prior, SIGPIPE) == 1;
    }
    ~SigpipeBlock() {
        if (!pending) {
            timespec zero{};
            while (sigtimedwait(&selected, nullptr, &zero) < 0 && errno == EINTR) {
            }
        }
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    }
};
void pipe_ready(int descriptor, bool reading, Clock::time_point deadline) {
    for (;;) {
        pollfd selected{descriptor, static_cast<short>(reading ? POLLIN : POLLOUT), 0};
        const auto result = ::poll(&selected, 1, remaining(deadline));
        if (result < 0 && errno == EINTR)
            continue;
        if (result == 0)
            fail(ErrorCode::execution_failed,
                 "Region child RPC deadline expired; stream is uncertain");
        if (result < 0 || (selected.revents & (POLLERR | POLLNVAL)) != 0)
            fail(ErrorCode::io_failure, "Region child pipe failed");
        if ((selected.revents & selected.events) != 0)
            return;
        if ((selected.revents & POLLHUP) != 0)
            fail(ErrorCode::io_failure, "Region child pipe reached EOF");
    }
}
#endif
} // namespace

struct RegionChild::Impl {
    std::thread::id thread{std::this_thread::get_id()};
    bool faulted{false};
    mutable std::optional<int> exit_code{};
#ifdef _WIN32
    Handle process;
    Handle job;
    Handle input;
    Handle output;
    std::uint64_t pid{};
    ~Impl() {
        if (process.value != INVALID_HANDLE_VALUE) {
            if (job.value != INVALID_HANDLE_VALUE)
                TerminateJobObject(job.value, 1);
            WaitForSingleObject(process.value, 5000);
        }
    }
    bool alive() const {
        if (exit_code)
            return false;
        const auto result = WaitForSingleObject(process.value, 0);
        if (result == WAIT_TIMEOUT)
            return true;
        if (result != WAIT_OBJECT_0)
            fail(ErrorCode::io_failure, "Cannot inspect region child exit");
        DWORD native_status{};
        if (!GetExitCodeProcess(process.value, &native_status))
            fail(ErrorCode::io_failure, "Cannot inspect region child exit status");
        exit_code = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(native_status));
        return false;
    }
#else
    Descriptor input;
    Descriptor output;
    pid_t pid{-1};
    ~Impl() {
        if (pid > 0 && !exit_code) {
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
    }
    bool alive() const {
        if (exit_code)
            return false;
        pid_t result;
        int native_status{};
        do {
            result = ::waitpid(pid, &native_status, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == pid) {
            if (WIFEXITED(native_status))
                exit_code = WEXITSTATUS(native_status);
            else if (WIFSIGNALED(native_status))
                exit_code = 128 + WTERMSIG(native_status);
            else
                fail(ErrorCode::io_failure, "Region child ended with an unknown native status");
            return false;
        }
        if (result < 0)
            fail(ErrorCode::io_failure, "Cannot inspect region child exit");
        return true;
    }
#endif
    void owner() const {
        if (thread != std::this_thread::get_id())
            fail(ErrorCode::conflict, "Region child requires its creating thread");
    }
    void io(std::span<std::byte> bytes, bool reading, Clock::time_point deadline) {
        std::size_t offset{};
        while (offset < bytes.size()) {
#ifdef _WIN32
            static_cast<void>(remaining(deadline));
            offset +=
                transfer(reading ? output.value : input.value, bytes.data() + offset,
                         static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 65536)),
                         reading, deadline);
#else
            const auto descriptor = reading ? output.value : input.value;
            pipe_ready(descriptor, reading, deadline);
            const auto result =
                reading ? ::read(descriptor, bytes.data() + offset, bytes.size() - offset)
                        : ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
            if (result < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            if (result <= 0)
                fail(ErrorCode::io_failure, "Region child pipe closed or failed");
            offset += static_cast<std::size_t>(result);
#endif
        }
    }
};
RegionChild::RegionChild(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
RegionChild::~RegionChild() = default;
Result<std::unique_ptr<RegionChild>> RegionChild::launch(const std::filesystem::path& executable,
                                                         std::span<const std::string> arguments,
                                                         bool inherit_diagnostics) {
    return boundary<std::unique_ptr<RegionChild>>([&] {
        std::error_code error;
        if (!executable.is_absolute() || !std::filesystem::is_regular_file(executable, error) ||
            error || arguments.size() > 32)
            fail(ErrorCode::invalid_argument,
                 "Region child needs an absolute Server executable and bounded startup arguments");
        std::size_t total{};
        for (const auto& argument : arguments) {
            if (argument.size() > 4096 || argument.find('\0') != std::string::npos ||
                argument == "--region-worker" || argument.size() > 32768 - total)
                fail(ErrorCode::invalid_argument,
                     "Region child startup argument exceeds its bounds");
            total += argument.size();
        }
        auto impl = std::make_unique<Impl>();
#ifdef _WIN32
        Pipes pipes;
        pipe_pair(pipes.input, pipes.child_input, false);
        pipe_pair(pipes.output, pipes.child_output, true);
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        Handle diagnostics;
        if (inherit_diagnostics) {
            const auto parent_error = GetStdHandle(STD_ERROR_HANDLE);
            if (parent_error == nullptr || parent_error == INVALID_HANDLE_VALUE ||
                !DuplicateHandle(GetCurrentProcess(), parent_error, GetCurrentProcess(),
                                 &diagnostics.value, 0, TRUE, DUPLICATE_SAME_ACCESS))
                fail(ErrorCode::io_failure, "Cannot retain caller's region diagnostic stream");
        } else
            diagnostics.value =
                CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (diagnostics.value == INVALID_HANDLE_VALUE)
            fail(ErrorCode::io_failure, "Cannot isolate region child diagnostics");
        impl->job.value = CreateJobObjectW(nullptr, nullptr);
        if (!impl->job.value)
            fail(ErrorCode::io_failure, "Cannot create region child job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(impl->job.value, JobObjectExtendedLimitInformation, &limits,
                                     sizeof(limits)))
            fail(ErrorCode::io_failure, "Cannot configure region child lifetime");
        SIZE_T attribute_bytes{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
        std::vector<std::byte> attributes(attribute_bytes);
        const auto attribute_list =
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_bytes))
            fail(ErrorCode::io_failure, "Cannot restrict region child handle inheritance");
        struct AttributeCleanup {
            LPPROC_THREAD_ATTRIBUTE_LIST list;
            ~AttributeCleanup() {
                DeleteProcThreadAttributeList(list);
            }
        } cleanup{attribute_list};
        std::array inherited{pipes.child_input.value, pipes.child_output.value, diagnostics.value};
        if (!UpdateProcThreadAttribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       inherited.data(), sizeof(inherited), nullptr, nullptr))
            fail(ErrorCode::io_failure, "Cannot restrict region child handle inheritance");
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = pipes.child_input.value;
        startup.StartupInfo.hStdOutput = pipes.child_output.value;
        startup.StartupInfo.hStdError = diagnostics.value;
        startup.lpAttributeList = attribute_list;
        std::wstring command = quote_argument(executable.native()) + L" --region-worker";
        for (const auto& argument : arguments) {
            command.push_back(L' ');
            command += quote_argument(wide(argument));
        }
        if (command.size() > 32767)
            fail(ErrorCode::invalid_argument, "Region child command line exceeds Windows bounds");
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                            nullptr, nullptr, &startup.StartupInfo, &process))
            fail(ErrorCode::io_failure, "Cannot launch owned region Server child");
        impl->process.value = process.hProcess;
        impl->pid = process.dwProcessId;
        Handle primary{process.hThread};
        if (!AssignProcessToJobObject(impl->job.value, impl->process.value)) {
            TerminateProcess(impl->process.value, 1);
            WaitForSingleObject(impl->process.value, 5000);
            fail(ErrorCode::io_failure, "Cannot attach region child to its owned job");
        }
        impl->input.value = pipes.input.release();
        impl->output.value = pipes.output.release();
        if (ResumeThread(primary.value) == static_cast<DWORD>(-1))
            fail(ErrorCode::io_failure, "Cannot start region child thread");
#else
        Descriptor child_input, child_output;
        make_pipe(child_input, impl->input);
        make_pipe(impl->output, child_output);
        Descriptor diagnostics{inherit_diagnostics ? ::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3)
                                                   : ::open("/dev/null", O_WRONLY | O_CLOEXEC)};
        if (diagnostics.value < 0)
            fail(ErrorCode::io_failure, "Cannot isolate region child diagnostics");
        if (diagnostics.value < 3) {
            const auto moved = ::fcntl(diagnostics.value, F_DUPFD_CLOEXEC, 3);
            if (moved < 0)
                fail(ErrorCode::io_failure, "Cannot isolate region child diagnostic descriptor");
            ::close(diagnostics.value);
            diagnostics.value = moved;
        }
        std::string program = executable.native();
        std::vector<std::string> values{program, "--region-worker"};
        values.insert(values.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        for (auto& value : values)
            argv.push_back(value.data());
        argv.push_back(nullptr);
        rlimit descriptors{};
        if (::getrlimit(RLIMIT_NOFILE, &descriptors) != 0 ||
            descriptors.rlim_cur == RLIM_INFINITY || descriptors.rlim_cur > 1024 * 1024)
            fail(ErrorCode::capability_unavailable,
                 "Cannot bound inherited region child descriptors");
        const auto parent = ::getpid();
        const auto child = ::fork();
        if (child < 0)
            fail(ErrorCode::io_failure, "Cannot launch owned region Server child");
        if (child == 0) {
            if (::setpgid(0, 0) != 0 || ::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 ||
                ::getppid() != parent || ::dup2(child_input.value, STDIN_FILENO) < 0 ||
                ::dup2(child_output.value, STDOUT_FILENO) < 0 ||
                ::dup2(diagnostics.value, STDERR_FILENO) < 0)
                ::_exit(126);
            if (::syscall(SYS_close_range, 3U, std::numeric_limits<unsigned>::max(), 0) != 0)
                for (rlim_t descriptor = 3; descriptor < descriptors.rlim_cur; ++descriptor)
                    ::close(static_cast<int>(descriptor));
            ::execv(program.c_str(), argv.data());
            ::_exit(127);
        }
        impl->pid = child;
        ::setpgid(child, child);
        for (const auto descriptor : {impl->input.value, impl->output.value}) {
            const auto flags = ::fcntl(descriptor, F_GETFL);
            if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0)
                fail(ErrorCode::io_failure, "Cannot bound region child pipe I/O");
        }
#endif
        return std::unique_ptr<RegionChild>{new RegionChild{std::move(impl)}};
    });
}
Result<bool> RegionChild::alive() const {
    return boundary<bool>([&] {
        impl_->owner();
        return impl_->alive();
    });
}
Result<std::optional<int>> RegionChild::exit_status() const {
    return boundary<std::optional<int>>([&] {
        impl_->owner();
        static_cast<void>(impl_->alive());
        return impl_->exit_code;
    });
}
std::uint64_t RegionChild::process_id() const noexcept {
    return static_cast<std::uint64_t>(impl_->pid);
}
Result<void> RegionChild::terminate(std::chrono::milliseconds timeout) {
    return boundary<void>([&] {
        impl_->owner();
        timeout_valid(timeout);
        impl_->faulted = true;
        if (!impl_->alive())
            return;
#ifdef _WIN32
        if (!TerminateJobObject(impl_->job.value, 1) ||
            WaitForSingleObject(impl_->process.value, static_cast<DWORD>(timeout.count())) !=
                WAIT_OBJECT_0)
            fail(ErrorCode::io_failure, "Region child termination was not confirmed");
        static_cast<void>(impl_->alive());
#else
        if (::kill(-impl_->pid, SIGKILL) != 0 && errno != ESRCH)
            fail(ErrorCode::io_failure, "Cannot stop region child process group");
        const auto deadline = Clock::now() + timeout;
        while (impl_->alive()) {
            if (Clock::now() >= deadline)
                fail(ErrorCode::execution_failed, "Region child termination was not confirmed");
            ::poll(nullptr, 0, 1);
        }
#endif
    });
}
Result<std::vector<std::byte>> RegionChild::request(std::span<const std::byte> bytes,
                                                    std::chrono::milliseconds timeout) {
    return boundary<std::vector<std::byte>>([&] {
        impl_->owner();
        timeout_valid(timeout);
        if (bytes.empty() || bytes.size() > region_child_maximum_frame_bytes)
            fail(ErrorCode::invalid_argument, "Region child frame exceeds its bounds");
        if (impl_->faulted || !impl_->alive())
            fail(ErrorCode::conflict, "Region child stream is closed or uncertain");
        const auto deadline = Clock::now() + timeout;
        std::array<std::byte, 4> header{};
        auto size = static_cast<std::uint32_t>(bytes.size());
        for (std::size_t index = 0; index < header.size(); ++index)
            header[index] = static_cast<std::byte>((size >> ((3 - index) * 8U)) & 255U);
#ifndef _WIN32
        SigpipeBlock blocked;
#endif
        try {
            impl_->io(header, false, deadline);
            // Native writes do not modify their input; the private I/O adapter
            // uses one mutable span type for both operating-system operations.
            impl_->io({const_cast<std::byte*>(bytes.data()), bytes.size()}, false, deadline);
            impl_->io(header, true, deadline);
            size = 0;
            for (const auto byte : header)
                size = (size << 8U) | std::to_integer<unsigned>(byte);
            if (size == 0 || size > region_child_maximum_frame_bytes)
                fail(ErrorCode::invalid_configuration,
                     "Region child response length exceeds its bound");
            std::vector<std::byte> response(size);
            impl_->io(response, true, deadline);
            return response;
        } catch (...) {
            impl_->faulted = true;
            throw;
        }
    });
}
} // namespace shark::server
