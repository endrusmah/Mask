#include "app.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <memory>

namespace {

void AttachParentConsole() noexcept {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) {
        const DWORD type = GetFileType(out);
        if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) {
            return;
        }
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    mask::app::LaunchOptions opts{};
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) opts.selfTest = true;
        else if (std::strcmp(argv[i], "--bench") == 0) opts.bench = true;
        else if (std::strcmp(argv[i], "--relaunch") == 0) opts.relaunched = true;
        else if (std::strcmp(argv[i], "--minimized") == 0) opts.startMinimized = true;
    }

    if (opts.selfTest || opts.bench) {
        AttachParentConsole();
    }

    HANDLE instanceMutex = nullptr;
    if (!opts.selfTest && !opts.bench) {
        instanceMutex = CreateMutexW(nullptr, FALSE, mask::app::kMutexName);
        const DWORD wait = (instanceMutex != nullptr)
            ? WaitForSingleObject(instanceMutex, opts.relaunched ? 10'000U : 0U)
            : WAIT_FAILED;
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            if (const HWND existing = FindWindowW(mask::app::kWindowClass, nullptr)) {
                PostMessageW(existing, mask::app::WM_APP_RESTORE_WINDOW, 0, 0);
            }
            if (instanceMutex != nullptr) {
                CloseHandle(instanceMutex);
            }
            return 0;
        }
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    auto app = std::make_unique<mask::app::App>();
    const int exitCode = app ? app->Run(opts) : 1;

    if (instanceMutex != nullptr) {
        ReleaseMutex(instanceMutex);
        CloseHandle(instanceMutex);
    }
    return exitCode;
}
