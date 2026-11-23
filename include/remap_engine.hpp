#pragma once

// Multi-Keyboard, Multi-Mouse, Multi-Monitor Remapping Engine + Layer / Unlock State
// + Unified Hook-Driven RemapHookPipeline + Immediate Raw Input Enumeration & Correlation.
// Zero Win32 hooks are installed when all remap rules are disabled or automation is paused.

#include "core_contracts.hpp"
#include "input_pipeline.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>

#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <memory>
#include <new>

namespace mask::remap {

inline constexpr uint32_t kRemapRulesMagic   = 0x4D524D50U; // 'MRMP'
inline constexpr uint32_t kRemapRulesVersion = 1U;
inline constexpr size_t   kMaxMonitors       = 8U;

struct MonitorRectInfo {
    int32_t  left{0};
    int32_t  top{0};
    int32_t  right{1920};
    int32_t  bottom{1080};
    uint32_t dpi{96};
    bool     isPrimary{true};
    wchar_t  name[32]{L"Monitor 1"};
};

namespace detail {

inline constexpr uint64_t kMouseButtonVkMaskWord0 =
    (1ULL << VK_LBUTTON) |
    (1ULL << VK_RBUTTON) |
    (1ULL << VK_MBUTTON) |
    (1ULL << VK_XBUTTON1) |
    (1ULL << VK_XBUTTON2);

[[nodiscard]] inline bool SanitizeBool(const bool& value) noexcept {
    uint8_t raw = 0;
    std::memcpy(&raw, &value, 1);
    return raw != 0U;
}

[[nodiscard]] constexpr bool IsMouseButtonVk(uint16_t vk) noexcept {
    const uint16_t b = static_cast<uint16_t>(vk & 0xFFU);
    return b == VK_LBUTTON || b == VK_RBUTTON || b == VK_MBUTTON ||
           b == VK_XBUTTON1 || b == VK_XBUTTON2;
}

[[nodiscard]] inline bool IsProtectedSafetyVk(uint16_t vk, uint16_t pauseVk, uint16_t stopAllVk) noexcept {
    const uint16_t b = static_cast<uint16_t>(vk & 0xFFU);
    if (b == 0U || b == VK_ESCAPE || b == VK_LCONTROL || b == VK_RCONTROL || b == VK_CONTROL) {
        return true;
    }
    if (pauseVk != 0U && b == (pauseVk & 0xFFU)) {
        return true;
    }
    if (stopAllVk != 0U && b == (stopAllVk & 0xFFU)) {
        return true;
    }
    return false;
}

[[nodiscard]] constexpr uint16_t NormalizeHookVk(uint16_t vk, DWORD scanCode, DWORD flags) noexcept {
    const uint16_t b = static_cast<uint16_t>(vk & 0xFFU);
    if (b == VK_SHIFT) {
        return (scanCode == 0x36U) ? VK_RSHIFT : VK_LSHIFT;
    }
    if (b == VK_CONTROL) {
        return ((flags & LLKHF_EXTENDED) != 0U) ? VK_RCONTROL : VK_LCONTROL;
    }
    if (b == VK_MENU) {
        return ((flags & LLKHF_EXTENDED) != 0U) ? VK_RMENU : VK_LMENU;
    }
    return b;
}

[[nodiscard]] constexpr bool VkMatchesRuleSource(uint16_t ruleSourceVk, uint16_t hookVk) noexcept {
    const uint16_t r = static_cast<uint16_t>(ruleSourceVk & 0xFFU);
    const uint16_t h = static_cast<uint16_t>(hookVk & 0xFFU);
    if (r == 0U || h == 0U) {
        return false;
    }
    if (r == h) {
        return true;
    }
    if (r == VK_SHIFT   && (h == VK_LSHIFT   || h == VK_RSHIFT))   return true;
    if (r == VK_CONTROL && (h == VK_LCONTROL || h == VK_RCONTROL)) return true;
    if (r == VK_MENU    && (h == VK_LMENU    || h == VK_RMENU))    return true;
    return false;
}

[[nodiscard]] inline uint64_t Estimate50MsTscCycles() noexcept {
    DWORD mhz = 0;
    DWORD sz = sizeof(mhz);
    if (RegGetValueW(
            HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            L"~MHz",
            RRF_RT_REG_DWORD,
            nullptr,
            &mhz,
            &sz) == ERROR_SUCCESS && mhz >= 500U && mhz <= 10'000U) {
        return static_cast<uint64_t>(mhz) * 50'000ULL;
    }
    return 150'000'000ULL; // Safe 3.0 GHz fallback for 50 ms
}

[[nodiscard]] inline bool ConsumeVirtualEchoToken(
    mask::core::EngineSharedState& state,
    uint16_t vk,
    bool isDown,
    uint64_t nowTsc,
    uint64_t windowTsc
) noexcept {
    const uint16_t normVk = mask::input::InputDispatcher::NormalizeEchoVk(vk);
    if (normVk == 0U || normVk >= 256U) {
        return false;
    }

    auto& countAtom = isDown ? state.virtualEchoDownCount[normVk] : state.virtualEchoUpCount[normVk];
    auto& tscAtom   = isDown ? state.virtualEchoDownTsc[normVk]   : state.virtualEchoUpTsc[normVk];

    constexpr uint64_t kMaxCrossCoreSkewTsc = 100'000ULL;

    uint32_t cnt = countAtom.load(std::memory_order_acquire);
    while (cnt > 0U) {
        const uint64_t stampTsc = tscAtom.load(std::memory_order_acquire);
        const bool fresh = (nowTsc >= stampTsc)
            ? ((nowTsc - stampTsc) <= windowTsc)
            : ((stampTsc - nowTsc) <= kMaxCrossCoreSkewTsc);

        if (!fresh) {
            if (tscAtom.load(std::memory_order_acquire) == stampTsc) {
                countAtom.compare_exchange_strong(cnt, 0U, std::memory_order_acq_rel, std::memory_order_acquire);
            } else {
                cnt = countAtom.load(std::memory_order_acquire);
                continue;
            }
            return false;
        }

        if (countAtom.compare_exchange_weak(cnt, cnt - 1U, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline wchar_t AsciiToUpperW(wchar_t c) noexcept {
    return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c;
}

[[nodiscard]] inline bool ContainsIgnoreCase(const wchar_t* haystack, const wchar_t* needle) noexcept {
    if (haystack == nullptr || needle == nullptr || needle[0] == L'\0') {
        return false;
    }
    const size_t nLen = std::wcslen(needle);
    for (const wchar_t* p = haystack; *p != L'\0'; ++p) {
        size_t i = 0;
        while (i < nLen && p[i] != L'\0' && AsciiToUpperW(p[i]) == AsciiToUpperW(needle[i])) {
            ++i;
        }
        if (i == nLen) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool StartsWithIgnoreCase(const wchar_t* str, const wchar_t* prefix) noexcept {
    if (str == nullptr || prefix == nullptr) {
        return false;
    }
    for (size_t i = 0; prefix[i] != L'\0'; ++i) {
        if (str[i] == L'\0' || AsciiToUpperW(str[i]) != AsciiToUpperW(prefix[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool ParseHex4(const wchar_t* p, uint16_t& out) noexcept {
    if (p == nullptr) {
        return false;
    }
    uint16_t val = 0U;
    for (int i = 0; i < 4; ++i) {
        const wchar_t c = p[i];
        val <<= 4U;
        if (c >= L'0' && c <= L'9') {
            val = static_cast<uint16_t>(val | static_cast<uint16_t>(c - L'0'));
        } else if (c >= L'A' && c <= L'F') {
            val = static_cast<uint16_t>(val | static_cast<uint16_t>(c - L'A' + 10));
        } else if (c >= L'a' && c <= L'f') {
            val = static_cast<uint16_t>(val | static_cast<uint16_t>(c - L'a' + 10));
        } else {
            return false;
        }
    }
    out = val;
    return true;
}

inline void ParseVidPidFromDevicePath(const wchar_t* path, uint16_t& outVid, uint16_t& outPid) noexcept {
    outVid = 0U;
    outPid = 0U;
    if (path == nullptr) {
        return;
    }
    for (const wchar_t* p = path; *p != L'\0'; ++p) {
        if (outVid == 0U && StartsWithIgnoreCase(p, L"VID_")) {
            uint16_t v = 0U;
            if (ParseHex4(p + 4, v)) {
                outVid = v;
            }
        } else if (outPid == 0U && StartsWithIgnoreCase(p, L"PID_")) {
            uint16_t pid = 0U;
            if (ParseHex4(p + 4, pid)) {
                outPid = pid;
            }
        }
        if (outVid != 0U && outPid != 0U) {
            break;
        }
    }
}

[[nodiscard]] inline bool IsVirtualHidMaestroDevice(
    uint16_t vid,
    uint16_t pid,
    const wchar_t* devPath
) noexcept {
    if (ContainsIgnoreCase(devPath, L"HIDMAESTRO")) {
        return true;
    }
    if ((vid == mask::input::kHidMaestroMouseVid && pid == mask::input::kHidMaestroMousePid) ||
        (vid == mask::input::kHidMaestroKeyboardVid &&
         (pid == mask::input::kHidMaestroKeyboardPid || pid == mask::input::kHidMaestroKeyboardPidAlt))) {
        return true;
    }

    uint16_t activeMouseVid = 0, activeMousePid = 0, activeKbVid = 0, activeKbPid = 0;
    mask::bridge::HIDMaestroHost::Instance().GetActiveVirtualHardwareIds(
        activeMouseVid, activeMousePid, activeKbVid, activeKbPid);
    if ((activeMouseVid != 0U && vid == activeMouseVid && pid == activeMousePid) ||
        (activeKbVid != 0U && vid == activeKbVid && pid == activeKbPid)) {
        return true;
    }

    if (vid != 0U || pid != 0U) {
        const uint32_t packed = (static_cast<uint32_t>(vid) << 16) | static_cast<uint32_t>(pid);
        for (const auto& entry : mask::input::GameInputGlobalContext::registeredVirtualVidPids) {
            const uint32_t reg = entry.load(std::memory_order_relaxed);
            if (reg == 0U) {
                break;
            }
            if (reg == packed) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] inline uint64_t HashUpperWideString64(const wchar_t* s, uint64_t seed) noexcept {
    uint64_t h = 0xCBF29CE484222325ULL ^ seed;
    if (s != nullptr) {
        for (; *s != L'\0'; ++s) {
            const uint16_t ch = static_cast<uint16_t>(AsciiToUpperW(*s));
            h ^= static_cast<uint64_t>(ch & 0xFFU);
            h *= 0x100000001B3ULL;
            h ^= static_cast<uint64_t>(ch >> 8);
            h *= 0x100000001B3ULL;
        }
    }
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBULL;
    h ^= h >> 31;
    return h;
}

inline void BuildPersistentRawDeviceId(
    const wchar_t* identityStr,
    uint8_t devKind,
    uint16_t vid,
    uint16_t pid,
    uint8_t outBytes[32]
) noexcept {
    const uint64_t h0 = HashUpperWideString64(identityStr, 0x9E3779B97F4A7C15ULL ^ devKind);
    const uint64_t h1 = HashUpperWideString64(identityStr, 0xD1B54A32D192ED03ULL ^ (static_cast<uint64_t>(devKind) << 8));
    const uint64_t h2 = HashUpperWideString64(identityStr, (static_cast<uint64_t>(vid) << 16) | pid);
    std::memcpy(outBytes + 0, &h0, 8U);
    std::memcpy(outBytes + 8, &h1, 8U);
    outBytes[16] = static_cast<uint8_t>(vid & 0xFFU);
    outBytes[17] = static_cast<uint8_t>(vid >> 8);
    outBytes[18] = static_cast<uint8_t>(pid & 0xFFU);
    outBytes[19] = static_cast<uint8_t>(pid >> 8);
    outBytes[20] = devKind;
    outBytes[21] = static_cast<uint8_t>('M');
    outBytes[22] = static_cast<uint8_t>('R');
    outBytes[23] = static_cast<uint8_t>('W');
    std::memcpy(outBytes + 24, &h2, 8U);
}

struct CfgMgrApi {
    using DEVINST_T = DWORD;
    using CONFIGRET_T = DWORD;
    struct DevPropKey {
        GUID  fmtid;
        ULONG pid;
    };
    using CM_Locate_DevNodeW_Fn = CONFIGRET_T(WINAPI*)(DEVINST_T*, const wchar_t*, ULONG);
    using CM_Get_Parent_Fn = CONFIGRET_T(WINAPI*)(DEVINST_T*, DEVINST_T, ULONG);
    using CM_Get_Device_IDW_Fn = CONFIGRET_T(WINAPI*)(DEVINST_T, wchar_t*, ULONG, ULONG);
    using CM_Get_DevNode_PropertyW_Fn = CONFIGRET_T(WINAPI*)(DEVINST_T, const DevPropKey*, ULONG*, PBYTE, PULONG, ULONG);

    CM_Locate_DevNodeW_Fn       locateDevNode{nullptr};
    CM_Get_Parent_Fn            getParent{nullptr};
    CM_Get_Device_IDW_Fn        getDeviceId{nullptr};
    CM_Get_DevNode_PropertyW_Fn getDevNodeProp{nullptr};
    bool                        initialized{false};
};

[[nodiscard]] inline const CfgMgrApi& GetCfgMgrApi() noexcept {
    static const CfgMgrApi api = []() noexcept -> CfgMgrApi {
        CfgMgrApi a{};
        HMODULE mod = LoadLibraryExW(L"cfgmgr32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (mod != nullptr) {
            a.locateDevNode  = reinterpret_cast<CfgMgrApi::CM_Locate_DevNodeW_Fn>(GetProcAddress(mod, "CM_Locate_DevNodeW"));
            a.getParent      = reinterpret_cast<CfgMgrApi::CM_Get_Parent_Fn>(GetProcAddress(mod, "CM_Get_Parent"));
            a.getDeviceId    = reinterpret_cast<CfgMgrApi::CM_Get_Device_IDW_Fn>(GetProcAddress(mod, "CM_Get_Device_IDW"));
            a.getDevNodeProp = reinterpret_cast<CfgMgrApi::CM_Get_DevNode_PropertyW_Fn>(GetProcAddress(mod, "CM_Get_DevNode_PropertyW"));
            a.initialized    = (a.locateDevNode != nullptr && a.getDevNodeProp != nullptr);
        }
        return a;
    }();
    return api;
}

inline void ResolveRawDeviceIdentityAndName(
    const wchar_t* devPath,
    uint8_t devKind,
    uint16_t vid,
    uint16_t pid,
    wchar_t* outIdentityStr,
    size_t outIdentityCap,
    wchar_t* outDisplayName,
    size_t outDisplayCap
) noexcept {
    wcsncpy_s(outIdentityStr, outIdentityCap, devPath != nullptr ? devPath : L"", _TRUNCATE);
    outDisplayName[0] = L'\0';

    // Convert "\\?\HID#VID_...#instance#{guid}" -> "HID\VID_...\instance"
    wchar_t devInst[384]{};
    if (devPath != nullptr) {
        const wchar_t* p = devPath;
        if (std::wcsncmp(p, L"\\\\?\\", 4) == 0) {
            p += 4;
        }
        size_t pos = 0U;
        int hashes = 0;
        for (; *p != L'\0' && pos + 1U < std::size(devInst); ++p) {
            if (*p == L'#') {
                ++hashes;
                if (hashes == 3) {
                    break;
                }
                devInst[pos++] = L'\\';
            } else {
                devInst[pos++] = *p;
            }
        }
        devInst[pos] = L'\0';
    }

    // Query DEVPKEY_Device_BusReportedDeviceDesc ({540b947e-8b40-45bc-a8a2-6a0b894cbda2}, 4)
    constexpr CfgMgrApi::DevPropKey kBusDescKey{
        { 0x540b947eU, 0x8b40U, 0x45bcU, { 0xa8U, 0xa2U, 0x6aU, 0x0bU, 0x89U, 0x4cU, 0xbdU, 0xa2U } },
        4U
    };
    const auto& cm = GetCfgMgrApi();
    if (cm.initialized && devInst[0] != L'\0') {
        CfgMgrApi::DEVINST_T dn = 0;
        if (cm.locateDevNode(&dn, devInst, 0U) == 0U /* CR_SUCCESS */) {
            wchar_t busDesc[128]{};
            ULONG propType = 0;
            ULONG sz = sizeof(busDesc);
            if (cm.getDevNodeProp(dn, &kBusDescKey, &propType, reinterpret_cast<PBYTE>(busDesc), &sz, 0U) == 0U &&
                busDesc[0] != L'\0') {
                wcsncpy_s(outDisplayName, outDisplayCap, busDesc, _TRUNCATE);
            }

            CfgMgrApi::DEVINST_T parentDn = 0;
            if (cm.getParent != nullptr && cm.getDeviceId != nullptr && cm.getParent(&parentDn, dn, 0U) == 0U) {
                wchar_t parentId[256]{};
                if (cm.getDeviceId(parentDn, parentId, static_cast<ULONG>(std::size(parentId)), 0U) == 0U) {
                    const bool usbOrHidParent =
                        StartsWithIgnoreCase(parentId, L"USB\\") ||
                        StartsWithIgnoreCase(parentId, L"HID\\") ||
                        StartsWithIgnoreCase(parentId, L"BTH");
                    if (usbOrHidParent) {
                        // Group multiple HID collections (Col01/Col02) on the same USB interface
                        wcsncpy_s(outIdentityStr, outIdentityCap, parentId, _TRUNCATE);
                        if (outDisplayName[0] == L'\0') {
                            wchar_t parDesc[128]{};
                            propType = 0;
                            sz = sizeof(parDesc);
                            if (cm.getDevNodeProp(parentDn, &kBusDescKey, &propType, reinterpret_cast<PBYTE>(parDesc), &sz, 0U) == 0U &&
                                parDesc[0] != L'\0') {
                                wcsncpy_s(outDisplayName, outDisplayCap, parDesc, _TRUNCATE);
                            }
                        }
                    }
                }
            }
        }
    }

    // Fallback to Registry FriendlyName / DeviceDesc under Enum\<devInst>
    if (outDisplayName[0] == L'\0' && devInst[0] != L'\0') {
        wchar_t regSub[440]{};
        if (std::swprintf(regSub, std::size(regSub), L"SYSTEM\\CurrentControlSet\\Enum\\%ls", devInst) > 0) {
            wchar_t buf[256]{};
            DWORD bytes = sizeof(buf);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, regSub, L"FriendlyName", RRF_RT_REG_SZ, nullptr, buf, &bytes) == ERROR_SUCCESS &&
                buf[0] != L'\0') {
                wcsncpy_s(outDisplayName, outDisplayCap, buf, _TRUNCATE);
            } else {
                bytes = sizeof(buf);
                if (RegGetValueW(HKEY_LOCAL_MACHINE, regSub, L"DeviceDesc", RRF_RT_REG_SZ, nullptr, buf, &bytes) == ERROR_SUCCESS &&
                    buf[0] != L'\0') {
                    const wchar_t* semi = std::wcsrchr(buf, L';');
                    const wchar_t* clean = (semi != nullptr && semi[1] != L'\0') ? (semi + 1) : buf;
                    wcsncpy_s(outDisplayName, outDisplayCap, clean, _TRUNCATE);
                }
            }
        }
    }

    // Trim trailing whitespace if any
    size_t len = std::wcslen(outDisplayName);
    while (len > 0U && outDisplayName[len - 1U] == L' ') {
        outDisplayName[--len] = L'\0';
    }

    if (outDisplayName[0] == L'\0') {
        const wchar_t* prefix = (devKind == 2U) ? L"HID Mouse" : L"HID Keyboard";
        if (vid != 0U || pid != 0U) {
            std::swprintf(outDisplayName, outDisplayCap, L"%ls (%04X:%04X)", prefix, vid, pid);
        } else {
            std::swprintf(outDisplayName, outDisplayCap, L"%ls", (devKind == 2U) ? L"Mouse" : L"Keyboard");
        }
    }
}

inline void SanitizeRemapRule(mask::core::RemapRuleConfig& r, size_t slotIdx) noexcept {
    r.slotIndex        = static_cast<uint8_t>(slotIdx);
    r.enabled          = SanitizeBool(r.enabled);
    r.inUse            = SanitizeBool(r.inUse);
    r.suppressOriginal = SanitizeBool(r.suppressOriginal);
    r.anyDevice        = SanitizeBool(r.anyDevice);
    if (r.deviceKind > 2U) {
        r.deviceKind = 0U;
    }
    if (r.layerIndex >= mask::core::kMaxRemapLayers) {
        r.layerIndex = 0U;
    }
    if (r.unlockPolicy > static_cast<uint8_t>(mask::core::RemapUnlockPolicy::WhenLocked)) {
        r.unlockPolicy = static_cast<uint8_t>(mask::core::RemapUnlockPolicy::Always);
    }
    if (r.actionKind > static_cast<uint8_t>(mask::core::RemapLayerAction::WarpToMonitor)) {
        r.actionKind = static_cast<uint8_t>(mask::core::RemapLayerAction::RemapKey);
    }
    if (r.targetLayer >= mask::core::kMaxRemapLayers) {
        r.targetLayer = 0U;
    }
    if (r.monitorIndex < -1 || r.monitorIndex >= static_cast<int8_t>(kMaxMonitors)) {
        r.monitorIndex = -1;
    }
    r.reserved = 0U;
    r.sourceVk = static_cast<uint16_t>(r.sourceVk & 0xFFU);
    r.targetVk = static_cast<uint16_t>(r.targetVk & 0xFFU);

    r.deviceLabel[std::size(r.deviceLabel) - 1] = L'\0';
    if (r.deviceLabel[0] == L'\0') {
        wcscpy_s(r.deviceLabel, L"Any device");
    }
    r.name[std::size(r.name) - 1] = L'\0';
    if (r.name[0] == L'\0') {
        std::swprintf(r.name, std::size(r.name), L"Remap %zu", slotIdx + 1U);
    }
}

struct RemapsFilePayload {
    uint32_t magic{kRemapRulesMagic};
    uint32_t version{kRemapRulesVersion};
    uint32_t count{static_cast<uint32_t>(mask::core::kMaxRemapRules)};
    uint32_t reserved{0};
    std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules> rules{};
};

static_assert(std::is_trivially_copyable_v<RemapsFilePayload>,
              "RemapsFilePayload must be trivially copyable.");

[[nodiscard]] inline bool WriteFileAtomic(const wchar_t* path, const void* data, size_t size) noexcept {
    if (path == nullptr || path[0] == L'\0' || data == nullptr || size == 0U) {
        return false;
    }

    wchar_t tmp[1040]{};
    wchar_t bak[1040]{};
    if (std::swprintf(tmp, std::size(tmp), L"%ls.tmp", path) < 0 ||
        std::swprintf(bak, std::size(bak), L"%ls.bak", path) < 0) {
        return false;
    }

    wchar_t dir[1040]{};
    if (wcscpy_s(dir, path) == 0) {
        if (wchar_t* slash = std::wcsrchr(dir, L'\\')) {
            *slash = L'\0';
            if (dir[0] != L'\0') {
                CreateDirectoryW(dir, nullptr);
            }
        }
    }

    const HANDLE h = CreateFileW(
        tmp,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    const bool ok = WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr) != FALSE &&
                    written == static_cast<DWORD>(size) &&
                    FlushFileBuffers(h) != FALSE;
    CloseHandle(h);

    if (!ok) {
        DeleteFileW(tmp);
        return false;
    }

    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES &&
        ReplaceFileW(path, tmp, bak, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != FALSE) {
        return true;
    }
    if (MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE) {
        return true;
    }

    DeleteFileW(tmp);
    return false;
}

[[nodiscard]] inline bool ReadRemapsFileRaw(const wchar_t* path, RemapsFilePayload& out) noexcept {
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }
    const HANDLE h = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER fileSize{};
    bool ok = GetFileSizeEx(h, &fileSize) != FALSE &&
              fileSize.QuadPart == static_cast<LONGLONG>(sizeof(RemapsFilePayload));

    if (ok) {
        DWORD bytesRead = 0;
        ok = ReadFile(h, &out, static_cast<DWORD>(sizeof(RemapsFilePayload)), &bytesRead, nullptr) != FALSE &&
             bytesRead == static_cast<DWORD>(sizeof(RemapsFilePayload)) &&
             out.magic == kRemapRulesMagic &&
             out.version == kRemapRulesVersion &&
             out.count == static_cast<uint32_t>(mask::core::kMaxRemapRules);
    }

    CloseHandle(h);
    return ok;
}

[[nodiscard]] inline uint32_t QueryMonitorDpi(HMONITOR hMon) noexcept {
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static GetDpiForMonitorFn fn = []() noexcept -> GetDpiForMonitorFn {
        HMODULE shcore = GetModuleHandleW(L"shcore.dll");
        if (shcore == nullptr) {
            shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
        if (shcore == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"));
    }();

    if (fn != nullptr) {
        UINT dpiX = 96;
        UINT dpiY = 96;
        if (SUCCEEDED(fn(hMon, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY)) && dpiX >= 72U && dpiX <= 480U) {
            return static_cast<uint32_t>(dpiX);
        }
    }

    const HDC hdc = GetDC(nullptr);
    uint32_t fallbackDpi = 96U;
    if (hdc != nullptr) {
        const int px = GetDeviceCaps(hdc, LOGPIXELSX);
        if (px >= 72 && px <= 480) {
            fallbackDpi = static_cast<uint32_t>(px);
        }
        ReleaseDC(nullptr, hdc);
    }
    return fallbackDpi;
}

} // namespace detail

/**
 * @brief Multi-Keyboard, Multi-Mouse, Multi-Monitor Remapping Engine with Layers, Unlock Policy,
 *        Monitor Warp, Immediate Raw Input Enumeration, and Unified `RemapHookPipeline`.
 */
class RemapEngine {
public:
    explicit RemapEngine(mask::core::EngineSharedState& sharedState) noexcept
        : sharedState_(sharedState)
        , echoWindowTsc_(detail::Estimate50MsTscCycles()) {
        InitializeSRWLock(&ruleLock_);
        InitializeSRWLock(&monitorLock_);
        InitializeSRWLock(&holdLock_);
        InitializeSRWLock(&rawLock_);
        InitializeSRWLock(&corrLock_);
        InitializeSRWLock(&hookLock_);

        for (size_t i = 0; i < mask::core::kMaxRemapRules; ++i) {
            rules_[i].slotIndex = static_cast<uint8_t>(i);
        }
        RefreshMonitors();
        RefreshRawInputDevices();
    }

    ~RemapEngine() noexcept {
        Stop();
    }

    RemapEngine(const RemapEngine&) = delete;
    RemapEngine& operator=(const RemapEngine&) = delete;

    void Start() {
        started_.store(true, std::memory_order_release);
        RefreshMonitors();
        RefreshRawInputDevices();
        RecomputeSuppressedMask();
    }

    void Stop() noexcept {
        started_.store(false, std::memory_order_release);
        ResetStateAndSuppression();
        ShutdownHooks();
    }

    void RefreshMonitors() noexcept {
        struct EnumCtx {
            std::array<MonitorRectInfo, kMaxMonitors> list{};
            int count{0};
        } ctx{};

        auto enumProc = [](HMONITOR hMon, HDC /*hdc*/, LPRECT /*rect*/, LPARAM lParam) -> BOOL {
            auto* c = reinterpret_cast<EnumCtx*>(lParam);
            if (c == nullptr || c->count >= static_cast<int>(kMaxMonitors)) {
                return FALSE;
            }
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hMon, &mi) == FALSE) {
                return TRUE;
            }
            auto& info = c->list[static_cast<size_t>(c->count)];
            info.left = mi.rcMonitor.left;
            info.top = mi.rcMonitor.top;
            info.right = mi.rcMonitor.right;
            info.bottom = mi.rcMonitor.bottom;
            info.dpi = detail::QueryMonitorDpi(hMon);
            info.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0U;
            ++c->count;
            return TRUE;
        };

        EnumDisplayMonitors(nullptr, nullptr, enumProc, reinterpret_cast<LPARAM>(&ctx));

        if (ctx.count == 0) {
            ctx.count = 1;
            ctx.list[0].left = 0;
            ctx.list[0].top = 0;
            ctx.list[0].right = GetSystemMetrics(SM_CXSCREEN);
            ctx.list[0].bottom = GetSystemMetrics(SM_CYSCREEN);
            if (ctx.list[0].right <= 0) {
                ctx.list[0].right = 1920;
            }
            if (ctx.list[0].bottom <= 0) {
                ctx.list[0].bottom = 1080;
            }
            ctx.list[0].dpi = 96U;
            ctx.list[0].isPrimary = true;
        } else {
            // Place primary display at index 0 for predictable "Monitor 1" indexing
            for (int i = 1; i < ctx.count; ++i) {
                if (ctx.list[static_cast<size_t>(i)].isPrimary) {
                    std::swap(ctx.list[0], ctx.list[static_cast<size_t>(i)]);
                    break;
                }
            }
        }

        for (int i = 0; i < ctx.count; ++i) {
            auto& m = ctx.list[static_cast<size_t>(i)];
            if (m.isPrimary) {
                std::swprintf(m.name, std::size(m.name), L"Monitor %d (Primary)", i + 1);
            } else {
                std::swprintf(m.name, std::size(m.name), L"Monitor %d", i + 1);
            }
        }

        AcquireSRWLockExclusive(&monitorLock_);
        monitors_ = ctx.list;
        monitorCount_ = ctx.count;
        ReleaseSRWLockExclusive(&monitorLock_);

        RefreshRawInputDevices();
    }

    [[nodiscard]] int GetMonitors(std::array<MonitorRectInfo, 8>& outMonitors) const noexcept {
        AcquireSRWLockShared(&monitorLock_);
        outMonitors = monitors_;
        const int count = monitorCount_;
        ReleaseSRWLockShared(&monitorLock_);
        return count;
    }

    /**
     * @brief Immediately enumerates all physical keyboards and mice via Win32 Raw Input (`GetRawInputDeviceList`),
     * filtering out HIDMaestro virtual VID/PIDs and RDP pseudo-devices, resolving friendly product names.
     */
    void RefreshRawInputDevices() noexcept {
        std::array<RAWINPUTDEVICELIST, 128> stackList{};
        UINT numDevices = static_cast<UINT>(stackList.size());
        UINT got = GetRawInputDeviceList(stackList.data(), &numDevices, sizeof(RAWINPUTDEVICELIST));

        std::unique_ptr<RAWINPUTDEVICELIST[]> heapList;
        const RAWINPUTDEVICELIST* entries = stackList.data();
        if (got == static_cast<UINT>(-1)) {
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && numDevices > 0U && numDevices <= 1024U) {
                heapList.reset(new (std::nothrow) RAWINPUTDEVICELIST[numDevices]{});
                if (heapList) {
                    got = GetRawInputDeviceList(heapList.get(), &numDevices, sizeof(RAWINPUTDEVICELIST));
                    entries = heapList.get();
                } else {
                    got = 0U;
                }
            } else {
                got = 0U;
            }
        }
        if (got == static_cast<UINT>(-1)) {
            got = 0U;
        }

        std::array<RawHandleEntry, kMaxRawHandles> newHandleMap{};
        size_t newHandleCount = 0U;

        std::array<mask::input::PhysicalDeviceInfo, kMaxRawInventory> newDevices{};
        int newDevCount = 0;

        auto addRawDeviceCandidate = [&](const RAWINPUTDEVICELIST& rawEntry, bool usbPreferredPass) noexcept {
            if (rawEntry.dwType != RIM_TYPEKEYBOARD && rawEntry.dwType != RIM_TYPEMOUSE) {
                return;
            }
            const uint8_t devKind = (rawEntry.dwType == RIM_TYPEKEYBOARD) ? 1U : 2U;

            wchar_t devPath[512]{};
            UINT pathChars = static_cast<UINT>(std::size(devPath));
            const UINT nameRes = GetRawInputDeviceInfoW(rawEntry.hDevice, RIDI_DEVICENAME, devPath, &pathChars);
            if (nameRes == static_cast<UINT>(-1) || devPath[0] == L'\0') {
                return;
            }
            devPath[std::size(devPath) - 1] = L'\0';

            if (detail::ContainsIgnoreCase(devPath, L"RDP_")) {
                return;
            }

            uint16_t vid = 0U;
            uint16_t pid = 0U;
            detail::ParseVidPidFromDevicePath(devPath, vid, pid);

            const bool isUsbVidPid = (vid != 0U || pid != 0U);
            if (isUsbVidPid != usbPreferredPass) {
                return;
            }

            const bool isVirtual = detail::IsVirtualHidMaestroDevice(vid, pid, devPath);

            wchar_t identityStr[384]{};
            wchar_t displayName[40]{};
            uint8_t devIdBytes[32]{};
            if (!isVirtual) {
                detail::ResolveRawDeviceIdentityAndName(
                    devPath,
                    devKind,
                    vid,
                    pid,
                    identityStr,
                    std::size(identityStr),
                    displayName,
                    std::size(displayName));
                detail::BuildPersistentRawDeviceId(identityStr, devKind, vid, pid, devIdBytes);
            }

            if (newHandleCount < newHandleMap.size()) {
                auto& h = newHandleMap[newHandleCount++];
                h.hDevice   = rawEntry.hDevice;
                h.vendorId  = vid;
                h.productId = pid;
                h.devKind   = devKind;
                h.isVirtual = isVirtual;
                std::memcpy(h.deviceIdBytes, devIdBytes, sizeof(devIdBytes));
            }

            if (isVirtual) {
                return;
            }

            // Collapse multiple HID collections on the same physical interface & device kind
            for (int j = 0; j < newDevCount; ++j) {
                const auto& existing = newDevices[static_cast<size_t>(j)];
                const bool sameKind = (devKind == 1U) ? existing.isKeyboard : existing.isMouse;
                if (sameKind && std::memcmp(existing.deviceIdBytes, devIdBytes, 32U) == 0) {
                    return;
                }
            }

            if (newDevCount < static_cast<int>(kMaxRawInventory)) {
                auto& dst = newDevices[static_cast<size_t>(newDevCount++)];
                dst = mask::input::PhysicalDeviceInfo{};
                std::memcpy(dst.deviceIdBytes, devIdBytes, sizeof(dst.deviceIdBytes));
                dst.vendorId   = vid;
                dst.productId  = pid;
                dst.isKeyboard = (devKind == 1U);
                dst.isMouse    = (devKind == 2U);
                wcsncpy_s(dst.displayName, displayName, _TRUNCATE);
            }
        };

        // Pass 1: USB/HID devices with real VID/PID; Pass 2: ACPI/touchpad fallback devices
        for (UINT i = 0; i < got; ++i) {
            addRawDeviceCandidate(entries[i], true);
        }
        for (UINT i = 0; i < got; ++i) {
            addRawDeviceCandidate(entries[i], false);
        }

        // Also include any active GameInput physical device not already represented by Raw Input
        std::array<mask::input::PhysicalDeviceInfo, kMaxRawInventory> giList{};
        const int giCount = mask::input::CopyPhysicalDeviceInventory(giList);
        for (int g = 0; g < giCount && newDevCount < static_cast<int>(kMaxRawInventory); ++g) {
            const auto& gi = giList[static_cast<size_t>(g)];
            bool alreadyPresent = false;
            for (int d = 0; d < newDevCount; ++d) {
                const auto& raw = newDevices[static_cast<size_t>(d)];
                if (std::memcmp(raw.deviceIdBytes, gi.deviceIdBytes, 32U) == 0) {
                    alreadyPresent = true;
                    break;
                }
                if (gi.vendorId != 0U && raw.vendorId == gi.vendorId && raw.productId == gi.productId &&
                    ((gi.isKeyboard && raw.isKeyboard) || (gi.isMouse && raw.isMouse))) {
                    alreadyPresent = true;
                    break;
                }
            }
            if (!alreadyPresent) {
                newDevices[static_cast<size_t>(newDevCount++)] = gi;
            }
        }

        AcquireSRWLockExclusive(&rawLock_);
        rawHandleMap_   = newHandleMap;
        rawHandleCount_ = newHandleCount;
        rawDevices_     = newDevices;
        rawDeviceCount_ = newDevCount;
        ReleaseSRWLockExclusive(&rawLock_);
    }

    /**
     * @brief Thread-safe snapshot of all physical keyboards & mice populated by Raw Input at startup.
     */
    [[nodiscard]] int GetRawDevices(
        std::array<mask::input::PhysicalDeviceInfo, mask::input::kMaxPhysicalGameInputDevices>& outList
    ) const noexcept {
        AcquireSRWLockShared(&rawLock_);
        outList = rawDevices_;
        const int count = rawDeviceCount_;
        ReleaseSRWLockShared(&rawLock_);
        return count;
    }

    void UpdateRule(size_t index, const mask::core::RemapRuleConfig& rule) noexcept {
        if (index >= mask::core::kMaxRemapRules) {
            return;
        }
        mask::core::RemapRuleConfig clean = rule;
        detail::SanitizeRemapRule(clean, index);

        AcquireSRWLockExclusive(&ruleLock_);
        rules_[index] = clean;
        ReleaseSRWLockExclusive(&ruleLock_);

        RecomputeSuppressedMask();
    }

    void UpdateAllRules(
        const std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>& rules
    ) noexcept {
        AcquireSRWLockExclusive(&ruleLock_);
        for (size_t i = 0; i < mask::core::kMaxRemapRules; ++i) {
            mask::core::RemapRuleConfig clean = rules[i];
            detail::SanitizeRemapRule(clean, i);
            rules_[i] = clean;
        }
        ReleaseSRWLockExclusive(&ruleLock_);

        RecomputeSuppressedMask();
    }

    // Called by GameInput per-device transition callback: records per-device correlation and
    // acts as a clean fallback ONLY when the LL hook for that device kind is not currently active.
    void OnPhysicalDeviceTransition(
        const uint8_t deviceId[32],
        uint8_t deviceKind,
        uint16_t vk,
        bool isDown,
        uint64_t nowTsc
    ) noexcept {
        if (!started_.load(std::memory_order_acquire)) {
            return;
        }
        const uint16_t boundedVk = static_cast<uint16_t>(vk & 0xFFU);
        if (boundedVk == 0U) {
            return;
        }

        if (deviceId != nullptr) {
            RecordGameInputCorrelation(deviceId, deviceKind, boundedVk, isDown, nowTsc);
        }

        // Prevent double-firing when the unified RemapHookPipeline LL hook handles this kind
        const bool hookHandlesThis = detail::IsMouseButtonVk(boundedVk)
            ? mouseHookActive_.load(std::memory_order_acquire)
            : kbHookActive_.load(std::memory_order_acquire);
        if (hookHandlesThis) {
            return;
        }

        if (!isDown) {
            (void)HandleKeyRelease(deviceId, boundedVk, nowTsc);
            return;
        }

        if (sharedState_.automationPaused.load(std::memory_order_acquire) ||
            sharedState_.suppressTriggers.load(std::memory_order_relaxed) ||
            sharedState_.captureBindTarget.load(std::memory_order_relaxed) != 0U) {
            return;
        }

        const uint16_t pauseVk   = sharedState_.globalPauseVk.load(std::memory_order_relaxed);
        const uint16_t stopAllVk = sharedState_.globalStopAllVk.load(std::memory_order_relaxed);
        if (detail::IsProtectedSafetyVk(boundedVk, pauseVk, stopAllVk)) {
            return;
        }

        ActiveHoldInfo existing{};
        if (FindExistingHold(deviceId, boundedVk, existing)) {
            return;
        }

        uint8_t activeLayer = sharedState_.activeRemapLayer.load(std::memory_order_acquire);
        if (activeLayer >= mask::core::kMaxRemapLayers) {
            activeLayer = 0U;
        }
        const bool unlocked = sharedState_.remapUnlocked.load(std::memory_order_acquire);

        CorrelatedDevice corr{};
        if (deviceId != nullptr) {
            corr.hasGiId = true;
            std::memcpy(corr.giIdBytes, deviceId, 32U);
            ResolveAliasVidPidFromGi(deviceId, corr.vendorId, corr.productId);
        }

        mask::core::RemapRuleConfig winner{};
        if (!FindMatchingRule(boundedVk, deviceKind, activeLayer, unlocked, corr, winner)) {
            return;
        }

        const bool shouldSuppress =
            winner.suppressOriginal ||
            (winner.actionKind == static_cast<uint8_t>(mask::core::RemapLayerAction::BlockKey));
        ExecuteRuleOnDown(winner, deviceId, boundedVk, activeLayer, unlocked, nowTsc, shouldSuppress);
    }

    void ResetStateAndSuppression() noexcept {
        std::array<uint16_t, kMaxActiveHolds> keysToRelease{};
        size_t releaseCount = 0U;
        bool hadShiftedLayer = false;

        AcquireSRWLockExclusive(&holdLock_);
        for (auto& h : activeHolds_) {
            if (!h.active) {
                continue;
            }
            if (h.actionKind == static_cast<uint8_t>(mask::core::RemapLayerAction::RemapKey) &&
                h.emittedTargetVk != 0U && releaseCount < keysToRelease.size()) {
                keysToRelease[releaseCount++] = h.emittedTargetVk;
            } else if (h.actionKind == static_cast<uint8_t>(mask::core::RemapLayerAction::ShiftLayerHold)) {
                hadShiftedLayer = true;
            }
            h = ActiveRemapHold{};
        }
        ReleaseSRWLockExclusive(&holdLock_);

        if (releaseCount > 0U && sharedState_.engineRunning.load(std::memory_order_acquire)) {
            unsigned int aux = 0;
            const uint64_t nowTsc = __rdtscp(&aux);
            bool pushed = false;
            for (size_t i = 0; i < releaseCount; ++i) {
                mask::core::AuxOutputCommand cmd{};
                cmd.timestampTsc = nowTsc;
                cmd.vk = keysToRelease[i];
                cmd.kind = static_cast<uint8_t>(mask::core::AuxActionKind::KeyUp);
                if (sharedState_.auxOutputQueue.try_push(cmd)) {
                    pushed = true;
                }
            }
            if (pushed) {
                mask::input::WakeEngine();
            }
        }

        if (hadShiftedLayer) {
            sharedState_.activeRemapLayer.store(0U, std::memory_order_release);
        }

        for (auto& word : sharedState_.suppressedVkBits) {
            word.store(0ULL, std::memory_order_release);
        }
        sharedState_.anySuppressionActive.store(false, std::memory_order_release);
        SyncHooks(false, false);
    }

    void RecomputeSuppressedMask() noexcept {
        const mask::core::TelemetrySnapshot snap = sharedState_.ReadTelemetry();
        if (snap.calibratedTscFreqHz >= 500'000'000ULL) {
            echoWindowTsc_.store(snap.calibratedTscFreqHz / 20ULL, std::memory_order_release); // exact 50 ms
        }

        if (!started_.load(std::memory_order_acquire) ||
            sharedState_.automationPaused.load(std::memory_order_acquire)) {
            for (auto& word : sharedState_.suppressedVkBits) {
                word.store(0ULL, std::memory_order_release);
            }
            sharedState_.anySuppressionActive.store(false, std::memory_order_release);
            SyncHooks(false, false);
            return;
        }

        uint8_t activeLayer = sharedState_.activeRemapLayer.load(std::memory_order_acquire);
        if (activeLayer >= mask::core::kMaxRemapLayers) {
            activeLayer = 0U;
        }
        const bool unlocked      = sharedState_.remapUnlocked.load(std::memory_order_acquire);
        const uint16_t pauseVk   = sharedState_.globalPauseVk.load(std::memory_order_relaxed);
        const uint16_t stopAllVk = sharedState_.globalStopAllVk.load(std::memory_order_relaxed);

        uint64_t suppBits[4]{0ULL, 0ULL, 0ULL, 0ULL};
        bool wantKeyboard = false;
        bool wantMouse    = false;

        AcquireSRWLockShared(&ruleLock_);
        for (size_t i = 0; i < mask::core::kMaxRemapRules; ++i) {
            const auto& r = rules_[i];
            if (!r.enabled || !r.inUse) {
                continue;
            }
            if (r.layerIndex != activeLayer && r.layerIndex != 0U) {
                continue;
            }
            if (!MatchesUnlockPolicy(r.unlockPolicy, unlocked)) {
                continue;
            }
            const uint16_t srcVk = static_cast<uint16_t>(r.sourceVk & 0xFFU);
            if (detail::IsProtectedSafetyVk(srcVk, pauseVk, stopAllVk)) {
                continue;
            }

            if (detail::IsMouseButtonVk(srcVk)) {
                wantMouse = true;
            } else {
                wantKeyboard = true;
            }

            const bool wantsSuppress =
                r.suppressOriginal ||
                (r.actionKind == static_cast<uint8_t>(mask::core::RemapLayerAction::BlockKey));
            if (wantsSuppress) {
                suppBits[srcVk >> 6] |= (1ULL << (srcVk & 63U));
                if (srcVk == VK_SHIFT) {
                    suppBits[VK_LSHIFT >> 6] |= (1ULL << (VK_LSHIFT & 63U));
                    suppBits[VK_RSHIFT >> 6] |= (1ULL << (VK_RSHIFT & 63U));
                } else if (srcVk == VK_MENU) {
                    suppBits[VK_LMENU >> 6] |= (1ULL << (VK_LMENU & 63U));
                    suppBits[VK_RMENU >> 6] |= (1ULL << (VK_RMENU & 63U));
                }
            }
        }
        ReleaseSRWLockShared(&ruleLock_);

        for (size_t w = 0; w < 4; ++w) {
            sharedState_.suppressedVkBits[w].store(suppBits[w], std::memory_order_release);
        }
        const bool anySupp = (suppBits[0] | suppBits[1] | suppBits[2] | suppBits[3]) != 0ULL;
        sharedState_.anySuppressionActive.store(anySupp, std::memory_order_release);

        SyncHooks(wantKeyboard, wantMouse);
    }

    [[nodiscard]] static bool SaveRemaps(
        const wchar_t* path,
        const std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>& rules
    ) noexcept {
        std::unique_ptr<detail::RemapsFilePayload> payload(new (std::nothrow) detail::RemapsFilePayload{});
        if (!payload) {
            return false;
        }
        payload->magic    = kRemapRulesMagic;
        payload->version  = kRemapRulesVersion;
        payload->count    = static_cast<uint32_t>(mask::core::kMaxRemapRules);
        payload->reserved = 0U;
        for (size_t i = 0; i < mask::core::kMaxRemapRules; ++i) {
            payload->rules[i] = rules[i];
            detail::SanitizeRemapRule(payload->rules[i], i);
        }
        return detail::WriteFileAtomic(path, payload.get(), sizeof(detail::RemapsFilePayload));
    }

    [[nodiscard]] static bool LoadRemaps(
        const wchar_t* path,
        std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>& outRules
    ) noexcept {
        if (path == nullptr || path[0] == L'\0') {
            return false;
        }
        std::unique_ptr<detail::RemapsFilePayload> payload(new (std::nothrow) detail::RemapsFilePayload{});
        if (!payload) {
            return false;
        }

        bool ok = detail::ReadRemapsFileRaw(path, *payload);
        if (!ok) {
            wchar_t bak[1040]{};
            if (std::swprintf(bak, std::size(bak), L"%ls.bak", path) >= 0) {
                ok = detail::ReadRemapsFileRaw(bak, *payload);
            }
        }
        if (!ok) {
            return false;
        }

        for (size_t i = 0; i < mask::core::kMaxRemapRules; ++i) {
            detail::SanitizeRemapRule(payload->rules[i], i);
            outRules[i] = payload->rules[i];
        }
        return true;
    }

    /**
     * @brief Deterministic offline verification of the hook-driven remap pipeline:
     * KeyDown/AutoRepeat/KeyUp, A <-> B bidirectional swap with virtual echo token consumption,
     * BlockKey, ShiftLayerHold, ToggleLayer, ToggleUnlockState, WarpToMonitor, Safety VKs,
     * generic modifier matching, and mouse button hook transitions.
     */
    [[nodiscard]] static bool RunRemapSelfTest() noexcept {
        std::unique_ptr<mask::core::EngineSharedState> state(new (std::nothrow) mask::core::EngineSharedState{});
        if (!state) {
            return false;
        }
        std::unique_ptr<RemapEngine> engine(new (std::nothrow) RemapEngine(*state));
        if (!engine) {
            return false;
        }

        const uint64_t freqHz = 3'000'000'000ULL;
        const uint64_t win50Ms = freqHz / 20ULL; // 150,000,000 cycles
        engine->echoWindowTsc_.store(win50Ms, std::memory_order_release);
        engine->started_.store(true, std::memory_order_release);

        // Configure deterministic 2-monitor layout
        {
            AcquireSRWLockExclusive(&engine->monitorLock_);
            engine->monitorCount_ = 2;
            engine->monitors_[0] = MonitorRectInfo{ 0, 0, 1920, 1080, 96, true, L"Monitor 1 (Primary)" };
            engine->monitors_[1] = MonitorRectInfo{ 1920, 0, 3840, 1080, 96, false, L"Monitor 2" };
            ReleaseSRWLockExclusive(&engine->monitorLock_);
        }

        auto drainAux = [&](std::array<mask::core::AuxOutputCommand, 16>& cmds) noexcept -> size_t {
            size_t count = 0U;
            mask::core::AuxOutputCommand cmd{};
            while (state->auxOutputQueue.try_pop(cmd)) {
                if (count < cmds.size()) {
                    cmds[count++] = cmd;
                }
            }
            return count;
        };

        auto makeRule = [](uint8_t slot, uint16_t src, uint16_t dst, bool suppress,
                           mask::core::RemapLayerAction act = mask::core::RemapLayerAction::RemapKey,
                           uint8_t layer = 0, uint8_t targetLayer = 0,
                           mask::core::RemapUnlockPolicy pol = mask::core::RemapUnlockPolicy::Always,
                           int8_t monIdx = -1) noexcept {
            mask::core::RemapRuleConfig r{};
            r.slotIndex        = slot;
            r.enabled          = true;
            r.inUse            = true;
            r.suppressOriginal = suppress;
            r.anyDevice        = true;
            r.deviceKind       = 0U;
            r.layerIndex       = layer;
            r.unlockPolicy     = static_cast<uint8_t>(pol);
            r.actionKind       = static_cast<uint8_t>(act);
            r.targetLayer      = targetLayer;
            r.monitorIndex     = monIdx;
            r.sourceVk         = src;
            r.targetVk         = dst;
            return r;
        };

        auto resetRulesAndState = [&]() noexcept {
            std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules> blank{};
            for (size_t i = 0; i < blank.size(); ++i) {
                blank[i].slotIndex = static_cast<uint8_t>(i);
            }
            {
                AcquireSRWLockExclusive(&engine->ruleLock_);
                engine->rules_ = blank;
                ReleaseSRWLockExclusive(&engine->ruleLock_);
            }
            {
                AcquireSRWLockExclusive(&engine->holdLock_);
                engine->activeHolds_.fill(ActiveRemapHold{});
                ReleaseSRWLockExclusive(&engine->holdLock_);
            }
            state->activeRemapLayer.store(0U, std::memory_order_release);
            state->remapUnlocked.store(true, std::memory_order_release);
            mask::input::InputDispatcher::ClearVirtualEchoTokens(*state);
            std::array<mask::core::AuxOutputCommand, 16> trash{};
            (void)drainAux(trash);
        };

        auto setRulesOnly = [&](const std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>& rules) noexcept {
            AcquireSRWLockExclusive(&engine->ruleLock_);
            for (size_t i = 0; i < mask::core::kMaxRemapRules; ++i) {
                engine->rules_[i] = rules[i];
                detail::SanitizeRemapRule(engine->rules_[i], i);
            }
            ReleaseSRWLockExclusive(&engine->ruleLock_);
        };

        std::array<mask::core::AuxOutputCommand, 16> cmds{};
        uint64_t t = 1'000'000'000ULL;

        // 1. KeyDown / AutoRepeat / KeyUp release (both suppressed and non-suppressed)
        {
            resetRulesAndState();
            std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules> rules{};
            rules[0] = makeRule(0, 'Q', 'W', true);
            rules[1] = makeRule(1, 'T', 'U', false);
            setRulesOnly(rules);

            if (!engine->ProcessHookKeyEvent('Q', 0, 0, true, t)) return false;
            if (drainAux(cmds) != 1U || cmds[0].kind != static_cast<uint8_t>(mask::core::AuxActionKind::KeyDown) || cmds[0].vk != 'W') return false;

            // OS auto-repeat must swallow physical key and emit NO duplicate KeyDown
            if (!engine->ProcessHookKeyEvent('Q', 0, 0, true, t + 10'000ULL)) return false;
            if (drainAux(cmds) != 0U) return false;

            // KeyUp releases 'W' and swallows matching 'Q' Up
            if (!engine->ProcessHookKeyEvent('Q', 0, 0, false, t + 20'000ULL)) return false;
            if (drainAux(cmds) != 1U || cmds[0].kind != static_cast<uint8_t>(mask::core::AuxActionKind::KeyUp) || cmds[0].vk != 'W') return false;

            // Non-suppressed rule passes original through on Down, Repeat, and Up
            if (engine->ProcessHookKeyEvent('T', 0, 0, true, t + 30'000ULL)) return false;
            if (drainAux(cmds) != 1U || cmds[0].vk != 'U') return false;
            if (engine->ProcessHookKeyEvent('T', 0, 0, true, t + 40'000ULL)) return false;
            if (drainAux(cmds) != 0U) return false;
            if (engine->ProcessHookKeyEvent('T', 0, 0, false, t + 50'000ULL)) return false;
            if (drainAux(cmds) != 1U || cmds[0].kind != static_cast<uint8_t>(mask::core::AuxActionKind::KeyUp) || cmds[0].vk != 'U') return false;
        }

        // 2. A <-> B bidirectional swap via virtual echo token consumption (plus simultaneous press & expiry)
        {
            resetRulesAndState();
            std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules> rules{};
            rules[0] = makeRule(0, 'A', 'B', true);
            rules[1] = makeRule(1, 'B', 'A', true);
            setRulesOnly(rules);

            // Physical 'A' down -> suppresses 'A', emits 'B' down
            if (!engine->ProcessHookKeyEvent('A', 0, 0, true, t)) return false;
            if (drainAux(cmds) != 1U || cmds[0].kind != static_cast<uint8_t>(mask::core::AuxActionKind::KeyDown) || cmds[0].vk != 'B') return false;

            // Virtual echo 'B' down arrives right after HIDMaestro submission -> passes straight through, zero loop
            unsigned int auxCpu = 0;
            const uint64_t realNow = __rdtscp(&auxCpu);
            mask::input::InputDispatcher::StampVirtualEchoToken(*state, 'B', true);
            if (engine->ProcessHookKeyEvent('B', 0, 0, true, realNow)) return false;
            if (drainAux(cmds) != 0U) return false;

            // Simultaneous physical 'B' down -> suppresses 'B', emits 'A' down
            if (!engine->ProcessHookKeyEvent('B', 0, 0, true, realNow + 1000ULL)) return false;
            if (drainAux(cmds) != 1U || cmds[0].kind != static_cast<uint8_t>(mask::core::AuxActionKind::KeyDown) || cmds[0].vk != 'A') return false;

            mask::input::InputDispatcher::StampVirtualEchoToken(*state, 'A', true);
            if (engine->ProcessHookKeyEvent('A', 0, 0, true, realNow + 2000ULL)) return false;
            if (drainAux(cmds) != 0U) return false;

            // Release physical 'A' -> suppresses 'A' Up, emits 'B' Up, echo 'B' Up passes through
            if (!engine->ProcessHookKeyEvent('A', 0, 0, false, realNow + 3000ULL)) return false;
            if (drainAux(cmds) != 1U || cmds[0].kind != static_cast<uint8_t>(mask::core::AuxActionKind::KeyUp) || cmds[0].vk != 'B') return false;
            mask::input::InputDispatcher::StampVirtualEchoToken(*state, 'B', false);
            if (engine->ProcessHookKeyEvent('B', 0, 0, false, realNow + 4000ULL)) return false;
            if (drainAux(cmds) != 0U) return false;

            // Release physical 'B' -> suppresses 'B' Up, emits 'A' Up, echo 'A' Up passes through
            if (!engine->ProcessHookKeyEvent('B', 0, 0, false, realNow + 5000ULL)) return false;
            if (drainAux(cmds) != 1U || cmds[0].kind != static_cast<uint8_t>(mask::core::AuxActionKind::KeyUp) || cmds[0].vk != 'A') return false;
            mask::input::InputDispatcher::StampVirtualEchoToken(*state, 'A', false);
            if (engine->ProcessHookKeyEvent('A', 0, 0, false, realNow + 6000ULL)) return false;
            if (drainAux(cmds) != 0U) return false;

            // Expired token (> 50 ms) must NOT swallow/pass-through a later physical press
            state->virtualEchoDownTsc['B'].store(realNow, std::memory_order_release);
            state->virtualEchoDownCount['B'].store(1U, std::memory_order_release);
            if (!engine->ProcessHookKeyEvent('B', 0, 0, true, realNow + win50Ms + 50'000ULL)) return false;
            if (state->virtualEchoDownCount['B'].load(std::memory_order_acquire) != 0U) return false;
            if (drainAux(cmds) != 1U || cmds[0].vk != 'A') return false;
            if (!engine->ProcessHookKeyEvent('B', 0, 0, false, realNow + win50Ms + 60'000ULL)) return false;
            (void)drainAux(cmds);
        }

        // 3. BlockKey (suppresses Down, Repeat, and Up even if suppressOriginal was false)
        {
            resetRulesAndState();
            std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules> rules{};
            rules[0] = makeRule(0, 'X', 0, false, mask::core::RemapLayerAction::BlockKey);
            setRulesOnly(rules);

            if (!engine->ProcessHookKeyEvent('X', 0, 0, true, t)) return false;
            if (!engine->ProcessHookKeyEvent('X', 0, 0, true, t + 1000ULL)) return false;
            if (!engine->ProcessHookKeyEvent('X', 0, 0, false, t + 2000ULL)) return false;
            if (drainAux(cmds) != 0U) return false;
        }

        // 4. ShiftLayerHold + layer 1 remap + base fallback
        {
            resetRulesAndState();
            std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules> rules{};
            rules[0] = makeRule(0, VK_CAPITAL, 0, true, mask::core::RemapLayerAction::ShiftLayerHold, 0, 1);
            rules[1] = makeRule(1, 'E', 'R', true, mask::core::RemapLayerAction::RemapKey, 1, 0);
            setRulesOnly(rules);

            // Base layer: 'E' passes through untouched
            if (engine->ProcessHookKeyEvent('E', 0, 0, true, t)) return false;
            if (engine->ProcessHookKeyEvent('E', 0, 0, false, t + 500ULL)) return false;
            if (drainAux(cmds) != 0U) return false;

            // Hold CapsLock -> shifts to Layer 1
            if (!engine->ProcessHookKeyEvent(VK_CAPITAL, 0, 0, true, t + 1000ULL)) return false;
            if (state->activeRemapLayer.load(std::memory_order_acquire) != 1U) return false;

            // 'E' now remaps to 'R'
            if (!engine->ProcessHookKeyEvent('E', 0, 0, true, t + 2000ULL)) return false;
            if (drainAux(cmds) != 1U || cmds[0].vk != 'R' || cmds[0].kind != static_cast<uint8_t>(mask::core::AuxActionKind::KeyDown)) return false;
            if (!engine->ProcessHookKeyEvent('E', 0, 0, false, t + 3000ULL)) return false;
            if (drainAux(cmds) != 1U || cmds[0].vk != 'R' || cmds[0].kind != static_cast<uint8_t>(mask::core::AuxActionKind::KeyUp)) return false;

            // Release CapsLock -> restores Layer 0
            if (!engine->ProcessHookKeyEvent(VK_CAPITAL, 0, 0, false, t + 4000ULL)) return false;
            if (state->activeRemapLayer.load(std::memory_order_acquire) != 0U) return false;
        }

        // 5. ToggleLayer (auto-repeat safe!)
        {
            resetRulesAndState();
            std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules> rules{};
            rules[0] = makeRule(0, VK_F1, 0, true, mask::core::RemapLayerAction::ToggleLayer, 0, 2);
            setRulesOnly(rules);

            if (!engine->ProcessHookKeyEvent(VK_F1, 0, 0, true, t)) return false;
            if (state->activeRemapLayer.load(std::memory_order_acquire) != 2U) return false;
            if (!engine->ProcessHookKeyEvent(VK_F1, 0, 0, true, t + 1000ULL)) return false; // repeat must NOT toggle back
            if (state->activeRemapLayer.load(std::memory_order_acquire) != 2U) return false;
            if (!engine->ProcessHookKeyEvent(VK_F1, 0, 0, false, t + 2000ULL)) return false;
            if (state->activeRemapLayer.load(std::memory_order_acquire) != 2U) return false;

            if (!engine->ProcessHookKeyEvent(VK_F1, 0, 0, true, t + 3000ULL)) return false;
            if (state->activeRemapLayer.load(std::memory_order_acquire) != 0U) return false;
            if (!engine->ProcessHookKeyEvent(VK_F1, 0, 0, false, t + 4000ULL)) return false;
        }

        // 6. ToggleUnlockState + UnlockPolicy
        {
            resetRulesAndState();
            std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules> rules{};
            rules[0] = makeRule(0, VK_F2, 0, true, mask::core::RemapLayerAction::ToggleUnlockState);
            rules[1] = makeRule(1, 'K', 'L', true, mask::core::RemapLayerAction::RemapKey, 0, 0, mask::core::RemapUnlockPolicy::WhenUnlocked);
            rules[2] = makeRule(2, 'K', 'M', true, mask::core::RemapLayerAction::RemapKey, 0, 0, mask::core::RemapUnlockPolicy::WhenLocked);
            setRulesOnly(rules);

            if (!engine->ProcessHookKeyEvent('K', 0, 0, true, t)) return false;
            if (drainAux(cmds) != 1U || cmds[0].vk != 'L') return false;
            if (!engine->ProcessHookKeyEvent('K', 0, 0, false, t + 500ULL)) return false;
            (void)drainAux(cmds);

            if (!engine->ProcessHookKeyEvent(VK_F2, 0, 0, true, t + 1000ULL)) return false;
            if (state->remapUnlocked.load(std::memory_order_acquire)) return false;
            if (!engine->ProcessHookKeyEvent(VK_F2, 0, 0, false, t + 1500ULL)) return false;

            if (!engine->ProcessHookKeyEvent('K', 0, 0, true, t + 2000ULL)) return false;
            if (drainAux(cmds) != 1U || cmds[0].vk != 'M') return false;
            if (!engine->ProcessHookKeyEvent('K', 0, 0, false, t + 2500ULL)) return false;
            (void)drainAux(cmds);
        }

        // 7. WarpToMonitor
        {
            resetRulesAndState();
            std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules> rules{};
            rules[0] = makeRule(0, 'G', 0, true, mask::core::RemapLayerAction::WarpToMonitor, 0, 0,
                                mask::core::RemapUnlockPolicy::Always, 1);
            setRulesOnly(rules);

            if (!engine->ProcessHookKeyEvent('G', 0, 0, true, t)) return false;
            if (drainAux(cmds) != 1U ||
                cmds[0].kind != static_cast<uint8_t>(mask::core::AuxActionKind::MouseMoveAbs) ||
                cmds[0].argX != 2880 || cmds[0].argY != 540) {
                return false;
            }
            if (!engine->ProcessHookKeyEvent('G', 0, 0, false, t + 1000ULL)) return false;
        }

        // 8. Safety VK + Generic Modifier Normalization + Mouse Hook Path
        {
            resetRulesAndState();
            std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules> rules{};
            rules[0] = makeRule(0, VK_ESCAPE, 0, true, mask::core::RemapLayerAction::BlockKey);
            rules[1] = makeRule(1, VK_SHIFT, 'Z', true);
            rules[2] = makeRule(2, VK_XBUTTON1, 'V', true);
            setRulesOnly(rules);

            // Esc must never be blocked
            if (engine->ProcessHookKeyEvent(VK_ESCAPE, 0, 0, true, t)) return false;
            if (drainAux(cmds) != 0U) return false;

            // Rule bound to generic VK_SHIFT matches VK_LSHIFT
            if (!engine->ProcessHookKeyEvent(VK_LSHIFT, 0x2A, 0, true, t + 1000ULL)) return false;
            if (drainAux(cmds) != 1U || cmds[0].vk != 'Z' || cmds[0].kind != static_cast<uint8_t>(mask::core::AuxActionKind::KeyDown)) return false;
            if (!engine->ProcessHookKeyEvent(VK_LSHIFT, 0x2A, 0, false, t + 2000ULL)) return false;
            if (drainAux(cmds) != 1U || cmds[0].vk != 'Z' || cmds[0].kind != static_cast<uint8_t>(mask::core::AuxActionKind::KeyUp)) return false;

            // Virtual echo stamped with generic VK_SHIFT passes through VK_LSHIFT
            unsigned int auxCpu = 0;
            const uint64_t realNow = __rdtscp(&auxCpu);
            mask::input::InputDispatcher::StampVirtualEchoToken(*state, VK_SHIFT, true);
            if (engine->ProcessHookKeyEvent(VK_LSHIFT, 0x2A, 0, true, realNow)) return false;
            if (drainAux(cmds) != 0U) return false;

            // Mouse button hook test
            if (!engine->ProcessHookMouseButtonEvent(VK_XBUTTON1, true, 0, realNow + 1000ULL)) return false;
            if (drainAux(cmds) != 1U || cmds[0].vk != 'V' || cmds[0].kind != static_cast<uint8_t>(mask::core::AuxActionKind::KeyDown)) return false;
            if (!engine->ProcessHookMouseButtonEvent(VK_XBUTTON1, false, 0, realNow + 2000ULL)) return false;
            if (drainAux(cmds) != 1U || cmds[0].vk != 'V' || cmds[0].kind != static_cast<uint8_t>(mask::core::AuxActionKind::KeyUp)) return false;
        }

        return true;
    }

private:
    static constexpr size_t kMaxActiveHolds = 32U;
    static constexpr size_t kMaxRawInventory = mask::input::kMaxPhysicalGameInputDevices;
    static constexpr size_t kMaxRawHandles   = 64U;
    static constexpr UINT   kMsgSyncHooks    = WM_APP + 101U;
    static constexpr UINT   kMsgRefreshRaw   = WM_APP + 102U;
    static constexpr const wchar_t* kHookWndClass = L"MaskRemapHookMsgWindow";

    inline static std::atomic<RemapEngine*> activeInstance_{nullptr};

    struct RawHandleEntry {
        HANDLE   hDevice{nullptr};
        uint8_t  deviceIdBytes[32]{};
        uint16_t vendorId{0};
        uint16_t productId{0};
        uint8_t  devKind{0}; // 1 = Keyboard, 2 = Mouse
        bool     isVirtual{false};
    };

    struct CorrelatedDevice {
        bool     hasRawId{false};
        bool     hasGiId{false};
        uint8_t  rawIdBytes[32]{};
        uint8_t  giIdBytes[32]{};
        uint16_t vendorId{0};
        uint16_t productId{0};
    };

    struct RawTransitionRecord {
        bool     valid{false};
        uint8_t  deviceIdBytes[32]{};
        uint16_t vendorId{0};
        uint16_t productId{0};
        uint64_t timestampTsc{0};
    };

    struct GiTransitionRecord {
        bool     valid{false};
        uint8_t  deviceIdBytes[32]{};
        uint64_t timestampTsc{0};
    };

    struct ActiveRemapHold {
        bool     active{false};
        bool     hasDeviceId{false};
        bool     wasSuppressed{false};
        uint8_t  deviceId[32]{};
        uint16_t sourceVk{0};
        uint16_t emittedTargetVk{0};
        uint8_t  actionKind{0};
        uint8_t  prevLayer{0};
    };

    struct ActiveHoldInfo {
        bool wasSuppressed{false};
    };

    // ---- Hook Thread & Raw Input Listener (`RemapHookPipeline`) --------------------

    void SyncHooks(bool wantKeyboardHook, bool wantMouseHook) noexcept {
        const bool wantAny = wantKeyboardHook || wantMouseHook;

        if (GetCurrentThreadId() == hookThreadId_.load(std::memory_order_acquire) && hookThreadId_.load(std::memory_order_acquire) != 0U) {
            RequestSyncFromHookThread(wantKeyboardHook, wantMouseHook);
            return;
        }

        HANDLE joinHandle = nullptr;
        DWORD  joinTid = 0U;

        AcquireSRWLockExclusive(&hookLock_);
        wantKeyboard_.store(wantKeyboardHook, std::memory_order_release);
        wantMouse_.store(wantMouseHook, std::memory_order_release);
        desiredActive_.store(wantAny, std::memory_order_release);

        if (wantAny) {
            activeInstance_.store(this, std::memory_order_release);

            if (hookThreadHandle_ != nullptr && WaitForSingleObject(hookThreadHandle_, 0U) == WAIT_OBJECT_0) {
                CloseHandle(hookThreadHandle_);
                hookThreadHandle_ = nullptr;
                hookThreadId_.store(0U, std::memory_order_release);
            }

            if (hookThreadHandle_ == nullptr) {
                if (hookReadyEvent_ == nullptr) {
                    hookReadyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                } else {
                    ResetEvent(hookReadyEvent_);
                }
                DWORD tid = 0;
                hookThreadHandle_ = CreateThread(nullptr, 0, &RemapEngine::HookThreadEntry, this, 0, &tid);
                if (hookThreadHandle_ != nullptr) {
                    hookThreadId_.store(tid, std::memory_order_release);
                    if (hookReadyEvent_ != nullptr) {
                        WaitForSingleObject(hookReadyEvent_, 500U);
                    }
                }
            } else {
                const DWORD tid = hookThreadId_.load(std::memory_order_acquire);
                if (tid != 0U) {
                    PostThreadMessageW(tid, kMsgSyncHooks, 0, 0);
                }
            }
        } else {
            joinTid = hookThreadId_.load(std::memory_order_acquire);
            if (joinTid != 0U) {
                PostThreadMessageW(joinTid, WM_QUIT, 0, 0);
            }
            joinHandle = hookThreadHandle_;
            hookThreadHandle_ = nullptr;
            hookThreadId_.store(0U, std::memory_order_release);
        }
        ReleaseSRWLockExclusive(&hookLock_);

        if (joinHandle != nullptr) {
            if (WaitForSingleObject(joinHandle, 50U) == WAIT_TIMEOUT && joinTid != 0U) {
                PostThreadMessageW(joinTid, WM_QUIT, 0, 0);
                WaitForSingleObject(joinHandle, 2000U);
            }
            CloseHandle(joinHandle);
        }
    }

    void RequestSyncFromHookThread(bool wantKeyboardHook, bool wantMouseHook) noexcept {
        wantKeyboard_.store(wantKeyboardHook, std::memory_order_release);
        wantMouse_.store(wantMouseHook, std::memory_order_release);
        desiredActive_.store(wantKeyboardHook || wantMouseHook, std::memory_order_release);
        const DWORD tid = hookThreadId_.load(std::memory_order_acquire);
        if (tid != 0U) {
            PostThreadMessageW(tid, kMsgSyncHooks, 0, 0);
        }
    }

    void ShutdownHooks() noexcept {
        SyncHooks(false, false);

        AcquireSRWLockExclusive(&hookLock_);
        if (hookReadyEvent_ != nullptr) {
            CloseHandle(hookReadyEvent_);
            hookReadyEvent_ = nullptr;
        }
        ReleaseSRWLockExclusive(&hookLock_);

        RemapEngine* expected = this;
        activeInstance_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    }

    static DWORD WINAPI HookThreadEntry(LPVOID param) noexcept {
        auto* self = static_cast<RemapEngine*>(param);
        if (self != nullptr) {
            self->HookThreadLoop();
        }
        return 0;
    }

    static LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
        if (msg == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            if (cs != nullptr) {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            }
            return TRUE;
        }
        if (msg == WM_INPUT) {
            auto* self = reinterpret_cast<RemapEngine*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (self != nullptr) {
                self->HandleRawInputMessage(reinterpret_cast<HRAWINPUT>(lParam));
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) noexcept {
        if (nCode != HC_ACTION || lParam == 0) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }
        RemapEngine* self = activeInstance_.load(std::memory_order_acquire);
        if (self == nullptr) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        const bool isUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
        if (!isDown && !isUp) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        unsigned int aux = 0;
        const uint64_t nowTsc = __rdtscp(&aux);
        if (self->ProcessHookKeyEvent(static_cast<uint16_t>(kb->vkCode), kb->scanCode, kb->flags, isDown, nowTsc)) {
            return 1;
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) noexcept {
        if (nCode != HC_ACTION || lParam == 0) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        uint16_t vk = 0U;
        bool isDown = false;
        switch (wParam) {
        case WM_LBUTTONDOWN: vk = VK_LBUTTON; isDown = true;  break;
        case WM_LBUTTONUP:   vk = VK_LBUTTON; isDown = false; break;
        case WM_RBUTTONDOWN: vk = VK_RBUTTON; isDown = true;  break;
        case WM_RBUTTONUP:   vk = VK_RBUTTON; isDown = false; break;
        case WM_MBUTTONDOWN: vk = VK_MBUTTON; isDown = true;  break;
        case WM_MBUTTONUP:   vk = VK_MBUTTON; isDown = false; break;
        case WM_XBUTTONDOWN:
            vk = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            isDown = true;
            break;
        case WM_XBUTTONUP:
            vk = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            isDown = false;
            break;
        default:
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        RemapEngine* self = activeInstance_.load(std::memory_order_acquire);
        if (self == nullptr) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }
        unsigned int aux = 0;
        const uint64_t nowTsc = __rdtscp(&aux);
        if (self->ProcessHookMouseButtonEvent(vk, isDown, ms->flags, nowTsc)) {
            return 1;
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    void SyncInstalledHooks(HHOOK& kbHook, HHOOK& mouseHook) noexcept {
        const bool active = desiredActive_.load(std::memory_order_acquire);
        const bool wantKb = active && wantKeyboard_.load(std::memory_order_acquire);
        const bool wantMs = active && wantMouse_.load(std::memory_order_acquire);

        if (wantKb && kbHook == nullptr) {
            kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, &RemapEngine::LowLevelKeyboardProc, nullptr, 0);
        } else if (!wantKb && kbHook != nullptr) {
            UnhookWindowsHookEx(kbHook);
            kbHook = nullptr;
        }

        if (wantMs && mouseHook == nullptr) {
            mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &RemapEngine::LowLevelMouseProc, nullptr, 0);
        } else if (!wantMs && mouseHook != nullptr) {
            UnhookWindowsHookEx(mouseHook);
            mouseHook = nullptr;
        }

        kbHookActive_.store(kbHook != nullptr, std::memory_order_release);
        mouseHookActive_.store(mouseHook != nullptr, std::memory_order_release);
    }

    void HookThreadLoop() noexcept {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        MSG msg{};
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        // Register hidden HWND_MESSAGE listener for background Raw Input per-device correlation
        HWND rawHwnd = nullptr;
        bool rawRegistered = false;
        const HINSTANCE hInst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = &RemapEngine::HookWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = kHookWndClass;
        SetLastError(0);
        const ATOM clsAtom = RegisterClassExW(&wc);
        if (clsAtom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            rawHwnd = CreateWindowExW(
                0U, kHookWndClass, L"", 0U,
                0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, this);
            if (rawHwnd != nullptr) {
                RAWINPUTDEVICE rids[2]{};
                rids[0].usUsagePage = 0x01U;
                rids[0].usUsage     = 0x06U; // Keyboard
                rids[0].dwFlags     = RIDEV_INPUTSINK;
                rids[0].hwndTarget  = rawHwnd;
                rids[1].usUsagePage = 0x01U;
                rids[1].usUsage     = 0x02U; // Mouse
                rids[1].dwFlags     = RIDEV_INPUTSINK;
                rids[1].hwndTarget  = rawHwnd;
                rawRegistered = (RegisterRawInputDevices(rids, 2U, sizeof(RAWINPUTDEVICE)) != FALSE);
            }
        }

        HHOOK kbHook = nullptr;
        HHOOK mouseHook = nullptr;
        if (desiredActive_.load(std::memory_order_acquire)) {
            SyncInstalledHooks(kbHook, mouseHook);
        }

        if (hookReadyEvent_ != nullptr) {
            SetEvent(hookReadyEvent_);
        }

        if (desiredActive_.load(std::memory_order_acquire) &&
            (kbHook != nullptr || mouseHook != nullptr)) {
            while (desiredActive_.load(std::memory_order_acquire)) {
                const BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
                if (ret <= 0) {
                    break;
                }
                if (msg.hwnd == nullptr) {
                    if (msg.message == kMsgSyncHooks) {
                        SyncInstalledHooks(kbHook, mouseHook);
                        if (!desiredActive_.load(std::memory_order_acquire) ||
                            (kbHook == nullptr && mouseHook == nullptr)) {
                            break;
                        }
                    } else if (msg.message == kMsgRefreshRaw) {
                        rawRefreshQueued_.store(false, std::memory_order_release);
                        RefreshRawInputDevices();
                    }
                } else {
                    DispatchMessageW(&msg);
                }
            }
        }

        if (kbHook != nullptr) {
            UnhookWindowsHookEx(kbHook);
            kbHook = nullptr;
        }
        if (mouseHook != nullptr) {
            UnhookWindowsHookEx(mouseHook);
            mouseHook = nullptr;
        }
        kbHookActive_.store(false, std::memory_order_release);
        mouseHookActive_.store(false, std::memory_order_release);

        if (rawRegistered) {
            RAWINPUTDEVICE removeRids[2]{};
            removeRids[0].usUsagePage = 0x01U;
            removeRids[0].usUsage     = 0x06U;
            removeRids[0].dwFlags     = RIDEV_REMOVE;
            removeRids[1].usUsagePage = 0x01U;
            removeRids[1].usUsage     = 0x02U;
            removeRids[1].dwFlags     = RIDEV_REMOVE;
            RegisterRawInputDevices(removeRids, 2U, sizeof(RAWINPUTDEVICE));
        }
        if (rawHwnd != nullptr) {
            DestroyWindow(rawHwnd);
        }
        hookThreadId_.store(0U, std::memory_order_release);
    }

    void HandleRawInputMessage(HRAWINPUT hRawInput) noexcept {
        if (hRawInput == nullptr) {
            return;
        }
        RAWINPUT raw{};
        UINT sz = sizeof(raw);
        if (GetRawInputData(hRawInput, RID_INPUT, &raw, &sz, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) {
            return;
        }
        if (raw.header.hDevice == nullptr) {
            return;
        }
        if (raw.header.dwType != RIM_TYPEKEYBOARD && raw.header.dwType != RIM_TYPEMOUSE) {
            return;
        }

        bool found = false;
        bool isVirtual = false;
        uint8_t devIdBytes[32]{};
        uint16_t vid = 0U;
        uint16_t pid = 0U;
        uint8_t devKind = 0U;

        AcquireSRWLockShared(&rawLock_);
        for (size_t i = 0; i < rawHandleCount_; ++i) {
            const auto& e = rawHandleMap_[i];
            if (e.hDevice == raw.header.hDevice) {
                found     = true;
                isVirtual = e.isVirtual;
                vid       = e.vendorId;
                pid       = e.productId;
                devKind   = e.devKind;
                std::memcpy(devIdBytes, e.deviceIdBytes, 32U);
                break;
            }
        }
        ReleaseSRWLockShared(&rawLock_);

        if (!found) {
            // Newly plugged device: request async Raw Input refresh on hook thread
            if (!rawRefreshQueued_.exchange(true, std::memory_order_acq_rel)) {
                const DWORD tid = hookThreadId_.load(std::memory_order_acquire);
                if (tid != 0U) {
                    PostThreadMessageW(tid, kMsgRefreshRaw, 0, 0);
                }
            }
            return;
        }
        if (isVirtual) {
            return;
        }

        unsigned int aux = 0;
        const uint64_t nowTsc = __rdtscp(&aux);

        AcquireSRWLockExclusive(&corrLock_);
        if (devKind == 1U && raw.header.dwType == RIM_TYPEKEYBOARD) {
            uint16_t vk = static_cast<uint16_t>(raw.data.keyboard.VKey & 0xFFU);
            if (vk != 0U && vk != 0xFFU) {
                if (vk == VK_SHIFT) {
                    vk = (raw.data.keyboard.MakeCode == 0x36U) ? VK_RSHIFT : VK_LSHIFT;
                } else if (vk == VK_CONTROL) {
                    vk = ((raw.data.keyboard.Flags & RI_KEY_E0) != 0U) ? VK_RCONTROL : VK_LCONTROL;
                } else if (vk == VK_MENU) {
                    vk = ((raw.data.keyboard.Flags & RI_KEY_E0) != 0U) ? VK_RMENU : VK_LMENU;
                }
                RawTransitionRecord rec{};
                rec.valid        = true;
                rec.vendorId     = vid;
                rec.productId    = pid;
                rec.timestampTsc = nowTsc;
                std::memcpy(rec.deviceIdBytes, devIdBytes, 32U);
                mostRecentKbRaw_ = rec;
                perVkRaw_[vk]    = rec;
            }
        } else if (devKind == 2U && raw.header.dwType == RIM_TYPEMOUSE) {
            RawTransitionRecord rec{};
            rec.valid        = true;
            rec.vendorId     = vid;
            rec.productId    = pid;
            rec.timestampTsc = nowTsc;
            std::memcpy(rec.deviceIdBytes, devIdBytes, 32U);
            mostRecentMouseRaw_ = rec;
        }
        ReleaseSRWLockExclusive(&corrLock_);
    }

    // ---- Unified Hook Event Evaluation & Execution ---------------------------------

    [[nodiscard]] bool ProcessHookKeyEvent(
        uint16_t rawVk,
        DWORD scanCode,
        DWORD flags,
        bool isDown,
        uint64_t nowTsc
    ) noexcept {
        const uint16_t vk = detail::NormalizeHookVk(rawVk, scanCode, flags);
        if (vk == 0U || vk >= 256U) {
            return false;
        }

        // 1. Virtual Echo Pass-Through FIRST! (HIDMaestro UMDF2 events carry no LLKHF_INJECTED flag.)
        const uint64_t winTsc = echoWindowTsc_.load(std::memory_order_relaxed);
        if (detail::ConsumeVirtualEchoToken(sharedState_, vk, isDown, nowTsc, winTsc)) {
            return false;
        }

        // 2. Pass through injected events & protected safety keys
        if ((flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED)) != 0U) {
            return false;
        }
        const uint16_t pauseVk   = sharedState_.globalPauseVk.load(std::memory_order_relaxed);
        const uint16_t stopAllVk = sharedState_.globalStopAllVk.load(std::memory_order_relaxed);
        if (detail::IsProtectedSafetyVk(vk, pauseVk, stopAllVk)) {
            return false;
        }

        // 3. Key-Up: always release any active RemapHold cleanly (even if paused/suppressed/layer changed mid-hold)
        if (!isDown) {
            ActiveHoldInfo released{};
            if (HandleKeyRelease(nullptr, vk, nowTsc, &released)) {
                return released.wasSuppressed;
            }
            return false;
        }

        // 4. Skip new Key-Downs while stopped, paused, capturing a bind, or typing in UI
        if (!started_.load(std::memory_order_acquire) ||
            sharedState_.automationPaused.load(std::memory_order_acquire) ||
            sharedState_.captureBindTarget.load(std::memory_order_relaxed) != 0U ||
            sharedState_.suppressTriggers.load(std::memory_order_relaxed)) {
            return false;
        }

        // 5. Key-Down auto-repeat guard: never re-fire while held; swallow repeats if original was suppressed
        ActiveHoldInfo existing{};
        if (FindExistingHold(nullptr, vk, existing)) {
            return existing.wasSuppressed;
        }

        // 6. Evaluate matching rule using correlated Raw Input + GameInput device identity
        uint8_t activeLayer = sharedState_.activeRemapLayer.load(std::memory_order_acquire);
        if (activeLayer >= mask::core::kMaxRemapLayers) {
            activeLayer = 0U;
        }
        const bool unlocked = sharedState_.remapUnlocked.load(std::memory_order_acquire);
        const CorrelatedDevice corr = GetCorrelatedDevice(1U, vk, nowTsc, winTsc);

        mask::core::RemapRuleConfig winner{};
        if (!FindMatchingRule(vk, 1U, activeLayer, unlocked, corr, winner)) {
            return false;
        }

        const bool shouldSuppress =
            winner.suppressOriginal ||
            (winner.actionKind == static_cast<uint8_t>(mask::core::RemapLayerAction::BlockKey));
        const uint8_t* devBytes = corr.hasRawId ? corr.rawIdBytes : (corr.hasGiId ? corr.giIdBytes : nullptr);
        ExecuteRuleOnDown(winner, devBytes, vk, activeLayer, unlocked, nowTsc, shouldSuppress);
        return shouldSuppress;
    }

    [[nodiscard]] bool ProcessHookMouseButtonEvent(
        uint16_t vk,
        bool isDown,
        DWORD flags,
        uint64_t nowTsc
    ) noexcept {
        if (vk == 0U || vk >= 256U) {
            return false;
        }

        // 1. Virtual Echo Pass-Through FIRST!
        const uint64_t winTsc = echoWindowTsc_.load(std::memory_order_relaxed);
        if (detail::ConsumeVirtualEchoToken(sharedState_, vk, isDown, nowTsc, winTsc)) {
            return false;
        }

        // 2. Pass through injected events & protected safety keys
        if ((flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) != 0U) {
            return false;
        }
        const uint16_t pauseVk   = sharedState_.globalPauseVk.load(std::memory_order_relaxed);
        const uint16_t stopAllVk = sharedState_.globalStopAllVk.load(std::memory_order_relaxed);
        if (detail::IsProtectedSafetyVk(vk, pauseVk, stopAllVk)) {
            return false;
        }

        // 3. Mouse Button Up: always release active hold cleanly
        if (!isDown) {
            ActiveHoldInfo released{};
            if (HandleKeyRelease(nullptr, vk, nowTsc, &released)) {
                return released.wasSuppressed;
            }
            return false;
        }

        // 4. Skip new presses while stopped, paused, binding, or typing
        if (!started_.load(std::memory_order_acquire) ||
            sharedState_.automationPaused.load(std::memory_order_acquire) ||
            sharedState_.captureBindTarget.load(std::memory_order_relaxed) != 0U ||
            sharedState_.suppressTriggers.load(std::memory_order_relaxed)) {
            return false;
        }

        // 5. Duplicate press guard
        ActiveHoldInfo existing{};
        if (FindExistingHold(nullptr, vk, existing)) {
            return existing.wasSuppressed;
        }

        // 6. Evaluate matching rule
        uint8_t activeLayer = sharedState_.activeRemapLayer.load(std::memory_order_acquire);
        if (activeLayer >= mask::core::kMaxRemapLayers) {
            activeLayer = 0U;
        }
        const bool unlocked = sharedState_.remapUnlocked.load(std::memory_order_acquire);
        const CorrelatedDevice corr = GetCorrelatedDevice(2U, vk, nowTsc, winTsc);

        mask::core::RemapRuleConfig winner{};
        if (!FindMatchingRule(vk, 2U, activeLayer, unlocked, corr, winner)) {
            return false;
        }

        const bool shouldSuppress =
            winner.suppressOriginal ||
            (winner.actionKind == static_cast<uint8_t>(mask::core::RemapLayerAction::BlockKey));
        const uint8_t* devBytes = corr.hasRawId ? corr.rawIdBytes : (corr.hasGiId ? corr.giIdBytes : nullptr);
        ExecuteRuleOnDown(winner, devBytes, vk, activeLayer, unlocked, nowTsc, shouldSuppress);
        return shouldSuppress;
    }

    // ---- Correlation, Matching & Hold Tracking -------------------------------------

    void RecordGameInputCorrelation(
        const uint8_t deviceId[32],
        uint8_t deviceKind,
        uint16_t vk,
        bool isDown,
        uint64_t nowTsc
    ) noexcept {
        if (deviceId == nullptr || vk >= 256U) {
            return;
        }
        AcquireSRWLockExclusive(&corrLock_);
        GiTransitionRecord rec{};
        rec.valid        = true;
        rec.timestampTsc = nowTsc;
        std::memcpy(rec.deviceIdBytes, deviceId, 32U);
        if (isDown) {
            perVkGi_[vk] = rec;
        }
        if (deviceKind == 1U) {
            mostRecentGiKb_ = rec;
        } else if (deviceKind == 2U) {
            mostRecentGiMouse_ = rec;
        }
        ReleaseSRWLockExclusive(&corrLock_);
    }

    void ResolveAliasVidPidFromGi(const uint8_t giId[32], uint16_t& outVid, uint16_t& outPid) const noexcept {
        outVid = 0U;
        outPid = 0U;
        std::array<mask::input::PhysicalDeviceInfo, kMaxRawInventory> giList{};
        const int count = mask::input::CopyPhysicalDeviceInventory(giList);
        for (int i = 0; i < count; ++i) {
            if (std::memcmp(giList[static_cast<size_t>(i)].deviceIdBytes, giId, 32U) == 0) {
                outVid = giList[static_cast<size_t>(i)].vendorId;
                outPid = giList[static_cast<size_t>(i)].productId;
                return;
            }
        }
    }

    [[nodiscard]] CorrelatedDevice GetCorrelatedDevice(
        uint8_t deviceKind,
        uint16_t vk,
        uint64_t nowTsc,
        uint64_t windowTsc
    ) const noexcept {
        CorrelatedDevice out{};
        auto isFresh = [nowTsc, windowTsc](uint64_t ts) noexcept -> bool {
            return (ts != 0ULL) && (nowTsc >= ts) && ((nowTsc - ts) <= windowTsc);
        };

        AcquireSRWLockShared(&corrLock_);
        if (deviceKind == 1U) {
            if (vk < 256U && perVkRaw_[vk].valid && isFresh(perVkRaw_[vk].timestampTsc)) {
                out.hasRawId  = true;
                out.vendorId  = perVkRaw_[vk].vendorId;
                out.productId = perVkRaw_[vk].productId;
                std::memcpy(out.rawIdBytes, perVkRaw_[vk].deviceIdBytes, 32U);
            } else if (mostRecentKbRaw_.valid && isFresh(mostRecentKbRaw_.timestampTsc)) {
                out.hasRawId  = true;
                out.vendorId  = mostRecentKbRaw_.vendorId;
                out.productId = mostRecentKbRaw_.productId;
                std::memcpy(out.rawIdBytes, mostRecentKbRaw_.deviceIdBytes, 32U);
            }

            if (vk < 256U && perVkGi_[vk].valid && isFresh(perVkGi_[vk].timestampTsc)) {
                out.hasGiId = true;
                std::memcpy(out.giIdBytes, perVkGi_[vk].deviceIdBytes, 32U);
            } else if (mostRecentGiKb_.valid && isFresh(mostRecentGiKb_.timestampTsc)) {
                out.hasGiId = true;
                std::memcpy(out.giIdBytes, mostRecentGiKb_.deviceIdBytes, 32U);
            }
        } else if (deviceKind == 2U) {
            if (mostRecentMouseRaw_.valid && isFresh(mostRecentMouseRaw_.timestampTsc)) {
                out.hasRawId  = true;
                out.vendorId  = mostRecentMouseRaw_.vendorId;
                out.productId = mostRecentMouseRaw_.productId;
                std::memcpy(out.rawIdBytes, mostRecentMouseRaw_.deviceIdBytes, 32U);
            }
            if (mostRecentGiMouse_.valid && isFresh(mostRecentGiMouse_.timestampTsc)) {
                out.hasGiId = true;
                std::memcpy(out.giIdBytes, mostRecentGiMouse_.deviceIdBytes, 32U);
            }
        }
        ReleaseSRWLockShared(&corrLock_);

        if (out.hasGiId && out.vendorId == 0U && out.productId == 0U) {
            ResolveAliasVidPidFromGi(out.giIdBytes, out.vendorId, out.productId);
        }
        return out;
    }

    [[nodiscard]] bool MatchesCorrelatedDevice(
        const mask::core::RemapRuleConfig& rule,
        const CorrelatedDevice& corr
    ) const noexcept {
        if (rule.anyDevice) {
            return true;
        }
        if (corr.hasRawId && std::memcmp(rule.deviceIdBytes, corr.rawIdBytes, 32U) == 0) {
            return true;
        }
        if (corr.hasGiId && std::memcmp(rule.deviceIdBytes, corr.giIdBytes, 32U) == 0) {
            return true;
        }

        // Cross-match persistent Raw ID ('MRW' tag at [21..23]) VID/PID or GameInput VID/PID fallback
        if (corr.vendorId != 0U || corr.productId != 0U) {
            if (rule.deviceIdBytes[21] == 'M' && rule.deviceIdBytes[22] == 'R' && rule.deviceIdBytes[23] == 'W') {
                const uint16_t ruleVid = static_cast<uint16_t>(rule.deviceIdBytes[16]) |
                                         (static_cast<uint16_t>(rule.deviceIdBytes[17]) << 8);
                const uint16_t rulePid = static_cast<uint16_t>(rule.deviceIdBytes[18]) |
                                         (static_cast<uint16_t>(rule.deviceIdBytes[19]) << 8);
                if (ruleVid == corr.vendorId && rulePid == corr.productId) {
                    return true;
                }
            }
        }

        // If the rule targets a specific single-present physical device and no recent correlation fired yet
        if (!corr.hasRawId && !corr.hasGiId) {
            int matchingKindCount = 0;
            bool ruleDevPresent = false;
            const uint8_t wantKind = rule.deviceKind;
            AcquireSRWLockShared(&rawLock_);
            for (int i = 0; i < rawDeviceCount_; ++i) {
                const auto& d = rawDevices_[static_cast<size_t>(i)];
                const bool kindMatch = (wantKind == 0U) ||
                                       (wantKind == 1U && d.isKeyboard) ||
                                       (wantKind == 2U && d.isMouse);
                if (kindMatch) {
                    ++matchingKindCount;
                }
                if (std::memcmp(d.deviceIdBytes, rule.deviceIdBytes, 32U) == 0) {
                    ruleDevPresent = true;
                }
            }
            ReleaseSRWLockShared(&rawLock_);
            if (ruleDevPresent && matchingKindCount == 1) {
                return true;
            }
        }

        return false;
    }

    [[nodiscard]] bool FindMatchingRule(
        uint16_t vk,
        uint8_t deviceKind,
        uint8_t activeLayer,
        bool unlocked,
        const CorrelatedDevice& corr,
        mask::core::RemapRuleConfig& outWinner
    ) const noexcept {
        bool hasCursorPos = false;
        POINT cursorPt{};

        bool foundExact    = false;
        bool foundFallback = false;
        mask::core::RemapRuleConfig exactRule{};
        mask::core::RemapRuleConfig fallbackRule{};

        auto scoreRule = [vk](const mask::core::RemapRuleConfig& r) noexcept -> int {
            int s = 0;
            if ((r.sourceVk & 0xFFU) == vk) s += 4;
            if (!r.anyDevice)                s += 2;
            if (r.monitorIndex >= 0)         s += 1;
            return s;
        };

        AcquireSRWLockShared(&ruleLock_);
        for (size_t i = 0; i < mask::core::kMaxRemapRules; ++i) {
            const auto& r = rules_[i];
            if (!r.enabled || !r.inUse) {
                continue;
            }
            if (!detail::VkMatchesRuleSource(r.sourceVk, vk)) {
                continue;
            }
            if (r.deviceKind != 0U && r.deviceKind != deviceKind) {
                continue;
            }
            if (!MatchesCorrelatedDevice(r, corr)) {
                continue;
            }
            if (!MatchesUnlockPolicy(r.unlockPolicy, unlocked)) {
                continue;
            }

            const bool isExact    = (r.layerIndex == activeLayer);
            const bool isFallback = (activeLayer > 0U && r.layerIndex == 0U);
            if (!isExact && !isFallback) {
                continue;
            }

            const auto action = static_cast<mask::core::RemapLayerAction>(r.actionKind);
            if (action != mask::core::RemapLayerAction::WarpToMonitor && r.monitorIndex >= 0) {
                if (!hasCursorPos) {
                    hasCursorPos = (GetCursorPos(&cursorPt) != FALSE);
                }
                if (!hasCursorPos || !IsPointInMonitor(cursorPt, r.monitorIndex)) {
                    continue;
                }
            }

            if (isExact) {
                if (!foundExact || scoreRule(r) > scoreRule(exactRule)) {
                    exactRule  = r;
                    foundExact = true;
                }
            } else {
                if (!foundFallback || scoreRule(r) > scoreRule(fallbackRule)) {
                    fallbackRule  = r;
                    foundFallback = true;
                }
            }
        }
        ReleaseSRWLockShared(&ruleLock_);

        if (!foundExact && !foundFallback) {
            return false;
        }
        outWinner = foundExact ? exactRule : fallbackRule;
        return true;
    }

    [[nodiscard]] static bool MatchesUnlockPolicy(uint8_t policyRaw, bool unlocked) noexcept {
        const auto policy = static_cast<mask::core::RemapUnlockPolicy>(policyRaw);
        switch (policy) {
        case mask::core::RemapUnlockPolicy::Always:       return true;
        case mask::core::RemapUnlockPolicy::WhenUnlocked: return unlocked;
        case mask::core::RemapUnlockPolicy::WhenLocked:   return !unlocked;
        default:                                          return true;
        }
    }

    [[nodiscard]] static bool SameDevice(const ActiveRemapHold& h, const uint8_t deviceId[32]) noexcept {
        if (!h.hasDeviceId || deviceId == nullptr) {
            return true;
        }
        return std::memcmp(h.deviceId, deviceId, 32U) == 0;
    }

    [[nodiscard]] bool IsPointInMonitor(const POINT& pt, int8_t monitorIndex) const noexcept {
        if (monitorIndex < 0) {
            return true;
        }
        AcquireSRWLockShared(&monitorLock_);
        bool inside = false;
        if (monitorIndex < monitorCount_) {
            const auto& m = monitors_[static_cast<size_t>(monitorIndex)];
            inside = (pt.x >= m.left && pt.x < m.right && pt.y >= m.top && pt.y < m.bottom);
        }
        ReleaseSRWLockShared(&monitorLock_);
        return inside;
    }

    [[nodiscard]] bool FindExistingHold(
        const uint8_t deviceId[32],
        uint16_t sourceVk,
        ActiveHoldInfo& outInfo
    ) const noexcept {
        AcquireSRWLockShared(&holdLock_);
        bool found = false;
        for (const auto& h : activeHolds_) {
            if (h.active && h.sourceVk == sourceVk && SameDevice(h, deviceId)) {
                outInfo.wasSuppressed = h.wasSuppressed;
                found = true;
                break;
            }
        }
        ReleaseSRWLockShared(&holdLock_);
        return found;
    }

    void RecordActiveHold(
        const uint8_t deviceId[32],
        uint16_t sourceVk,
        uint16_t emittedTargetVk,
        uint8_t actionKind,
        uint8_t prevLayer,
        bool wasSuppressed
    ) noexcept {
        AcquireSRWLockExclusive(&holdLock_);
        for (auto& h : activeHolds_) {
            if (!h.active) {
                h.active          = true;
                h.hasDeviceId     = (deviceId != nullptr);
                h.wasSuppressed   = wasSuppressed;
                if (deviceId != nullptr) {
                    std::memcpy(h.deviceId, deviceId, 32U);
                } else {
                    std::memset(h.deviceId, 0, 32U);
                }
                h.sourceVk        = sourceVk;
                h.emittedTargetVk = emittedTargetVk;
                h.actionKind      = actionKind;
                h.prevLayer       = prevLayer;
                break;
            }
        }
        ReleaseSRWLockExclusive(&holdLock_);
    }

    bool HandleKeyRelease(
        const uint8_t deviceId[32],
        uint16_t sourceVk,
        uint64_t nowTsc,
        ActiveHoldInfo* outReleasedInfo = nullptr
    ) noexcept {
        uint16_t releaseTargetVk = 0U;
        bool restoreLayer = false;
        uint8_t prevLayer = 0U;
        bool foundHold = false;
        bool wasSuppressed = false;

        AcquireSRWLockExclusive(&holdLock_);
        size_t matchIdx = activeHolds_.size();
        for (size_t i = 0; i < activeHolds_.size(); ++i) {
            const auto& h = activeHolds_[i];
            if (h.active && h.sourceVk == sourceVk && SameDevice(h, deviceId)) {
                matchIdx = i;
                break;
            }
        }
        if (matchIdx == activeHolds_.size()) {
            // Fallback: release any active hold matching sourceVk regardless of device correlation drift
            for (size_t i = 0; i < activeHolds_.size(); ++i) {
                const auto& h = activeHolds_[i];
                if (h.active && h.sourceVk == sourceVk) {
                    matchIdx = i;
                    break;
                }
            }
        }

        if (matchIdx < activeHolds_.size()) {
            auto& h = activeHolds_[matchIdx];
            foundHold     = true;
            wasSuppressed = h.wasSuppressed;
            const uint8_t kind       = h.actionKind;
            const uint16_t targetVk  = h.emittedTargetVk;
            const uint8_t savedLayer = h.prevLayer;
            h = ActiveRemapHold{};

            if (kind == static_cast<uint8_t>(mask::core::RemapLayerAction::RemapKey) && targetVk != 0U) {
                bool stillHeldElsewhere = false;
                for (const auto& other : activeHolds_) {
                    if (other.active &&
                        other.actionKind == static_cast<uint8_t>(mask::core::RemapLayerAction::RemapKey) &&
                        other.emittedTargetVk == targetVk) {
                        stillHeldElsewhere = true;
                        break;
                    }
                }
                if (!stillHeldElsewhere) {
                    releaseTargetVk = targetVk;
                }
            } else if (kind == static_cast<uint8_t>(mask::core::RemapLayerAction::ShiftLayerHold)) {
                restoreLayer = true;
                prevLayer = savedLayer;
            }
        }
        ReleaseSRWLockExclusive(&holdLock_);

        if (outReleasedInfo != nullptr) {
            outReleasedInfo->wasSuppressed = wasSuppressed;
        }

        if (releaseTargetVk != 0U) {
            mask::core::AuxOutputCommand cmd{};
            cmd.timestampTsc = nowTsc;
            cmd.vk           = releaseTargetVk;
            cmd.kind         = static_cast<uint8_t>(mask::core::AuxActionKind::KeyUp);
            if (sharedState_.auxOutputQueue.try_push(cmd)) {
                mask::input::WakeEngine();
            }
        }

        if (restoreLayer) {
            sharedState_.activeRemapLayer.store(prevLayer, std::memory_order_release);
            RecomputeSuppressedMask();
        }

        return foundHold;
    }

    void ExecuteRuleOnDown(
        const mask::core::RemapRuleConfig& rule,
        const uint8_t deviceId[32],
        uint16_t sourceVk,
        uint8_t currentLayer,
        bool currentlyUnlocked,
        uint64_t nowTsc,
        bool wasSuppressed
    ) noexcept {
        const auto action = static_cast<mask::core::RemapLayerAction>(rule.actionKind);
        switch (action) {
        case mask::core::RemapLayerAction::RemapKey: {
            const uint16_t targetVk = static_cast<uint16_t>(rule.targetVk & 0xFFU);
            if (targetVk != 0U) {
                RecordActiveHold(deviceId, sourceVk, targetVk, rule.actionKind, currentLayer, wasSuppressed);
                mask::core::AuxOutputCommand cmd{};
                cmd.timestampTsc = nowTsc;
                cmd.vk           = targetVk;
                cmd.kind         = static_cast<uint8_t>(mask::core::AuxActionKind::KeyDown);
                if (sharedState_.auxOutputQueue.try_push(cmd)) {
                    mask::input::WakeEngine();
                }
            }
            break;
        }
        case mask::core::RemapLayerAction::BlockKey: {
            RecordActiveHold(deviceId, sourceVk, 0U, rule.actionKind, currentLayer, true);
            break;
        }
        case mask::core::RemapLayerAction::ShiftLayerHold: {
            const uint8_t nextLayer = static_cast<uint8_t>(rule.targetLayer % mask::core::kMaxRemapLayers);
            RecordActiveHold(deviceId, sourceVk, 0U, rule.actionKind, currentLayer, wasSuppressed);
            sharedState_.activeRemapLayer.store(nextLayer, std::memory_order_release);
            RecomputeSuppressedMask();
            break;
        }
        case mask::core::RemapLayerAction::ToggleLayer: {
            const uint8_t targetLayer = static_cast<uint8_t>(rule.targetLayer % mask::core::kMaxRemapLayers);
            const uint8_t nextLayer = (currentLayer == targetLayer) ? 0U : targetLayer;
            RecordActiveHold(deviceId, sourceVk, 0U, rule.actionKind, currentLayer, wasSuppressed);
            sharedState_.activeRemapLayer.store(nextLayer, std::memory_order_release);
            RecomputeSuppressedMask();
            break;
        }
        case mask::core::RemapLayerAction::ToggleUnlockState: {
            RecordActiveHold(deviceId, sourceVk, 0U, rule.actionKind, currentLayer, wasSuppressed);
            sharedState_.remapUnlocked.store(!currentlyUnlocked, std::memory_order_release);
            RecomputeSuppressedMask();
            break;
        }
        case mask::core::RemapLayerAction::WarpToMonitor: {
            RecordActiveHold(deviceId, sourceVk, 0U, rule.actionKind, currentLayer, wasSuppressed);
            int32_t cx = 0;
            int32_t cy = 0;
            bool valid = false;
            AcquireSRWLockShared(&monitorLock_);
            if (monitorCount_ > 0) {
                int idx = (rule.monitorIndex >= 0) ? static_cast<int>(rule.monitorIndex) : 0;
                if (idx >= monitorCount_) {
                    idx = monitorCount_ - 1;
                }
                const auto& m = monitors_[static_cast<size_t>(idx)];
                cx = m.left + ((m.right - m.left) / 2);
                cy = m.top + ((m.bottom - m.top) / 2);
                valid = true;
            }
            ReleaseSRWLockShared(&monitorLock_);

            if (valid) {
                mask::core::AuxOutputCommand cmd{};
                cmd.timestampTsc = nowTsc;
                cmd.argX         = cx;
                cmd.argY         = cy;
                cmd.kind         = static_cast<uint8_t>(mask::core::AuxActionKind::MouseMoveAbs);
                if (sharedState_.auxOutputQueue.try_push(cmd)) {
                    mask::input::WakeEngine();
                }
            }
            break;
        }
        default:
            break;
        }
    }

    mask::core::EngineSharedState& sharedState_;
    std::atomic<bool>              started_{false};
    std::atomic<uint64_t>          echoWindowTsc_{150'000'000ULL};

    mutable SRWLOCK ruleLock_{};
    std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules> rules_{};

    mutable SRWLOCK monitorLock_{};
    std::array<MonitorRectInfo, kMaxMonitors> monitors_{};
    int monitorCount_{0};

    mutable SRWLOCK holdLock_{};
    std::array<ActiveRemapHold, kMaxActiveHolds> activeHolds_{};

    mutable SRWLOCK rawLock_{};
    std::array<RawHandleEntry, kMaxRawHandles>                     rawHandleMap_{};
    size_t                                                         rawHandleCount_{0};
    std::array<mask::input::PhysicalDeviceInfo, kMaxRawInventory>  rawDevices_{};
    int                                                            rawDeviceCount_{0};

    mutable SRWLOCK corrLock_{};
    RawTransitionRecord                 mostRecentKbRaw_{};
    RawTransitionRecord                 mostRecentMouseRaw_{};
    std::array<RawTransitionRecord, 256> perVkRaw_{};
    GiTransitionRecord                  mostRecentGiKb_{};
    GiTransitionRecord                  mostRecentGiMouse_{};
    std::array<GiTransitionRecord, 256> perVkGi_{};

    mutable SRWLOCK    hookLock_{};
    HANDLE             hookThreadHandle_{nullptr};
    HANDLE             hookReadyEvent_{nullptr};
    std::atomic<DWORD> hookThreadId_{0};
    std::atomic<bool>  desiredActive_{false};
    std::atomic<bool>  wantKeyboard_{false};
    std::atomic<bool>  wantMouse_{false};
    std::atomic<bool>  kbHookActive_{false};
    std::atomic<bool>  mouseHookActive_{false};
    std::atomic<bool>  rawRefreshQueued_{false};
};

} // namespace mask::remap
