#pragma once

// Included only by an instrumented copy in ignored output. Never an engine or
// normal test dependency. Late markers use direct Win32 writes, not CRT streams.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdlib>

static bool diagnostic_cpu_stall = false;

__declspec(no_sanitize_address) inline void diagnostic_marker(const char* stage) noexcept {
    constexpr char prefix[] = "{\"kind\":\"exit_marker\",\"stage\":\"";
    constexpr char suffix[] = "\"}\n";
    DWORD length = 0;
    while (stage[length] && length < 128) {
        ++length;
    }
    DWORD written = 0;
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    WriteFile(output, prefix, sizeof(prefix) - 1, &written, nullptr);
    WriteFile(output, stage, length, &written, nullptr);
    WriteFile(output, suffix, sizeof(suffix) - 1, &written, nullptr);
}

struct DiagnosticProcessExit {
    ~DiagnosticProcessExit() {
        diagnostic_marker("static_marker_destroyed");
    }
};
inline DiagnosticProcessExit diagnostic_process_exit;

struct DiagnosticScopeExit {
    ~DiagnosticScopeExit() {
        diagnostic_marker("native_function_locals_destroyed");
    }
};

inline void diagnostic_register_exit() {
    std::atexit([] {
        diagnostic_marker("atexit_enter");
        if (diagnostic_cpu_stall) {
            diagnostic_marker("cpu_exit_stall");
            // Deliberate CPU-only observer selfcheck, killed at its external
            // five-second deadline. Native workloads never set this flag.
            Sleep(60'000);
        }
        diagnostic_marker("atexit_leave");
    });
}

// PE TLS callback marks entry into EXE process-detach; it is not proof that
// every DLL/runtime callback has completed. No allocation or library teardown.
__declspec(no_sanitize_address) inline void NTAPI diagnostic_tls(void*, DWORD reason,
                                                                 void*) noexcept {
    if (reason == DLL_PROCESS_DETACH) {
        diagnostic_marker("exe_tls_process_detach");
    }
}
#pragma section(".CRT$XLB", read)
extern "C" {
__declspec(allocate(".CRT$XLB")) PIMAGE_TLS_CALLBACK diagnostic_tls_entry = diagnostic_tls;
}
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:diagnostic_tls_entry")
