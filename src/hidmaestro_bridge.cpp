#include "hidmaestro_bridge.hpp"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <iterator>

namespace mask::bridge {

namespace {

struct SrwExclusiveGuard {
    SRWLOCK& lock;
    explicit SrwExclusiveGuard(SRWLOCK& l) noexcept : lock(l) { AcquireSRWLockExclusive(&lock); }
    ~SrwExclusiveGuard() noexcept { ReleaseSRWLockExclusive(&lock); }
    SrwExclusiveGuard(const SrwExclusiveGuard&) = delete;
    SrwExclusiveGuard& operator=(const SrwExclusiveGuard&) = delete;
};

} // namespace

HIDMaestroHost& HIDMaestroHost::Instance() noexcept {
    static HIDMaestroHost instance{};
    return instance;
}

bool HIDMaestroHost::LoadAndInitialize(
    const wchar_t* mouseProfileId,
    const wchar_t* keyboardProfileId
) noexcept {
    // Copy inputs to local stack buffers before locking/overwriting to avoid self-aliasing
    const wchar_t* reqMouse = (mouseProfileId != nullptr && mouseProfileId[0] != L'\0')
        ? mouseProfileId : L"logitech-g502-hero";
    const wchar_t* reqKb = (keyboardProfileId != nullptr && keyboardProfileId[0] != L'\0')
        ? keyboardProfileId : L"keychron-q6";
    wchar_t mouseCopy[64]{};
    wchar_t kbCopy[64]{};
    wcsncpy_s(mouseCopy, reqMouse, _TRUNCATE);
    wcsncpy_s(kbCopy, reqKb, _TRUNCATE);

    SrwExclusiveGuard lock(lifecycleLock_);

    if (!hostLoaded_.load(std::memory_order_acquire)) {
        if (!LoadDotNetHostAndDelegates()) {
            return false;
        }
        hostLoaded_.store(true, std::memory_order_release);
    }

    const HMBridge_Initialize_Fn fnInitialize = fnInitialize_.load(std::memory_order_acquire);
    if (fnInitialize == nullptr) {
        return false;
    }

    wchar_t statusBuf[256]{};
    const int rc = fnInitialize(mouseCopy, kbCopy, statusBuf, 256);
    wcsncpy_s(lastStatusMessage_, statusBuf, _TRUNCATE);

    if (rc >= 0) {
        wcsncpy_s(activeMouseProfileId_, mouseCopy, _TRUNCATE);
        wcsncpy_s(activeKeyboardProfileId_, kbCopy, _TRUNCATE);
    }

    if (const HMBridge_GetActiveHardwareIds_Fn fnIds = fnGetActiveHardwareIds_.load(std::memory_order_acquire)) {
        uint16_t mVid = 0, mPid = 0, kVid = 0, kPid = 0;
        fnIds(&mVid, &mPid, &kVid, &kPid);
        activeMouseVid_.store(mVid, std::memory_order_release);
        activeMousePid_.store(mPid, std::memory_order_release);
        activeKeyboardVid_.store(kVid, std::memory_order_release);
        activeKeyboardPid_.store(kPid, std::memory_order_release);
    }

    controllersLive_.store(rc == 0, std::memory_order_release);
    return (rc >= 0);
}

void HIDMaestroHost::ReleaseAllNeutralHardware() noexcept {
    if (const HMBridge_ReleaseAllNeutral_Fn fn = fnReleaseAllNeutral_.load(std::memory_order_acquire)) {
        (void)fn();
    }
}

void HIDMaestroHost::EmergencyTeardownHardware(bool sweepEverything) noexcept {
    controllersLive_.store(false, std::memory_order_release);
    activeMouseVid_.store(0U, std::memory_order_release);
    activeMousePid_.store(0U, std::memory_order_release);
    activeKeyboardVid_.store(0U, std::memory_order_release);
    activeKeyboardPid_.store(0U, std::memory_order_release);
    if (const HMBridge_EmergencyTeardown_Fn fn = fnEmergencyTeardown_.load(std::memory_order_acquire)) {
        (void)fn(sweepEverything ? 1 : 0);
    }
}

void HIDMaestroHost::QueryDriverTelemetry(
    bool& outControllersLive,
    bool& outProcessElevated,
    bool& outDriverInstalled,
    int32_t& outMouseTransportIdx,
    int32_t& outKbTransportIdx,
    bool& outDeliversEveryReport,
    bool& outReleasesOnExit,
    uint8_t& outKeyboardLeds
) const noexcept {
    const HMBridge_QueryTelemetry_Fn fnQueryTelemetry = fnQueryTelemetry_.load(std::memory_order_acquire);
    if (fnQueryTelemetry == nullptr) {
        outControllersLive     = false;
        outProcessElevated     = false;
        outDriverInstalled     = false;
        outMouseTransportIdx   = -1;
        outKbTransportIdx      = -1;
        outDeliversEveryReport = false;
        outReleasesOnExit      = false;
        outKeyboardLeds        = 0;
        return;
    }

    int32_t init = 0, elev = 0, drv = 0, mIdx = -1, kIdx = -1, deliv = 0, relExit = 0, leds = 0;
    fnQueryTelemetry(&init, &elev, &drv, &mIdx, &kIdx, &deliv, &relExit, &leds);

    outControllersLive     = (init != 0);
    outProcessElevated     = (elev != 0);
    outDriverInstalled     = (drv != 0);
    outMouseTransportIdx   = mIdx;
    outKbTransportIdx      = kIdx;
    outDeliversEveryReport = (deliv != 0);
    outReleasesOnExit      = (relExit != 0);
    outKeyboardLeds        = static_cast<uint8_t>(leds & 0xFF);
}

void HIDMaestroHost::GetActiveVirtualHardwareIds(
    uint16_t& outMouseVid,
    uint16_t& outMousePid,
    uint16_t& outKbVid,
    uint16_t& outKbPid
) const noexcept {
    outMouseVid = activeMouseVid_.load(std::memory_order_acquire);
    outMousePid = activeMousePid_.load(std::memory_order_acquire);
    outKbVid    = activeKeyboardVid_.load(std::memory_order_acquire);
    outKbPid    = activeKeyboardPid_.load(std::memory_order_acquire);
}

uint64_t HIDMaestroHost::ParseVersionKey(const wchar_t* text) noexcept {
    uint64_t parts[3]{0, 0, 0};
    int index = 0;
    const wchar_t* p = text;
    while (*p != L'\0' && index < 3) {
        if (*p >= L'0' && *p <= L'9') {
            parts[index] = parts[index] * 10ULL + static_cast<uint64_t>(*p - L'0');
        } else if (*p == L'.') {
            ++index;
        } else {
            break; // "-preview", "-rc" ...
        }
        ++p;
    }
    const uint64_t isRelease = (std::wcschr(text, L'-') == nullptr) ? 1ULL : 0ULL;
    return ((std::min<uint64_t>)(parts[0], 0xFFFFULL) << 40) |
           ((std::min<uint64_t>)(parts[1], 0xFFFFULL) << 24) |
           ((std::min<uint64_t>)(parts[2], 0x7FFFFFULL) << 1) | isRelease;
}

bool HIDMaestroHost::FindNewestHostfxrUnder(const wchar_t* dotnetRoot, wchar_t* outPath, size_t maxChars) noexcept {
    if (dotnetRoot == nullptr || dotnetRoot[0] == L'\0') {
        return false;
    }
    wchar_t searchPattern[MAX_PATH]{};
    if (std::swprintf(searchPattern, MAX_PATH, L"%ls\\host\\fxr\\*", dotnetRoot) < 0) {
        return false;
    }

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(searchPattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return false;
    }

    wchar_t bestVersion[64]{};
    uint64_t bestKey = 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && fd.cFileName[0] != L'.') {
            const uint64_t key = ParseVersionKey(fd.cFileName);
            if (key > bestKey) {
                bestKey = key;
                wcsncpy_s(bestVersion, fd.cFileName, _TRUNCATE);
            }
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    if (bestVersion[0] == L'\0') {
        return false;
    }
    if (std::swprintf(outPath, maxChars, L"%ls\\host\\fxr\\%ls\\hostfxr.dll", dotnetRoot, bestVersion) < 0) {
        return false;
    }
    return (GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES);
}

bool HIDMaestroHost::LocateHostfxrDll(wchar_t* outPath, size_t maxChars) noexcept {
    // Honour DOTNET_ROOT, then the real Program Files location (not always on C:), then the usual default.
    wchar_t root[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(L"DOTNET_ROOT", root, MAX_PATH);
    if (len > 0 && len < MAX_PATH && FindNewestHostfxrUnder(root, outPath, maxChars)) {
        return true;
    }
    wchar_t programFiles[MAX_PATH]{};
    len = GetEnvironmentVariableW(L"ProgramW6432", programFiles, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        len = GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH);
    }
    if (len > 0 && len < MAX_PATH &&
        std::swprintf(root, MAX_PATH, L"%ls\\dotnet", programFiles) > 0 &&
        FindNewestHostfxrUnder(root, outPath, maxChars)) {
        return true;
    }
    return FindNewestHostfxrUnder(L"C:\\Program Files\\dotnet", outPath, maxChars);
}

bool HIDMaestroHost::LoadDotNetHostAndDelegates() noexcept {
    wchar_t hostfxrPath[MAX_PATH]{};
    if (!LocateHostfxrDll(hostfxrPath, MAX_PATH)) {
        wcscpy_s(lastStatusMessage_, L"The .NET 10 runtime (x64) wasn't found. Install it, then try again.");
        return false;
    }

    if (hHostfxr_ == nullptr) {
        hHostfxr_ = LoadLibraryW(hostfxrPath);
    }
    if (hHostfxr_ == nullptr) {
        wcscpy_s(lastStatusMessage_, L"Error: LoadLibraryW failed for hostfxr.dll.");
        return false;
    }

    const auto initForConfig = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
        GetProcAddress(hHostfxr_, "hostfxr_initialize_for_runtime_config")
    );
    const auto getDelegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
        GetProcAddress(hHostfxr_, "hostfxr_get_runtime_delegate")
    );
    const auto closeHost = reinterpret_cast<hostfxr_close_fn>(
        GetProcAddress(hHostfxr_, "hostfxr_close")
    );

    if (!initForConfig || !getDelegate || !closeHost) {
        wcscpy_s(lastStatusMessage_, L"Error: Missing exports in hostfxr.dll.");
        return false;
    }

    // Resolve directory containing the running executable
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = std::wcsrchr(exePath, L'\\');
    if (lastSlash != nullptr) {
        *lastSlash = L'\0';
    }

    wchar_t runtimeConfigPath[MAX_PATH]{};
    wchar_t bridgeDllPath[MAX_PATH]{};
    std::swprintf(runtimeConfigPath, MAX_PATH, L"%ls\\Mask.HIDMaestroBridge.runtimeconfig.json", exePath);
    std::swprintf(bridgeDllPath, MAX_PATH, L"%ls\\Mask.HIDMaestroBridge.dll", exePath);

    if (GetFileAttributesW(bridgeDllPath) == INVALID_FILE_ATTRIBUTES) {
        wcscpy_s(lastStatusMessage_, L"Error: Mask.HIDMaestroBridge.dll not found beside executable.");
        return false;
    }

    hostfxr_handle ctx = nullptr;
    const int32_t rcInit = initForConfig(runtimeConfigPath, nullptr, &ctx);
    if (rcInit < 0 || ctx == nullptr) {
        if (ctx != nullptr) {
            closeHost(ctx);
        }
        std::swprintf(lastStatusMessage_, std::size(lastStatusMessage_),
                      L"The .NET runtime could not start (0x%08X). Check that .NET 10 (x64) is installed.",
                      static_cast<unsigned>(rcInit));
        return false;
    }

    load_assembly_and_get_function_pointer_fn loadAssemblyFn = nullptr;
    const int32_t rcDel = getDelegate(
        ctx,
        HostfxrDelegateType::LoadAssemblyAndGetFunctionPointer,
        reinterpret_cast<void**>(&loadAssemblyFn)
    );
    closeHost(ctx);

    if (rcDel != 0 || loadAssemblyFn == nullptr) {
        wcscpy_s(lastStatusMessage_, L"Error: Failed to acquire LoadAssemblyAndGetFunctionPointer delegate.");
        return false;
    }

    const wchar_t* typeName = L"Mask.Bridge.NativeExports, Mask.HIDMaestroBridge";

    auto resolveMethod = [&](const wchar_t* methodName, void** outPtr) -> bool {
        const int rc = loadAssemblyFn(
            bridgeDllPath,
            typeName,
            methodName,
            kUnmanagedCallersOnlyMethod,
            nullptr,
            outPtr
        );
        return (rc == 0 && *outPtr != nullptr);
    };

    // Resolve everything into locals first and publish only a complete set, so another thread can never
    // observe a half-bound bridge.
    HMBridge_Initialize_Fn           fnInitialize = nullptr;
    HMBridge_SubmitMouse_Fn          fnSubmitMouse = nullptr;
    HMBridge_SubmitKeyboard_Fn       fnSubmitKeyboard = nullptr;
    HMBridge_SubmitBatch_Fn          fnSubmitBatch = nullptr;
    HMBridge_ReleaseAllNeutral_Fn    fnReleaseAllNeutral = nullptr;
    HMBridge_EmergencyTeardown_Fn    fnEmergencyTeardown = nullptr;
    HMBridge_QueryTelemetry_Fn       fnQueryTelemetry = nullptr;
    HMBridge_GetActiveHardwareIds_Fn fnGetActiveHardwareIds = nullptr;

    if (!resolveMethod(L"HMBridge_Initialize",           reinterpret_cast<void**>(&fnInitialize)) ||
        !resolveMethod(L"HMBridge_SubmitMouse",          reinterpret_cast<void**>(&fnSubmitMouse)) ||
        !resolveMethod(L"HMBridge_SubmitKeyboard",       reinterpret_cast<void**>(&fnSubmitKeyboard)) ||
        !resolveMethod(L"HMBridge_SubmitBatch",          reinterpret_cast<void**>(&fnSubmitBatch)) ||
        !resolveMethod(L"HMBridge_ReleaseAllNeutral",    reinterpret_cast<void**>(&fnReleaseAllNeutral)) ||
        !resolveMethod(L"HMBridge_EmergencyTeardown",    reinterpret_cast<void**>(&fnEmergencyTeardown)) ||
        !resolveMethod(L"HMBridge_QueryTelemetry",       reinterpret_cast<void**>(&fnQueryTelemetry)) ||
        !resolveMethod(L"HMBridge_GetActiveHardwareIds", reinterpret_cast<void**>(&fnGetActiveHardwareIds))) {
        wcscpy_s(lastStatusMessage_, L"Error: Failed to bind UnmanagedCallersOnly entry points in Mask.HIDMaestroBridge.dll.");
        return false;
    }

    fnSubmitMouse_.store(fnSubmitMouse, std::memory_order_release);
    fnSubmitKeyboard_.store(fnSubmitKeyboard, std::memory_order_release);
    fnSubmitBatch_.store(fnSubmitBatch, std::memory_order_release);
    fnReleaseAllNeutral_.store(fnReleaseAllNeutral, std::memory_order_release);
    fnEmergencyTeardown_.store(fnEmergencyTeardown, std::memory_order_release);
    fnQueryTelemetry_.store(fnQueryTelemetry, std::memory_order_release);
    fnGetActiveHardwareIds_.store(fnGetActiveHardwareIds, std::memory_order_release);
    fnInitialize_.store(fnInitialize, std::memory_order_release);

    return true;
}

} // namespace mask::bridge
