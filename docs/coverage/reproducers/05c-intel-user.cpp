// Diagnostic only: compare hook removal before and after an owned target thread exits.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <windows.h>

LRESULT CALLBACK hook(int code, WPARAM wp, LPARAM lp) {
    return CallNextHookEx(nullptr, code, wp, lp);
}
struct State {
    HANDLE ready;
    HANDLE finish;
    DWORD thread{};
};
DWORD WINAPI worker(void* argument) {
    auto& state = *static_cast<State*>(argument);
    state.thread = GetCurrentThreadId();
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    if (!SetEvent(state.ready) || WaitForSingleObject(state.finish, INFINITE) != WAIT_OBJECT_0)
        std::terminate();
    return 0;
}
DWORD count(DWORD kind) {
    SetLastError(0);
    const auto value = GetGuiResources(GetCurrentProcess(), kind);
    if (!value && GetLastError())
        throw std::runtime_error("GUI resource measurement failed");
    return value;
}
void sample(const char* phase, unsigned cycle) {
    std::cout << "{\"phase\":\"" << phase << "\",\"cycle\":" << cycle
              << ",\"thread\":" << GetCurrentThreadId() << ",\"user\":" << count(GR_USEROBJECTS)
              << ",\"gdi\":" << count(GR_GDIOBJECTS) << "}\n"
              << std::flush;
}
int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
        std::cout << "Owned USER hook lifetime diagnostic: --run; qualification NOT_RUN\n";
        return 0;
    }
    if (argc != 2 || std::string_view(argv[1]) != "--run")
        return 2;
    try {
        sample("initial", 0);
        for (unsigned cycle = 1; cycle <= 20; ++cycle) {
            State state{CreateEventW(nullptr, TRUE, FALSE, nullptr),
                        CreateEventW(nullptr, TRUE, FALSE, nullptr)};
            if (!state.ready || !state.finish)
                throw std::runtime_error("CreateEvent failed");
            std::thread owned([&] { worker(&state); });
            if (WaitForSingleObject(state.ready, 2000) != WAIT_OBJECT_0)
                std::terminate();
            std::array<HHOOK, 3> hooks{};
            constexpr std::array ids{WH_CALLWNDPROC, WH_GETMESSAGE, WH_CALLWNDPROCRET};
            for (unsigned i = 0; i < hooks.size(); ++i) {
                hooks[i] = SetWindowsHookExA(ids[i], hook, nullptr, state.thread);
                if (!hooks[i])
                    std::terminate();
            }
            sample("owned", cycle);
            const bool exit_first = cycle % 2 == 0;
            if (exit_first) {
                if (!SetEvent(state.finish))
                    std::terminate();
                owned.join();
                sample("target_exited", cycle);
            }
            for (unsigned i = 0; i < hooks.size(); ++i) {
                SetLastError(0);
                const auto result = UnhookWindowsHookEx(hooks[i]);
                const auto error = GetLastError();
                std::cout << "{\"phase\":\"unhook\",\"cycle\":" << cycle
                          << ",\"hook_id\":" << ids[i] << ",\"target\":" << state.thread
                          << ",\"exit_first\":" << (exit_first ? "true" : "false")
                          << ",\"result\":" << result << ",\"error\":" << error << "}\n"
                          << std::flush;
                if (!exit_first && !result)
                    std::terminate();
            }
            if (!exit_first) {
                if (!SetEvent(state.finish))
                    std::terminate();
                owned.join();
            }
            if (!CloseHandle(state.finish) || !CloseHandle(state.ready))
                std::terminate();
            sample("closed", cycle);
        }
        const auto start = std::chrono::steady_clock::now();
        for (const unsigned ms : {0U, 16U, 50U, 100U, 250U, 500U, 1000U, 2000U, 5000U}) {
            while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds{ms}) {
                MSG message{};
                unsigned messages{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (++messages > 4096)
                        throw std::runtime_error("Message pump limit exceeded");
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                if (MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE) ==
                    WAIT_FAILED)
                    throw std::runtime_error("Message wait failed");
            }
            sample("settled", ms);
        }
        std::cout << "{\"complete\":true,\"qualification\":\"NOT_RUN\"}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
