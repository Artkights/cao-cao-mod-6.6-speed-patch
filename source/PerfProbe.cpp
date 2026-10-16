#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <vector>
#include <algorithm>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    DWORD pid = strtoul(argv[1], nullptr, 10);
    HWND window = reinterpret_cast<HWND>(strtoul(argv[2], nullptr, 16));
    DWORD observed = 0;
    DWORD tid = GetWindowThreadProcessId(window, &observed);
    if (observed != pid) return 3;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!process || !thread) return 4;
    auto cpu = [&]() {
        FILETIME create, exit, kernel, user;
        GetProcessTimes(process, &create, &exit, &kernel, &user);
        ULARGE_INTEGER k, u;
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
        return k.QuadPart + u.QuadPart;
    };
    std::map<DWORD, int> ips, frames;
    ULONGLONG beforeCpu = cpu(), beforeTime = GetTickCount64();
    for (int sample = 0; sample < 500; ++sample) {
        if (SuspendThread(thread) == static_cast<DWORD>(-1)) break;
        CONTEXT context = {};
        context.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(thread, &context)) {
            ++ips[context.Eip];
            DWORD frame = context.Ebp;
            for (int depth = 0; depth < 20 && frame; ++depth) {
                DWORD data[2] = {};
                SIZE_T read = 0;
                if (!ReadProcessMemory(process, reinterpret_cast<void*>(frame), data, sizeof(data), &read) || read != sizeof(data)) break;
                ++frames[data[1]];
                if (data[0] <= frame || data[0] - frame > 1024 * 1024) break;
                frame = data[0];
            }
        }
        ResumeThread(thread);
        Sleep(5);
    }
    double elapsed = static_cast<double>(GetTickCount64() - beforeTime);
    printf("pid=%lu tid=%lu elapsed=%.0f ms cpu_one_core=%.2f%%\n", pid, tid, elapsed,
        (cpu() - beforeCpu) / (elapsed * 100.0));
    auto show = [](const char* name, const std::map<DWORD, int>& counts) {
        std::vector<std::pair<DWORD, int>> sorted(counts.begin(), counts.end());
        std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return a.second > b.second; });
        printf("%s\n", name);
        for (size_t i = 0; i < sorted.size() && i < 70; ++i) printf("%08lX %d\n", sorted[i].first, sorted[i].second);
    };
    show("EIP", ips); show("FRAME RETURNS", frames);
    CloseHandle(thread); CloseHandle(process);
    return 0;
}
