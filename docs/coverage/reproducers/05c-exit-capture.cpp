// Read-only observer for one owned diagnostic process. The caller supplies its
// recorded creation FILETIME as well as its PID, rejecting PID reuse before
// accessing memory. Run in a separate process with an external finite deadline.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <wct.h>

int wmain(int argc, wchar_t** argv) {
    if (argc != 4)
        return 2;
    wchar_t* end = nullptr;
    const auto parsed_pid = std::wcstoull(argv[1], &end, 10);
    if (!parsed_pid || parsed_pid > MAXDWORD || *end)
        return 2;
    const auto pid = static_cast<DWORD>(parsed_pid);
    const auto expected_creation = std::wcstoull(argv[2], &end, 10);
    if (!expected_creation || *end)
        return 2;
    HANDLE process =
        OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid);
    if (!process) {
        std::printf("{\"stage\":\"open_process\",\"pid\":%lu,\"error\":%lu}\n", pid,
                    GetLastError());
        return 1;
    }
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        CloseHandle(process);
        return 3;
    }
    const auto actual_creation =
        (static_cast<unsigned long long>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime;
    if (actual_creation != expected_creation) {
        std::printf("{\"stage\":\"identity_rejected\",\"pid\":%lu,\"expected_creation\":%llu,"
                    "\"actual_creation\":%llu}\n",
                    pid, expected_creation, actual_creation);
        CloseHandle(process);
        return 4;
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process, &exit_code)) {
        CloseHandle(process);
        return 3;
    }
    std::printf("{\"stage\":\"process_status\",\"pid\":%lu,\"creation_time\":%llu,\"identity_"
                "match\":true,\"exit_code\":%lu}\n",
                pid, actual_creation, exit_code);
    std::fflush(stdout);
    if (exit_code != STILL_ACTIVE) {
        CloseHandle(process);
        return 0;
    }
    HANDLE file =
        CreateFileW(argv[3], GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        CloseHandle(process);
        return 3;
    }
    const auto type =
        static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo |
                                   MiniDumpWithUnloadedModules | MiniDumpWithFullMemoryInfo);
    const bool dumped =
        MiniDumpWriteDump(process, pid, file, type, nullptr, nullptr, nullptr) != FALSE;
    const DWORD dump_error = dumped ? 0 : GetLastError();
    CloseHandle(file);
    std::printf("{\"stage\":\"minidump\",\"pid\":%lu,\"written\":%s,\"error\":%lu}\n", pid,
                dumped ? "true" : "false", dump_error);
    std::fflush(stdout);
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    const HWCT session = OpenThreadWaitChainSession(0, nullptr);
    if (snapshot == INVALID_HANDLE_VALUE || !session) {
        std::printf("{\"stage\":\"wait_chain_setup\",\"error\":%lu}\n", GetLastError());
    } else {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        unsigned count = 0;
        if (Thread32First(snapshot, &entry))
            do {
                if (entry.th32OwnerProcessID != pid)
                    continue;
                if (++count > 128) {
                    std::puts("{\"stage\":\"thread_limit\",\"truncated\":true}");
                    break;
                }
                WAITCHAIN_NODE_INFO nodes[WCT_MAX_NODE_COUNT]{};
                DWORD nodes_count = WCT_MAX_NODE_COUNT;
                BOOL cycle = FALSE;
                const bool found =
                    GetThreadWaitChain(session, 0, WCT_OUT_OF_PROC_FLAG | WCT_OUT_OF_PROC_CS_FLAG,
                                       entry.th32ThreadID, &nodes_count, nodes, &cycle) != FALSE;
                const DWORD error = found ? 0 : GetLastError();
                std::printf("{\"stage\":\"wait_chain\",\"pid\":%lu,\"tid\":%lu,\"ok\":%s,\"cycle\":"
                            "%s,\"error\":%lu,\"nodes\":[",
                            pid, entry.th32ThreadID, found ? "true" : "false",
                            cycle ? "true" : "false", error);
                const DWORD limit =
                    nodes_count < WCT_MAX_NODE_COUNT ? nodes_count : WCT_MAX_NODE_COUNT;
                for (DWORD i = 0; i < limit && (found || error == ERROR_MORE_DATA); ++i) {
                    if (i)
                        std::putchar(',');
                    std::printf("{\"type\":%d,\"status\":%d", static_cast<int>(nodes[i].ObjectType),
                                static_cast<int>(nodes[i].ObjectStatus));
                    if (nodes[i].ObjectType == WctThreadType)
                        std::printf(
                            ",\"pid\":%lu,\"tid\":%lu,\"wait_ms\":%lu,\"context_switches\":%lu",
                            nodes[i].ThreadObject.ProcessId, nodes[i].ThreadObject.ThreadId,
                            nodes[i].ThreadObject.WaitTime, nodes[i].ThreadObject.ContextSwitches);
                    std::putchar('}');
                }
                std::puts("]}");
                std::fflush(stdout);
            } while (Thread32Next(snapshot, &entry));
    }
    if (session)
        CloseThreadWaitChainSession(session);
    if (snapshot != INVALID_HANDLE_VALUE)
        CloseHandle(snapshot);
    CloseHandle(process);
    return dumped ? 0 : 1;
}
