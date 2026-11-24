#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "complex_sequence_engine.hpp"
#include "input_pipeline.hpp"
#include "timing_engine.hpp"

#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <intrin.h>

#ifdef _MSC_VER
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
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
#include <limits>
#include <memory>
#include <new>
#include <type_traits>

namespace mask::workflows {

namespace {

inline constexpr uint8_t kApprovalDecisionNone  = 0U;
inline constexpr uint8_t kApprovalDecisionDeny  = 1U;
inline constexpr uint8_t kApprovalDecisionAllow = 2U;

[[nodiscard]] bool SanitizeBool(const bool& value) noexcept {
    uint8_t raw = 0;
    std::memcpy(&raw, &value, 1);
    return raw != 0U;
}

[[nodiscard]] uint64_t CountTrailingZeros64(uint64_t mask) noexcept {
    unsigned long idx = 0;
    _BitScanForward64(&idx, mask);
    return static_cast<uint64_t>(idx);
}

[[nodiscard]] uint64_t NextXorShift64Star(uint64_t& state) noexcept {
    uint64_t x = state;
    if (x == 0ULL) {
        x = 0x9E3779B97F4A7C15ULL;
    }
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

[[nodiscard]] uint32_t ApplyHumanizeMs(uint32_t baseMs, uint8_t humanizePct, uint64_t& rngState) noexcept {
    if (baseMs == 0U || humanizePct == 0U) {
        return baseMs;
    }
    const uint32_t pct        = (humanizePct > 50U) ? 50U : static_cast<uint32_t>(humanizePct);
    const uint64_t span       = (static_cast<uint64_t>(baseMs) * pct) / 100ULL;
    if (span == 0ULL) {
        return baseMs;
    }
    const uint64_t range      = span * 2ULL + 1ULL;
    const uint64_t offset     = NextXorShift64Star(rngState) % range;
    const int64_t signedDelta = static_cast<int64_t>(offset) - static_cast<int64_t>(span);
    const int64_t adjusted    = static_cast<int64_t>(baseMs) + signedDelta;
    if (adjusted < 1) {
        return 1U;
    }
    if (adjusted > static_cast<int64_t>((std::numeric_limits<uint32_t>::max)())) {
        return (std::numeric_limits<uint32_t>::max)();
    }
    return static_cast<uint32_t>(adjusted);
}

void SanitizeComplexStep(mask::core::ComplexStep& s) noexcept {
    if (s.kind > static_cast<uint8_t>(mask::core::ComplexStepKind::EndIf)) {
        s.kind = static_cast<uint8_t>(mask::core::ComplexStepKind::WaitMs);
    }
    if (s.kind == static_cast<uint8_t>(mask::core::ComplexStepKind::IfCheck)) {
        s.flags = static_cast<uint8_t>(
            s.flags & (mask::core::kComplexCondMask | mask::core::kComplexFlagNegate));
        if ((s.flags & mask::core::kComplexCondMask) >
            static_cast<uint8_t>(mask::core::ComplexConditionKind::RandomChance)) {
            s.flags = static_cast<uint8_t>(s.flags & ~mask::core::kComplexCondMask);
        }
    }
    s.vk = static_cast<uint16_t>(s.vk & 0xFFU);
    s.text[std::size(s.text) - 1] = L'\0';
}

void SanitizeComplexSequenceConfig(mask::core::ComplexSequenceConfig& c, size_t slotIdx) noexcept {
    c.slotIndex = static_cast<uint8_t>(slotIdx);
    c.enabled   = SanitizeBool(c.enabled);
    c.inUse     = SanitizeBool(c.inUse);
    if (c.triggerMode > 2U) {
        c.triggerMode = 0U;
    }
    c.triggerVk = static_cast<uint16_t>(c.triggerVk & 0xFFU);
    if (c.stepCount > static_cast<uint16_t>(mask::core::kMaxComplexSteps)) {
        c.stepCount = static_cast<uint16_t>(mask::core::kMaxComplexSteps);
    }
    if (c.humanizePct > 50U) {
        c.humanizePct = 50U;
    }
    c.reserved[0] = static_cast<uint8_t>(c.reserved[0] & mask::core::kSeqFlagUntrustedImport);
    c.reserved[1] = 0U;
    c.reserved[2] = 0U;
    c.name[std::size(c.name) - 1] = L'\0';
    if (c.name[0] == L'\0') {
        std::swprintf(c.name, std::size(c.name), L"Sequence %zu", slotIdx + 1U);
    }
    for (auto& step : c.steps) {
        SanitizeComplexStep(step);
    }
}

struct SequencesFilePayloadV1 {
    uint32_t magic{kComplexSequencesMagic};
    uint32_t version{kComplexSequencesVersionV1};
    uint32_t count{static_cast<uint32_t>(mask::core::kMaxComplexSequences)};
    uint32_t reserved{0};
    std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences> sequences{};
};

struct SequencesFilePayload {
    uint32_t magic{kComplexSequencesMagic};
    uint32_t version{kComplexSequencesVersion};
    uint32_t count{static_cast<uint32_t>(mask::core::kMaxComplexSequences)};
    uint32_t reserved{0};
    std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences> sequences{};
};

static_assert(std::is_trivially_copyable_v<SequencesFilePayloadV1>,
              "SequencesFilePayloadV1 must be trivially copyable.");
static_assert(std::is_trivially_copyable_v<SequencesFilePayload>,
              "SequencesFilePayload must be trivially copyable.");
static_assert(sizeof(SequencesFilePayloadV1) == sizeof(SequencesFilePayload),
              "SequencesFilePayloadV1 layout must match SequencesFilePayload.");

[[nodiscard]] bool WriteFileAtomic(const wchar_t* path, const void* data, size_t size) noexcept {
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

[[nodiscard]] bool ReadSequencesFileRaw(const wchar_t* path, SequencesFilePayload& out) noexcept {
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
    const bool sizeOk = GetFileSizeEx(h, &fileSize) != FALSE &&
                        fileSize.QuadPart == static_cast<LONGLONG>(sizeof(SequencesFilePayload));

    bool ok = false;
    if (sizeOk) {
        DWORD bytesRead = 0;
        if (ReadFile(h, &out, static_cast<DWORD>(sizeof(SequencesFilePayload)), &bytesRead, nullptr) != FALSE &&
            bytesRead == static_cast<DWORD>(sizeof(SequencesFilePayload)) &&
            out.magic == kComplexSequencesMagic &&
            out.count == static_cast<uint32_t>(mask::core::kMaxComplexSequences)) {
            if (out.version == kComplexSequencesVersion) {
                ok = true;
            } else if (out.version == kComplexSequencesVersionV1) {
                out.version  = kComplexSequencesVersion;
                out.reserved = 0U;
                ok           = true;
            }
        }
    }

    CloseHandle(h);
    return ok;
}

void CleanupTempFileTriplet(const wchar_t* path) noexcept {
    if (path == nullptr || path[0] == L'\0') {
        return;
    }
    DeleteFileW(path);
    wchar_t aux[1040]{};
    if (std::swprintf(aux, std::size(aux), L"%ls.bak", path) >= 0) {
        DeleteFileW(aux);
    }
    if (std::swprintf(aux, std::size(aux), L"%ls.tmp", path) >= 0) {
        DeleteFileW(aux);
    }
}

[[nodiscard]] bool BuildUniqueTempFilePath(const wchar_t* tag, wchar_t* outPath, size_t outCap) noexcept {
    static std::atomic<uint32_t> s_seq{0};
    wchar_t tempDir[MAX_PATH]{};
    const DWORD len = GetTempPathW(static_cast<DWORD>(std::size(tempDir)), tempDir);
    if (len == 0U || len >= static_cast<DWORD>(std::size(tempDir))) {
        return false;
    }
    const uint32_t seq = s_seq.fetch_add(1U, std::memory_order_relaxed);
    return std::swprintf(
        outPath,
        outCap,
        L"%ls%ls_%lu_%u.bin",
        tempDir,
        tag,
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned>(seq)) > 0;
}

struct AsyncLaunchSlot {
    std::atomic<bool> inUse{false};
    bool              isCommand{false};
    wchar_t           text[128]{};
};

[[nodiscard]] std::array<AsyncLaunchSlot, 16>& GetAsyncLaunchPool() noexcept {
    static std::array<AsyncLaunchSlot, 16> pool{};
    return pool;
}

DWORD WINAPI AsyncLaunchWorker(LPVOID param) noexcept {
    auto* slot = static_cast<AsyncLaunchSlot*>(param);
    if (slot == nullptr) {
        return 0;
    }

    wchar_t localText[128]{};
    std::memcpy(localText, slot->text, sizeof(localText));
    localText[std::size(localText) - 1] = L'\0';
    const bool isCmd = slot->isCommand;
    slot->inUse.store(false, std::memory_order_release);

    if (localText[0] == L'\0') {
        return 0;
    }

    if (isCmd) {
        wchar_t cmdLine[160]{};
        if (std::swprintf(cmdLine, std::size(cmdLine), L"cmd.exe /C %ls", localText) > 0) {
            STARTUPINFOW si{};
            si.cb          = sizeof(si);
            si.dwFlags     = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(
                    nullptr,
                    cmdLine,
                    nullptr,
                    nullptr,
                    FALSE,
                    CREATE_NO_WINDOW,
                    nullptr,
                    nullptr,
                    &si,
                    &pi) != FALSE) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
    } else {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteW(nullptr, L"open", localText, nullptr, nullptr, SW_SHOWNORMAL);
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }
    return 0;
}

} // namespace

ComplexSequenceEngine::ComplexSequenceEngine(
    mask::core::EngineSharedState& sharedState,
    PixelSampleFn pixelSampler
) noexcept
    : sharedState_(sharedState)
    , pixelSampler_(pixelSampler)
    , configs_(new (std::nothrow) ConfigArray{})
    , workerConfigs_(new (std::nothrow) ConfigArray{}) {
    InitializeSRWLock(&configLock_);

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    qpcFreqHz_ = (freq.QuadPart > 0) ? static_cast<uint64_t>(freq.QuadPart) : 10'000'000ULL;

    for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
        if (configs_) {
            (*configs_)[i].slotIndex = static_cast<uint8_t>(i);
        }
        if (workerConfigs_) {
            (*workerConfigs_)[i].slotIndex = static_cast<uint8_t>(i);
        }
        runtime_[i].rngState = 0xD1B54A32D192ED03ULL ^ (static_cast<uint64_t>(i + 1U) * 0x9E3779B97F4A7C15ULL);
        cachedTriggerVk_[i].store(0U, std::memory_order_relaxed);
        cachedTriggerMeta_[i].store(0U, std::memory_order_relaxed);
        heldTriggerState_[i].store(0U, std::memory_order_relaxed);
        toggleEdgeCounts_[i].store(0U, std::memory_order_relaxed);
        currentStepIndices_[i].store(0U, std::memory_order_relaxed);
        pendingApprovalDecision_[i].store(kApprovalDecisionNone, std::memory_order_relaxed);
        waitingApprovalTelemetry_[i].store(false, std::memory_order_relaxed);
    }

    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

ComplexSequenceEngine::~ComplexSequenceEngine() noexcept {
    Stop();
    if (wakeEvent_ != nullptr) {
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
    }
}

void ComplexSequenceEngine::Start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    stopRequested_.store(false, std::memory_order_release);
    if (wakeEvent_ == nullptr) {
        wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    workerThread_ = CreateThread(nullptr, 0, &ComplexSequenceEngine::ThreadEntry, this, 0, nullptr);
    if (workerThread_ == nullptr) {
        running_.store(false, std::memory_order_release);
    }
}

void ComplexSequenceEngine::Stop() noexcept {
    stopRequested_.store(true, std::memory_order_release);
    abortRequested_.store(true, std::memory_order_release);
    SignalWake();

    if (workerThread_ != nullptr) {
        WaitForSingleObject(workerThread_, 3000U);
        CloseHandle(workerThread_);
        workerThread_ = nullptr;
    }
    running_.store(false, std::memory_order_release);
    activeSequenceBitmask_.store(0U, std::memory_order_release);
    for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
        currentStepIndices_[i].store(0U, std::memory_order_release);
        waitingApprovalTelemetry_[i].store(false, std::memory_order_release);
        pendingApprovalDecision_[i].store(kApprovalDecisionNone, std::memory_order_release);
    }
}

void ComplexSequenceEngine::SetPixelSampler(PixelSampleFn pixelSampler) noexcept {
    pixelSampler_.store(pixelSampler, std::memory_order_release);
}

void ComplexSequenceEngine::SetToggleMacroCallback(ToggleMacroFn toggleMacro) noexcept {
    toggleMacroFn_.store(toggleMacro, std::memory_order_release);
}

void ComplexSequenceEngine::SetCommandApprovalCallback(CommandApprovalFn cb) noexcept {
    commandApprovalCb_.store(cb, std::memory_order_release);
}

void ComplexSequenceEngine::ResolveCommandApproval(uint8_t sequenceIndex, bool allowAndTrust) noexcept {
    if (sequenceIndex >= mask::core::kMaxComplexSequences) {
        return;
    }
    if (allowAndTrust && configs_) {
        AcquireSRWLockExclusive(&configLock_);
        mask::core::SetSequenceUntrustedImport((*configs_)[sequenceIndex], false);
        ReleaseSRWLockExclusive(&configLock_);
    }
    pendingApprovalDecision_[sequenceIndex].store(
        allowAndTrust ? kApprovalDecisionAllow : kApprovalDecisionDeny,
        std::memory_order_release);
    SignalWake();
}

void ComplexSequenceEngine::UpdateConfig(size_t index, const mask::core::ComplexSequenceConfig& cfg) noexcept {
    if (index >= mask::core::kMaxComplexSequences || !configs_) {
        return;
    }
    AcquireSRWLockExclusive(&configLock_);
    (*configs_)[index] = cfg;
    SanitizeComplexSequenceConfig((*configs_)[index], index);
    PublishTriggerCache(index, (*configs_)[index]);
    ReleaseSRWLockExclusive(&configLock_);

    configDirtyMask_.fetch_or(static_cast<uint16_t>(1U << index), std::memory_order_release);
    SignalWake();
}

void ComplexSequenceEngine::UpdateAllConfigs(
    const std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>& cfgs
) noexcept {
    if (!configs_) {
        return;
    }
    AcquireSRWLockExclusive(&configLock_);
    for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
        (*configs_)[i] = cfgs[i];
        SanitizeComplexSequenceConfig((*configs_)[i], i);
        PublishTriggerCache(i, (*configs_)[i]);
    }
    ReleaseSRWLockExclusive(&configLock_);

    configDirtyMask_.store(0xFFFFU, std::memory_order_release);
    SignalWake();
}

void ComplexSequenceEngine::NotifyPhysicalKeyTransition(uint16_t vk, bool isDown) noexcept {
    const uint16_t boundedVk = static_cast<uint16_t>(vk & 0xFFU);
    if (boundedVk == 0U) {
        return;
    }

    const bool suppressNewStarts =
        sharedState_.automationPaused.load(std::memory_order_acquire) ||
        sharedState_.suppressTriggers.load(std::memory_order_relaxed) ||
        sharedState_.captureBindTarget.load(std::memory_order_relaxed) != 0U;

    bool matched = false;
    for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
        const uint8_t meta = cachedTriggerMeta_[i].load(std::memory_order_acquire);
        if ((meta & 1U) == 0U) {
            continue;
        }
        if (cachedTriggerVk_[i].load(std::memory_order_relaxed) != boundedVk) {
            continue;
        }

        const uint8_t mode = static_cast<uint8_t>((meta >> 1) & 0x03U);
        if (mode == 2U) {
            if (!isDown) {
                if (heldTriggerState_[i].exchange(0U, std::memory_order_acq_rel) != 0U) {
                    matched = true;
                }
            } else if (!suppressNewStarts) {
                if (heldTriggerState_[i].exchange(1U, std::memory_order_acq_rel) == 0U) {
                    matched = true;
                }
            }
        } else if (isDown && !suppressNewStarts) {
            if (mode == 0U) {
                pendingStartMask_.fetch_or(static_cast<uint16_t>(1U << i), std::memory_order_release);
                matched = true;
            } else if (mode == 1U) {
                toggleEdgeCounts_[i].fetch_add(1U, std::memory_order_release);
                matched = true;
            }
        }
    }

    if (matched) {
        SignalWake();
    }
}

void ComplexSequenceEngine::TriggerSequence(size_t index) noexcept {
    if (index >= mask::core::kMaxComplexSequences) {
        return;
    }
    if (sharedState_.automationPaused.load(std::memory_order_acquire)) {
        return;
    }
    pendingManualTriggerMask_.fetch_or(static_cast<uint16_t>(1U << index), std::memory_order_release);
    SignalWake();
}

void ComplexSequenceEngine::AbortAll() noexcept {
    pendingStartMask_.store(0U, std::memory_order_release);
    pendingManualTriggerMask_.store(0U, std::memory_order_release);
    for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
        heldTriggerState_[i].store(0U, std::memory_order_release);
    }
    abortRequested_.store(true, std::memory_order_release);
    SignalWake();
}

uint16_t ComplexSequenceEngine::GetActiveSequenceBitmask() const noexcept {
    return activeSequenceBitmask_.load(std::memory_order_acquire);
}

uint16_t ComplexSequenceEngine::GetCurrentStepIndex(size_t index) const noexcept {
    if (index >= mask::core::kMaxComplexSequences) {
        return 0U;
    }
    return currentStepIndices_[index].load(std::memory_order_acquire);
}

bool ComplexSequenceEngine::IsSequenceWaitingForApproval(size_t index) const noexcept {
    if (index >= mask::core::kMaxComplexSequences) {
        return false;
    }
    return waitingApprovalTelemetry_[index].load(std::memory_order_acquire);
}

bool ComplexSequenceEngine::RunControlFlowSelfTest() noexcept {
    std::unique_ptr<mask::core::EngineSharedState> state(new (std::nothrow) mask::core::EngineSharedState{});
    if (!state) {
        return false;
    }

    static int s_mockColorCalls = 0;
    const PixelSampleFn mockSampler = [](int32_t, int32_t, uint8_t& r, uint8_t& g, uint8_t& b) noexcept -> bool {
        ++s_mockColorCalls;
        if (s_mockColorCalls == 1 || s_mockColorCalls == 3) {
            r = 10U; g = 200U; b = 30U;
        } else {
            r = 240U; g = 15U; b = 15U;
        }
        return true;
    };

    ComplexSequenceEngine engine(*state, mockSampler);
    if (!engine.workerConfigs_ || !engine.configs_) {
        return false;
    }
    engine.dryRunLaunchesForTest_ = true;

    auto drainKeyClicks = [&](uint16_t* outVks, size_t maxOut, size_t& outCount) noexcept -> bool {
        outCount = 0U;
        mask::core::AuxOutputCommand cmd{};
        while (state->auxOutputQueue.try_pop(cmd)) {
            if (cmd.kind == static_cast<uint8_t>(mask::core::AuxActionKind::KeyClick)) {
                if (outCount >= maxOut) {
                    return false;
                }
                outVks[outCount++] = cmd.vk;
            }
        }
        return true;
    };

    auto runCase = [&](const mask::core::ComplexSequenceConfig& cfg,
                       const uint16_t* expectedVks,
                       size_t expectedCount) noexcept -> bool {
        mask::core::AuxOutputCommand trash{};
        while (state->auxOutputQueue.try_pop(trash)) {}

        (*engine.configs_)[0] = cfg;
        SanitizeComplexSequenceConfig((*engine.configs_)[0], 0);
        (*engine.workerConfigs_)[0] = (*engine.configs_)[0];
        engine.StartSequenceSlot(0, false);

        for (int guard = 0; guard < 16 && engine.runtime_[0].active; ++guard) {
            (void)engine.AdvanceSequence(0, NowQpc());
        }
        if (engine.runtime_[0].active) {
            return false;
        }

        std::array<uint16_t, 32> actual{};
        size_t actualCount = 0U;
        if (!drainKeyClicks(actual.data(), actual.size(), actualCount)) {
            return false;
        }
        if (actualCount != expectedCount) {
            return false;
        }
        for (size_t i = 0; i < expectedCount; ++i) {
            if (actual[i] != expectedVks[i]) {
                return false;
            }
        }
        return true;
    };

    auto makeKeyClick = [](uint16_t vk) noexcept {
        mask::core::ComplexStep st{};
        st.kind       = static_cast<uint8_t>(mask::core::ComplexStepKind::KeyClick);
        st.vk         = vk;
        st.durationMs = 0U;
        return st;
    };
    auto makeCond = [](mask::core::ComplexConditionKind cond, bool neg) noexcept {
        mask::core::ComplexStep st{};
        st.kind = static_cast<uint8_t>(mask::core::ComplexStepKind::IfCheck);
        mask::core::SetStepConditionKind(st, cond);
        mask::core::SetStepConditionNegated(st, neg);
        return st;
    };

    // Case 1: If (LayerIs 1) -> Click 'A' -> Else -> Click 'B' -> EndIf
    {
        mask::core::ComplexSequenceConfig cfg{};
        cfg.enabled   = true;
        cfg.inUse     = true;
        cfg.stepCount = 5;
        cfg.steps[0]  = makeCond(mask::core::ComplexConditionKind::LayerIs, false);
        cfg.steps[0].param = 1U;
        cfg.steps[1]  = makeKeyClick('A');
        cfg.steps[2].kind = static_cast<uint8_t>(mask::core::ComplexStepKind::ElseBranch);
        cfg.steps[3]  = makeKeyClick('B');
        cfg.steps[4].kind = static_cast<uint8_t>(mask::core::ComplexStepKind::EndIf);

        state->activeRemapLayer.store(1U, std::memory_order_release);
        const uint16_t exp1[1] = { 'A' };
        if (!runCase(cfg, exp1, 1U)) return false;

        state->activeRemapLayer.store(0U, std::memory_order_release);
        const uint16_t exp0[1] = { 'B' };
        if (!runCase(cfg, exp0, 1U)) return false;
    }

    // Case 2: Negated If (NOT KeyHeld 'K') -> Click 'C' -> EndIf
    {
        mask::core::ComplexSequenceConfig cfg{};
        cfg.enabled   = true;
        cfg.inUse     = true;
        cfg.stepCount = 3;
        cfg.steps[0]  = makeCond(mask::core::ComplexConditionKind::KeyHeld, true);
        cfg.steps[0].vk = 'K';
        cfg.steps[1]  = makeKeyClick('C');
        cfg.steps[2].kind = static_cast<uint8_t>(mask::core::ComplexStepKind::EndIf);

        state->physicalKeyState['K'].store(0U, std::memory_order_release);
        state->syntheticHeldState['K'].store(0U, std::memory_order_release);
        const uint16_t expOpen[1] = { 'C' };
        if (!runCase(cfg, expOpen, 1U)) return false;

        state->physicalKeyState['K'].store(1U, std::memory_order_release);
        const bool okHeld = runCase(cfg, nullptr, 0U);
        state->physicalKeyState['K'].store(0U, std::memory_order_release);
        if (!okHeld) return false;
    }

    // Case 3: LoopStart(3) -> If (ColorMatches) -> Click 'D' -> Else -> Click 'E' -> EndIf -> LoopEnd
    {
        s_mockColorCalls = 0;
        mask::core::ComplexSequenceConfig cfg{};
        cfg.enabled   = true;
        cfg.inUse     = true;
        cfg.stepCount = 7;
        cfg.steps[0].kind  = static_cast<uint8_t>(mask::core::ComplexStepKind::LoopStart);
        cfg.steps[0].param = 3U;
        cfg.steps[1]       = makeCond(mask::core::ComplexConditionKind::ColorMatches, false);
        cfg.steps[1].r     = 10U;
        cfg.steps[1].g     = 200U;
        cfg.steps[1].b     = 30U;
        cfg.steps[1].tolerance = 10U;
        cfg.steps[2]       = makeKeyClick('D');
        cfg.steps[3].kind  = static_cast<uint8_t>(mask::core::ComplexStepKind::ElseBranch);
        cfg.steps[4]       = makeKeyClick('E');
        cfg.steps[5].kind  = static_cast<uint8_t>(mask::core::ComplexStepKind::EndIf);
        cfg.steps[6].kind  = static_cast<uint8_t>(mask::core::ComplexStepKind::LoopEnd);

        const uint16_t expLoop[3] = { 'D', 'E', 'D' };
        if (!runCase(cfg, expLoop, 3U)) return false;
    }

    // Case 4: Nested If (UnlockStateIs Locked -> false) -> If (LayerIs 0 -> true) -> Click 'X' -> Else -> Click 'Y' -> EndIf -> Else -> Click 'Z' -> EndIf
    {
        state->remapUnlocked.store(true, std::memory_order_release);
        state->activeRemapLayer.store(0U, std::memory_order_release);
        mask::core::ComplexSequenceConfig cfg{};
        cfg.enabled   = true;
        cfg.inUse     = true;
        cfg.stepCount = 9;
        cfg.steps[0]  = makeCond(mask::core::ComplexConditionKind::UnlockStateIs, false);
        cfg.steps[0].param = 1U;
        cfg.steps[1]  = makeCond(mask::core::ComplexConditionKind::LayerIs, false);
        cfg.steps[1].param = 0U;
        cfg.steps[2]  = makeKeyClick('X');
        cfg.steps[3].kind = static_cast<uint8_t>(mask::core::ComplexStepKind::ElseBranch);
        cfg.steps[4]  = makeKeyClick('Y');
        cfg.steps[5].kind = static_cast<uint8_t>(mask::core::ComplexStepKind::EndIf);
        cfg.steps[6].kind = static_cast<uint8_t>(mask::core::ComplexStepKind::ElseBranch);
        cfg.steps[7]  = makeKeyClick('Z');
        cfg.steps[8].kind = static_cast<uint8_t>(mask::core::ComplexStepKind::EndIf);

        const uint16_t expNested[1] = { 'Z' };
        if (!runCase(cfg, expNested, 1U)) return false;
    }

    // Case 5: Unmatched If (LayerIs 2 -> false) -> Click 'X' (missing EndIf)
    {
        state->activeRemapLayer.store(0U, std::memory_order_release);
        mask::core::ComplexSequenceConfig cfg{};
        cfg.enabled   = true;
        cfg.inUse     = true;
        cfg.stepCount = 2;
        cfg.steps[0]  = makeCond(mask::core::ComplexConditionKind::LayerIs, false);
        cfg.steps[0].param = 2U;
        cfg.steps[1]  = makeKeyClick('X');

        if (!runCase(cfg, nullptr, 0U)) return false;
    }

    // Case 6: Untrusted imported sequence approval gate (block, deny, allow & trust, trusted pass-through)
    {
        static uint32_t s_promptCount = 0U;
        static uint8_t  s_lastSeqIdx  = 0xFFU;
        static uint16_t s_lastStepIdx = 0xFFFFU;
        static bool     s_lastIsCmd   = false;
        static wchar_t  s_lastCmdText[128]{};

        s_promptCount = 0U;
        s_lastSeqIdx  = 0xFFU;
        s_lastStepIdx = 0xFFFFU;
        s_lastIsCmd   = false;
        s_lastCmdText[0] = L'\0';

        const CommandApprovalFn testCb = [](
            uint8_t sequenceIndex,
            uint16_t stepIndex,
            bool isShellCommand,
            const wchar_t* commandText
        ) noexcept {
            ++s_promptCount;
            s_lastSeqIdx  = sequenceIndex;
            s_lastStepIdx = stepIndex;
            s_lastIsCmd   = isShellCommand;
            if (commandText != nullptr) {
                wcsncpy_s(s_lastCmdText, commandText, _TRUNCATE);
            } else {
                s_lastCmdText[0] = L'\0';
            }
        };
        engine.SetCommandApprovalCallback(testCb);

        // 6a. Imported RunCommand: blocks cleanly, denies cleanly (does not execute, finishes cleanly)
        mask::core::ComplexSequenceConfig cfg{};
        cfg.enabled   = true;
        cfg.inUse     = true;
        cfg.stepCount = 2U;
        cfg.steps[0].kind = static_cast<uint8_t>(mask::core::ComplexStepKind::RunCommand);
        wcscpy_s(cfg.steps[0].text, L"echo mask_blocked");
        cfg.steps[1] = makeKeyClick('T');
        mask::core::SetSequenceUntrustedImport(cfg, true);

        mask::core::AuxOutputCommand trash{};
        while (state->auxOutputQueue.try_pop(trash)) {}

        (*engine.configs_)[0] = cfg;
        SanitizeComplexSequenceConfig((*engine.configs_)[0], 0U);
        (*engine.workerConfigs_)[0] = (*engine.configs_)[0];
        if (!mask::core::IsSequenceUntrustedImport((*engine.workerConfigs_)[0])) {
            return false;
        }

        engine.StartSequenceSlot(0, false);
        const uint64_t wait1 = engine.AdvanceSequence(0, NowQpc());
        if (wait1 != 0ULL ||
            !engine.runtime_[0].active ||
            !engine.runtime_[0].waitingForCommandApproval ||
            !engine.runtime_[0].approvalPromptSent ||
            engine.runtime_[0].approvalPendingStep != 0U ||
            !engine.IsSequenceWaitingForApproval(0) ||
            s_promptCount != 1U ||
            s_lastSeqIdx != 0U ||
            s_lastStepIdx != 0U ||
            !s_lastIsCmd ||
            std::wcscmp(s_lastCmdText, L"echo mask_blocked") != 0 ||
            engine.testLaunchCount_ != 0U) {
            return false;
        }

        // Calling AdvanceSequence while waiting must not duplicate callback or run command
        if (engine.AdvanceSequence(0, NowQpc()) != 0ULL ||
            s_promptCount != 1U ||
            engine.testLaunchCount_ != 0U) {
            return false;
        }

        // Deny: clears wait, skips RunCommand without executing, executes trailing step, finishes
        engine.ResolveCommandApproval(0, false);
        for (int guard = 0; guard < 8 && engine.runtime_[0].active; ++guard) {
            (void)engine.AdvanceSequence(0, NowQpc());
        }
        if (engine.runtime_[0].active ||
            engine.runtime_[0].waitingForCommandApproval ||
            engine.IsSequenceWaitingForApproval(0) ||
            engine.testLaunchCount_ != 0U ||
            !mask::core::IsSequenceUntrustedImport((*engine.configs_)[0])) {
            return false;
        }

        std::array<uint16_t, 8> actual{};
        size_t actualCount = 0U;
        if (!drainKeyClicks(actual.data(), actual.size(), actualCount) ||
            actualCount != 1U || actual[0] != 'T') {
            return false;
        }

        // 6b. Imported LaunchProgram: blocks, Allow & Trust clears flag in configs_ & workerConfigs_ & executes
        cfg = mask::core::ComplexSequenceConfig{};
        cfg.enabled   = true;
        cfg.inUse     = true;
        cfg.stepCount = 2U;
        cfg.steps[0].kind = static_cast<uint8_t>(mask::core::ComplexStepKind::LaunchProgram);
        wcscpy_s(cfg.steps[0].text, L"notepad.exe");
        cfg.steps[1] = makeKeyClick('U');
        mask::core::SetSequenceUntrustedImport(cfg, true);

        (*engine.configs_)[0] = cfg;
        SanitizeComplexSequenceConfig((*engine.configs_)[0], 0U);
        (*engine.workerConfigs_)[0] = (*engine.configs_)[0];

        engine.StartSequenceSlot(0, false);
        const uint64_t wait2 = engine.AdvanceSequence(0, NowQpc());
        if (wait2 != 0ULL ||
            !engine.runtime_[0].waitingForCommandApproval ||
            s_promptCount != 2U ||
            s_lastIsCmd ||
            std::wcscmp(s_lastCmdText, L"notepad.exe") != 0 ||
            engine.testLaunchCount_ != 0U) {
            return false;
        }

        engine.ResolveCommandApproval(0, true);
        if (mask::core::IsSequenceUntrustedImport((*engine.configs_)[0])) {
            return false;
        }
        for (int guard = 0; guard < 8 && engine.runtime_[0].active; ++guard) {
            (void)engine.AdvanceSequence(0, NowQpc());
        }
        if (engine.runtime_[0].active ||
            engine.runtime_[0].waitingForCommandApproval ||
            mask::core::IsSequenceUntrustedImport((*engine.workerConfigs_)[0]) ||
            engine.testLaunchCount_ != 1U ||
            engine.testLastLaunchWasCmd_ ||
            std::wcscmp(engine.testLastLaunchText_, L"notepad.exe") != 0) {
            return false;
        }
        if (!drainKeyClicks(actual.data(), actual.size(), actualCount) ||
            actualCount != 1U || actual[0] != 'U') {
            return false;
        }

        // 6c. Already-trusted sequence runs immediately with zero approval prompt
        engine.StartSequenceSlot(0, false);
        for (int guard = 0; guard < 8 && engine.runtime_[0].active; ++guard) {
            (void)engine.AdvanceSequence(0, NowQpc());
        }
        if (engine.runtime_[0].active ||
            s_promptCount != 2U ||
            engine.testLaunchCount_ != 2U) {
            return false;
        }
        if (!drainKeyClicks(actual.data(), actual.size(), actualCount) ||
            actualCount != 1U || actual[0] != 'U') {
            return false;
        }
    }

    return true;
}

bool ComplexSequenceEngine::SaveSequences(
    const wchar_t* path,
    const std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>& cfgs
) noexcept {
    std::unique_ptr<SequencesFilePayload> payload(new (std::nothrow) SequencesFilePayload{});
    if (!payload) {
        return false;
    }
    payload->magic    = kComplexSequencesMagic;
    payload->version  = kComplexSequencesVersion;
    payload->count    = static_cast<uint32_t>(mask::core::kMaxComplexSequences);
    payload->reserved = 0U;
    for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
        payload->sequences[i] = cfgs[i];
        SanitizeComplexSequenceConfig(payload->sequences[i], i);
    }
    return WriteFileAtomic(path, payload.get(), sizeof(SequencesFilePayload));
}

bool ComplexSequenceEngine::LoadSequences(
    const wchar_t* path,
    std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>& outCfgs
) noexcept {
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }
    std::unique_ptr<SequencesFilePayload> payload(new (std::nothrow) SequencesFilePayload{});
    if (!payload) {
        return false;
    }

    bool ok = ReadSequencesFileRaw(path, *payload);
    if (!ok) {
        wchar_t bak[1040]{};
        if (std::swprintf(bak, std::size(bak), L"%ls.bak", path) >= 0) {
            ok = ReadSequencesFileRaw(bak, *payload);
        }
    }
    if (!ok) {
        return false;
    }

    for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
        SanitizeComplexSequenceConfig(payload->sequences[i], i);
        outCfgs[i] = payload->sequences[i];
    }
    return true;
}

bool ComplexSequenceEngine::RunSaveMigrationSelfTest() noexcept {
    wchar_t path[1040]{};
    if (!BuildUniqueTempFilePath(L"mask_seq_v1", path, std::size(path))) {
        return false;
    }

    struct CleanupGuard {
        const wchar_t* p;
        ~CleanupGuard() noexcept { CleanupTempFileTriplet(p); }
    } cleanup{path};

    std::unique_ptr<SequencesFilePayloadV1> v1(new (std::nothrow) SequencesFilePayloadV1{});
    if (!v1) {
        return false;
    }
    v1->magic    = kComplexSequencesMagic;
    v1->version  = kComplexSequencesVersionV1;
    v1->count    = static_cast<uint32_t>(mask::core::kMaxComplexSequences);
    v1->reserved = 0U;

    {
        auto& s       = v1->sequences[0];
        s.slotIndex   = 0U;
        s.enabled     = true;
        s.inUse       = true;
        s.triggerMode = 1U;
        s.triggerVk   = VK_F5;
        s.stepCount   = 3U;
        s.humanizePct = 10U;
        wcscpy_s(s.name, L"Legacy V1 Workflow");
        mask::core::SetSequenceUntrustedImport(s, true);
        s.reserved[1] = 0x55U; // obsolete byte cleared on sanitize

        s.steps[0].kind       = static_cast<uint8_t>(mask::core::ComplexStepKind::KeyClick);
        s.steps[0].vk         = 'E';
        s.steps[0].durationMs = 0U;

        s.steps[1].kind       = static_cast<uint8_t>(mask::core::ComplexStepKind::WaitMs);
        s.steps[1].durationMs = 4'500'000U; // > 600'000 / 3'600'000 -> uncapped

        s.steps[2].kind = static_cast<uint8_t>(mask::core::ComplexStepKind::RunCommand);
        wcscpy_s(s.steps[2].text, L"echo v1_migrated");
    }

    {
        auto& s       = v1->sequences[1];
        s.slotIndex   = 1U;
        s.enabled     = true;
        s.inUse       = true;
        s.triggerMode = 0U;
        s.stepCount   = 1U;
        wcscpy_s(s.name, L"User Local Launch");
        s.steps[0].kind = static_cast<uint8_t>(mask::core::ComplexStepKind::LaunchProgram);
        wcscpy_s(s.steps[0].text, L"C:\\Windows\\notepad.exe");
    }

    if (!WriteFileAtomic(path, v1.get(), sizeof(SequencesFilePayloadV1))) {
        return false;
    }

    std::unique_ptr<ConfigArray> loaded(new (std::nothrow) ConfigArray{});
    if (!loaded || !LoadSequences(path, *loaded)) {
        return false;
    }

    const auto& s0 = (*loaded)[0];
    if (!s0.enabled || !s0.inUse ||
        s0.triggerMode != 1U || s0.triggerVk != VK_F5 || s0.stepCount != 3U || s0.humanizePct != 10U ||
        !mask::core::IsSequenceUntrustedImport(s0) ||
        s0.reserved[1] != 0U ||
        s0.steps[0].durationMs != 0U ||
        s0.steps[1].durationMs != 4'500'000U ||
        std::wcscmp(s0.steps[2].text, L"echo v1_migrated") != 0 ||
        std::wcscmp(s0.name, L"Legacy V1 Workflow") != 0) {
        return false;
    }

    const auto& s1 = (*loaded)[1];
    if (mask::core::IsSequenceUntrustedImport(s1) ||
        std::wcscmp(s1.steps[0].text, L"C:\\Windows\\notepad.exe") != 0) {
        return false;
    }

    if (!SaveSequences(path, *loaded)) {
        return false;
    }
    std::unique_ptr<ConfigArray> v2Reloaded(new (std::nothrow) ConfigArray{});
    if (!v2Reloaded || !LoadSequences(path, *v2Reloaded)) {
        return false;
    }
    if ((*v2Reloaded)[0].steps[1].durationMs != 4'500'000U ||
        !mask::core::IsSequenceUntrustedImport((*v2Reloaded)[0]) ||
        mask::core::IsSequenceUntrustedImport((*v2Reloaded)[1])) {
        return false;
    }

    return true;
}

DWORD WINAPI ComplexSequenceEngine::ThreadEntry(LPVOID param) noexcept {
    auto* self = static_cast<ComplexSequenceEngine*>(param);
    if (self != nullptr) {
        self->WorkerLoop();
    }
    return 0;
}

void ComplexSequenceEngine::SignalWake() const noexcept {
    if (wakeEvent_ != nullptr) {
        SetEvent(wakeEvent_);
    }
}

uint64_t ComplexSequenceEngine::NowQpc() noexcept {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart);
}

uint64_t ComplexSequenceEngine::NowTsc() noexcept {
    return mask::timing::TimingEngine::ReadTsc();
}

uint64_t ComplexSequenceEngine::MsToQpc(uint64_t ms) const noexcept {
    if (ms == 0ULL) {
        return 0ULL;
    }
    const uint64_t wholeSec = ms / 1000ULL;
    const uint64_t remMs    = ms % 1000ULL;
    return (wholeSec * qpcFreqHz_) + ((remMs * qpcFreqHz_) / 1000ULL);
}

uint64_t ComplexSequenceEngine::QpcDeltaToNs(uint64_t deltaQpc) const noexcept {
    if (deltaQpc == 0ULL || qpcFreqHz_ == 0ULL) {
        return 0ULL;
    }
    constexpr uint64_t kNsPerSec  = 1'000'000'000ULL;
    constexpr uint64_t kMaxWaitNs = 3'600ULL * kNsPerSec;
    uint64_t high = 0;
    const uint64_t low = _umul128(deltaQpc, kNsPerSec, &high);
    if (high >= qpcFreqHz_) {
        return kMaxWaitNs;
    }
    uint64_t rem = 0;
    const uint64_t ns = _udiv128(high, low, qpcFreqHz_, &rem);
    if (ns == 0ULL) {
        return 1ULL;
    }
    return (std::min)(ns, kMaxWaitNs);
}

void ComplexSequenceEngine::PublishTriggerCache(
    size_t index,
    const mask::core::ComplexSequenceConfig& cfg
) noexcept {
    cachedTriggerVk_[index].store(cfg.triggerVk, std::memory_order_relaxed);
    const uint8_t activeBit = (cfg.enabled && cfg.inUse && cfg.stepCount > 0U) ? 1U : 0U;
    const uint8_t meta      = static_cast<uint8_t>(activeBit | ((cfg.triggerMode & 0x03U) << 1));
    cachedTriggerMeta_[index].store(meta, std::memory_order_release);
}

void ComplexSequenceEngine::PushAuxCommand(const mask::core::AuxOutputCommand& cmd) noexcept {
    if (sharedState_.auxOutputQueue.try_push(cmd)) {
        mask::input::WakeEngine();
    }
}

bool ComplexSequenceEngine::IsKeyHeldByOtherSequence(size_t ignoreIndex, uint16_t vk) const noexcept {
    const size_t word  = static_cast<size_t>((vk & 0xFFU) >> 6);
    const uint64_t bit = 1ULL << (vk & 63U);
    for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
        if (i == ignoreIndex || !runtime_[i].active) {
            continue;
        }
        if ((runtime_[i].heldVkBits[word] & bit) != 0ULL) {
            return true;
        }
    }
    return false;
}

void ComplexSequenceEngine::ReleaseSequenceHeldKeys(size_t index) noexcept {
    auto& rt             = runtime_[index];
    bool pushedAny       = false;
    const uint64_t nowTs = NowTsc();
    for (size_t w = 0; w < 4; ++w) {
        uint64_t bits     = rt.heldVkBits[w];
        rt.heldVkBits[w]  = 0ULL;
        while (bits != 0ULL) {
            const uint64_t bitIdx = CountTrailingZeros64(bits);
            bits &= (bits - 1ULL);
            const uint16_t vk = static_cast<uint16_t>((w << 6) | bitIdx);
            if (vk != 0U && !IsKeyHeldByOtherSequence(index, vk)) {
                mask::core::AuxOutputCommand cmd{};
                cmd.timestampTsc = nowTs;
                cmd.vk           = vk;
                cmd.kind         = static_cast<uint8_t>(mask::core::AuxActionKind::KeyUp);
                if (sharedState_.auxOutputQueue.try_push(cmd)) {
                    pushedAny = true;
                }
            }
        }
    }
    if (pushedAny) {
        mask::input::WakeEngine();
    }
}

void ComplexSequenceEngine::StopSequenceSlot(size_t index) noexcept {
    auto& rt                     = runtime_[index];
    rt.active                    = false;
    rt.toggleLatched             = false;
    rt.manualOneShot             = false;
    rt.waitingForTimer           = false;
    rt.waitingForColor           = false;
    rt.waitingForCommandApproval = false;
    rt.approvalPromptSent        = false;
    rt.approvalPendingStep       = 0U;
    rt.currentStep               = 0U;
    rt.waitDeadlineQpc           = 0ULL;
    rt.phaseAnchorQpc            = 0ULL;
    rt.nextColorPollQpc          = 0ULL;
    rt.colorTimeoutQpc           = 0ULL;
    rt.loopDepth                 = 0U;
    waitingApprovalTelemetry_[index].store(false, std::memory_order_release);
    pendingApprovalDecision_[index].store(kApprovalDecisionNone, std::memory_order_release);
    ReleaseSequenceHeldKeys(index);
    currentStepIndices_[index].store(0U, std::memory_order_release);
}

void ComplexSequenceEngine::AbortAllWorker() noexcept {
    for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
        if (runtime_[i].active ||
            (runtime_[i].heldVkBits[0] | runtime_[i].heldVkBits[1] |
             runtime_[i].heldVkBits[2] | runtime_[i].heldVkBits[3]) != 0ULL) {
            StopSequenceSlot(i);
        } else {
            runtime_[i].toggleLatched             = false;
            runtime_[i].manualOneShot             = false;
            runtime_[i].waitingForCommandApproval = false;
            runtime_[i].approvalPromptSent        = false;
            runtime_[i].approvalPendingStep       = 0U;
            waitingApprovalTelemetry_[i].store(false, std::memory_order_release);
            pendingApprovalDecision_[i].store(kApprovalDecisionNone, std::memory_order_release);
        }
        runtime_[i].lastSeenToggleEdges = toggleEdgeCounts_[i].load(std::memory_order_acquire);
    }
    activeSequenceBitmask_.store(0U, std::memory_order_release);
}

void ComplexSequenceEngine::StartSequenceSlot(size_t index, bool manualOneShot) noexcept {
    if (!workerConfigs_) {
        return;
    }
    const auto& cfg = (*workerConfigs_)[index];
    if (!cfg.enabled || !cfg.inUse || cfg.stepCount == 0U) {
        return;
    }
    auto& rt = runtime_[index];
    if (rt.active) {
        ReleaseSequenceHeldKeys(index);
    }
    rt.active                    = true;
    rt.manualOneShot             = manualOneShot;
    rt.waitingForTimer           = false;
    rt.waitingForColor           = false;
    rt.waitingForCommandApproval = false;
    rt.approvalPromptSent        = false;
    rt.approvalPendingStep       = 0U;
    rt.currentStep               = 0U;
    rt.waitDeadlineQpc           = 0ULL;
    rt.phaseAnchorQpc            = NowQpc();
    rt.nextColorPollQpc          = 0ULL;
    rt.colorTimeoutQpc           = 0ULL;
    rt.loopDepth                 = 0U;
    waitingApprovalTelemetry_[index].store(false, std::memory_order_release);
    pendingApprovalDecision_[index].store(kApprovalDecisionNone, std::memory_order_release);
    currentStepIndices_[index].store(0U, std::memory_order_release);
}

void ComplexSequenceEngine::SyncDirtyConfigs() noexcept {
    const uint16_t dirty = configDirtyMask_.exchange(0U, std::memory_order_acq_rel);
    if (dirty == 0U || !configs_ || !workerConfigs_) {
        return;
    }
    AcquireSRWLockShared(&configLock_);
    for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
        if ((dirty & static_cast<uint16_t>(1U << i)) != 0U) {
            (*workerConfigs_)[i] = (*configs_)[i];
        }
    }
    ReleaseSRWLockShared(&configLock_);

    for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
        if ((dirty & static_cast<uint16_t>(1U << i)) != 0U) {
            const auto& cfg = (*workerConfigs_)[i];
            if (runtime_[i].active && (!cfg.enabled || !cfg.inUse || cfg.stepCount == 0U)) {
                StopSequenceSlot(i);
            } else if (runtime_[i].active && runtime_[i].currentStep >= cfg.stepCount) {
                StopSequenceSlot(i);
            }
        }
    }
}

void ComplexSequenceEngine::DispatchAsyncLaunch(const wchar_t text[128], bool isCommand) noexcept {
    if (text[0] == L'\0') {
        return;
    }
    if (dryRunLaunchesForTest_) {
        ++testLaunchCount_;
        testLastLaunchWasCmd_ = isCommand;
        std::memcpy(testLastLaunchText_, text, sizeof(testLastLaunchText_));
        testLastLaunchText_[std::size(testLastLaunchText_) - 1] = L'\0';
        return;
    }
    auto& pool = GetAsyncLaunchPool();
    for (size_t i = 0; i < pool.size(); ++i) {
        bool expected = false;
        if (pool[i].inUse.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            pool[i].isCommand = isCommand;
            std::memcpy(pool[i].text, text, sizeof(pool[i].text));
            pool[i].text[std::size(pool[i].text) - 1] = L'\0';
            if (QueueUserWorkItem(&AsyncLaunchWorker, &pool[i], WT_EXECUTEDEFAULT) == FALSE) {
                pool[i].inUse.store(false, std::memory_order_release);
            }
            return;
        }
    }
}

void ComplexSequenceEngine::ProcessPendingTriggers() noexcept {
    const bool paused = sharedState_.automationPaused.load(std::memory_order_acquire) ||
                        sharedState_.flushAllRequested.load(std::memory_order_acquire);
    const bool abortNow = abortRequested_.exchange(false, std::memory_order_acq_rel);
    if (abortNow || paused) {
        pendingStartMask_.store(0U, std::memory_order_release);
        pendingManualTriggerMask_.store(0U, std::memory_order_release);
        AbortAllWorker();
        return;
    }

    if (!workerConfigs_) {
        return;
    }

    const uint16_t startMask  = pendingStartMask_.exchange(0U, std::memory_order_acq_rel);
    const uint16_t manualMask = pendingManualTriggerMask_.exchange(0U, std::memory_order_acq_rel);

    for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
        const auto& cfg    = (*workerConfigs_)[i];
        auto& rt           = runtime_[i];
        const uint16_t bit = static_cast<uint16_t>(1U << i);

        if (!cfg.enabled || !cfg.inUse || cfg.stepCount == 0U) {
            rt.lastSeenToggleEdges = toggleEdgeCounts_[i].load(std::memory_order_acquire);
            if (rt.active) {
                StopSequenceSlot(i);
            }
            continue;
        }

        const uint32_t edges = toggleEdgeCounts_[i].load(std::memory_order_acquire);
        if (edges != rt.lastSeenToggleEdges) {
            const uint32_t delta   = edges - rt.lastSeenToggleEdges;
            rt.lastSeenToggleEdges = edges;
            if (cfg.triggerMode == 1U && (delta & 1U) != 0U) {
                if (rt.active && rt.toggleLatched) {
                    StopSequenceSlot(i);
                } else {
                    rt.toggleLatched = true;
                    StartSequenceSlot(i, false);
                }
            }
        }

        if ((startMask & bit) != 0U && cfg.triggerMode == 0U) {
            StartSequenceSlot(i, false);
        }

        if (cfg.triggerMode == 2U) {
            const bool held = heldTriggerState_[i].load(std::memory_order_acquire) != 0U;
            if (held && !rt.active) {
                StartSequenceSlot(i, false);
            } else if (!held && rt.active && !rt.manualOneShot) {
                StopSequenceSlot(i);
            }
        }

        if ((manualMask & bit) != 0U) {
            if (cfg.triggerMode == 1U) {
                if (rt.active && rt.toggleLatched) {
                    StopSequenceSlot(i);
                } else {
                    rt.toggleLatched = true;
                    StartSequenceSlot(i, false);
                }
            } else if (cfg.triggerMode == 2U) {
                StartSequenceSlot(i, true);
            } else {
                StartSequenceSlot(i, false);
            }
        }
    }
}

bool ComplexSequenceEngine::EvaluateCondition(
    const mask::core::ComplexStep& step,
    SequenceRuntimeState& rt
) const noexcept {
    bool matched = false;
    switch (mask::core::GetStepConditionKind(step)) {
        case mask::core::ComplexConditionKind::KeyHeld: {
            const uint16_t vk = static_cast<uint16_t>(step.vk & 0xFFU);
            if (vk != 0U) {
                const bool phys  = sharedState_.physicalKeyState[vk].load(std::memory_order_acquire) != 0U;
                const bool synth = sharedState_.syntheticHeldState[vk].load(std::memory_order_relaxed) != 0U;
                matched = phys || (!synth && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0);
            }
            break;
        }
        case mask::core::ComplexConditionKind::ColorMatches: {
            const PixelSampleFn sampler = pixelSampler_.load(std::memory_order_acquire);
            if (sampler != nullptr) {
                uint8_t r = 0U, g = 0U, b = 0U;
                if (sampler(step.x, step.y, r, g, b)) {
                    const int dr  = static_cast<int>(r) - static_cast<int>(step.r);
                    const int dg  = static_cast<int>(g) - static_cast<int>(step.g);
                    const int db  = static_cast<int>(b) - static_cast<int>(step.b);
                    const int tol = static_cast<int>(step.tolerance);
                    matched = (dr >= -tol && dr <= tol) &&
                              (dg >= -tol && dg <= tol) &&
                              (db >= -tol && db <= tol);
                }
            }
            break;
        }
        case mask::core::ComplexConditionKind::LayerIs: {
            matched = ((sharedState_.activeRemapLayer.load(std::memory_order_acquire) & 3U) ==
                       (step.param & 3U));
            break;
        }
        case mask::core::ComplexConditionKind::UnlockStateIs: {
            const bool unlocked = sharedState_.remapUnlocked.load(std::memory_order_acquire);
            matched = (step.param != 0U) ? !unlocked : unlocked;
            break;
        }
        case mask::core::ComplexConditionKind::MacroActive: {
            const uint32_t slotIdx = step.param % static_cast<uint32_t>(mask::core::kMaxMacroSlots);
            const auto snap        = sharedState_.ReadTelemetry();
            matched = (((snap.activeSlotBitmask | snap.toggledSlotBitmask) & (1ULL << slotIdx)) != 0ULL);
            break;
        }
        case mask::core::ComplexConditionKind::RandomChance: {
            const uint32_t pct = (std::min)(step.param, 100U);
            if (pct >= 100U) {
                matched = true;
            } else if (pct == 0U) {
                matched = false;
            } else {
                matched = (NextXorShift64Star(rt.rngState) % 100ULL) < static_cast<uint64_t>(pct);
            }
            break;
        }
        default:
            matched = false;
            break;
    }
    if (mask::core::IsStepConditionNegated(step)) {
        matched = !matched;
    }
    return matched;
}

uint16_t ComplexSequenceEngine::FindConditionalJumpTarget(
    const mask::core::ComplexSequenceConfig& cfg,
    uint16_t fromStep,
    bool stopAtElse
) noexcept {
    uint32_t depth = 0U;
    for (uint16_t i = fromStep; i < cfg.stepCount; ++i) {
        const auto k = static_cast<mask::core::ComplexStepKind>(cfg.steps[i].kind);
        if (k == mask::core::ComplexStepKind::IfCheck) {
            ++depth;
        } else if (k == mask::core::ComplexStepKind::ElseBranch) {
            if (stopAtElse && depth == 0U) {
                return static_cast<uint16_t>(i + 1U);
            }
        } else if (k == mask::core::ComplexStepKind::EndIf) {
            if (depth == 0U) {
                return static_cast<uint16_t>(i + 1U);
            }
            --depth;
        }
    }
    return cfg.stepCount;
}

uint64_t ComplexSequenceEngine::AdvanceSequence(size_t index, uint64_t nowQpc) noexcept {
    if (!workerConfigs_) {
        return 0ULL;
    }
    auto& cfg = (*workerConfigs_)[index];
    auto& rt  = runtime_[index];
    if (!rt.active) {
        return 0ULL;
    }

    uint32_t stepsThisTick = 0U;
    while (rt.active && stepsThisTick < kMaxImmediateStepsPerTick) {
        if (rt.waitingForCommandApproval) {
            const uint8_t decision = pendingApprovalDecision_[index].exchange(
                kApprovalDecisionNone, std::memory_order_acq_rel);
            if (decision == kApprovalDecisionAllow) {
                mask::core::SetSequenceUntrustedImport(cfg, false);
                rt.waitingForCommandApproval = false;
                rt.approvalPendingStep       = 0U;
                rt.approvalPromptSent        = false;
                waitingApprovalTelemetry_[index].store(false, std::memory_order_release);
                rt.phaseAnchorQpc = nowQpc;
            } else if (decision == kApprovalDecisionDeny) {
                rt.waitingForCommandApproval = false;
                rt.approvalPendingStep       = 0U;
                rt.approvalPromptSent        = false;
                waitingApprovalTelemetry_[index].store(false, std::memory_order_release);
                rt.phaseAnchorQpc = nowQpc;
                ++rt.currentStep;
                continue;
            } else if (!mask::core::IsSequenceUntrustedImport(cfg)) {
                rt.waitingForCommandApproval = false;
                rt.approvalPendingStep       = 0U;
                rt.approvalPromptSent        = false;
                waitingApprovalTelemetry_[index].store(false, std::memory_order_release);
                rt.phaseAnchorQpc = nowQpc;
            } else {
                return 0ULL;
            }
        }

        if (rt.waitingForTimer) {
            if (nowQpc < rt.waitDeadlineQpc) {
                return rt.waitDeadlineQpc;
            }
            const uint64_t deadline = rt.waitDeadlineQpc;
            rt.waitingForTimer      = false;
            rt.waitDeadlineQpc      = 0ULL;
            rt.phaseAnchorQpc       = (nowQpc - deadline <= MsToQpc(50ULL)) ? deadline : nowQpc;
            ++rt.currentStep;
        }

        if (rt.waitingForColor) {
            if (rt.currentStep >= cfg.stepCount) {
                rt.waitingForColor  = false;
                rt.colorTimeoutQpc  = 0ULL;
                rt.nextColorPollQpc = 0ULL;
                rt.phaseAnchorQpc   = nowQpc;
            } else {
                if (rt.colorTimeoutQpc != 0ULL && nowQpc >= rt.colorTimeoutQpc) {
                    rt.waitingForColor  = false;
                    rt.colorTimeoutQpc  = 0ULL;
                    rt.nextColorPollQpc = 0ULL;
                    rt.phaseAnchorQpc   = nowQpc;
                    ++rt.currentStep;
                } else if (nowQpc < rt.nextColorPollQpc) {
                    return (rt.colorTimeoutQpc != 0ULL && rt.colorTimeoutQpc < rt.nextColorPollQpc)
                        ? rt.colorTimeoutQpc
                        : rt.nextColorPollQpc;
                } else {
                    const PixelSampleFn sampler = pixelSampler_.load(std::memory_order_acquire);
                    if (sampler == nullptr) {
                        rt.waitingForColor  = false;
                        rt.colorTimeoutQpc  = 0ULL;
                        rt.nextColorPollQpc = 0ULL;
                        rt.phaseAnchorQpc   = nowQpc;
                        ++rt.currentStep;
                    } else {
                        const auto& step = cfg.steps[rt.currentStep];
                        uint8_t r = 0, g = 0, b = 0;
                        bool matched = false;
                        if (sampler(step.x, step.y, r, g, b)) {
                            const int dr  = static_cast<int>(r) - static_cast<int>(step.r);
                            const int dg  = static_cast<int>(g) - static_cast<int>(step.g);
                            const int db  = static_cast<int>(b) - static_cast<int>(step.b);
                            const int tol = static_cast<int>(step.tolerance);
                            matched = (dr >= -tol && dr <= tol) &&
                                      (dg >= -tol && dg <= tol) &&
                                      (db >= -tol && db <= tol);
                        }
                        if (matched) {
                            rt.waitingForColor  = false;
                            rt.colorTimeoutQpc  = 0ULL;
                            rt.nextColorPollQpc = 0ULL;
                            rt.phaseAnchorQpc   = nowQpc;
                            ++rt.currentStep;
                        } else {
                            rt.nextColorPollQpc = nowQpc + MsToQpc(kColorPollIntervalMs);
                            return (rt.colorTimeoutQpc != 0ULL && rt.colorTimeoutQpc < rt.nextColorPollQpc)
                                ? rt.colorTimeoutQpc
                                : rt.nextColorPollQpc;
                        }
                    }
                }
            }
        }

        if (rt.currentStep >= cfg.stepCount) {
            const bool shouldLoop =
                (cfg.triggerMode == 1U && rt.toggleLatched) ||
                (cfg.triggerMode == 2U && !rt.manualOneShot &&
                 heldTriggerState_[index].load(std::memory_order_acquire) != 0U);
            if (shouldLoop) {
                rt.currentStep = 0U;
                rt.loopDepth   = 0U;
                currentStepIndices_[index].store(0U, std::memory_order_release);
                ++stepsThisTick;
                continue;
            }
            StopSequenceSlot(index);
            return 0ULL;
        }

        currentStepIndices_[index].store(rt.currentStep, std::memory_order_release);
        const auto& step = cfg.steps[rt.currentStep];
        const auto kind  = static_cast<mask::core::ComplexStepKind>(step.kind);
        ++stepsThisTick;

        switch (kind) {
        case mask::core::ComplexStepKind::KeyClick: {
            const uint16_t vk     = static_cast<uint16_t>(step.vk & 0xFFU);
            const uint32_t holdMs = (step.durationMs > 0U)
                ? ApplyHumanizeMs(step.durationMs, cfg.humanizePct, rt.rngState)
                : 0U;
            if (vk != 0U) {
                mask::core::AuxOutputCommand cmd{};
                cmd.timestampTsc = NowTsc();
                cmd.vk           = vk;
                cmd.holdMs       = holdMs;
                cmd.kind         = static_cast<uint8_t>(mask::core::AuxActionKind::KeyClick);
                PushAuxCommand(cmd);
            }
            if (step.durationMs > 0U) {
                const uint64_t baseQpc = (rt.phaseAnchorQpc != 0ULL) ? rt.phaseAnchorQpc : nowQpc;
                rt.waitingForTimer     = true;
                rt.waitDeadlineQpc     = baseQpc + MsToQpc(holdMs);
                return rt.waitDeadlineQpc;
            }
            ++rt.currentStep;
            break;
        }
        case mask::core::ComplexStepKind::KeyDown: {
            const uint16_t vk = static_cast<uint16_t>(step.vk & 0xFFU);
            if (vk != 0U) {
                rt.heldVkBits[vk >> 6] |= (1ULL << (vk & 63U));
                mask::core::AuxOutputCommand cmd{};
                cmd.timestampTsc = NowTsc();
                cmd.vk           = vk;
                cmd.kind         = static_cast<uint8_t>(mask::core::AuxActionKind::KeyDown);
                PushAuxCommand(cmd);
            }
            ++rt.currentStep;
            break;
        }
        case mask::core::ComplexStepKind::KeyUp: {
            const uint16_t vk = static_cast<uint16_t>(step.vk & 0xFFU);
            if (vk != 0U) {
                rt.heldVkBits[vk >> 6] &= ~(1ULL << (vk & 63U));
                if (!IsKeyHeldByOtherSequence(index, vk)) {
                    mask::core::AuxOutputCommand cmd{};
                    cmd.timestampTsc = NowTsc();
                    cmd.vk           = vk;
                    cmd.kind         = static_cast<uint8_t>(mask::core::AuxActionKind::KeyUp);
                    PushAuxCommand(cmd);
                }
            }
            ++rt.currentStep;
            break;
        }
        case mask::core::ComplexStepKind::MouseMoveRel: {
            mask::core::AuxOutputCommand cmd{};
            cmd.timestampTsc = NowTsc();
            cmd.argX         = step.x;
            cmd.argY         = step.y;
            cmd.kind         = static_cast<uint8_t>(mask::core::AuxActionKind::MouseMoveRel);
            PushAuxCommand(cmd);
            ++rt.currentStep;
            break;
        }
        case mask::core::ComplexStepKind::MouseMoveAbs: {
            mask::core::AuxOutputCommand cmd{};
            cmd.timestampTsc = NowTsc();
            cmd.argX         = step.x;
            cmd.argY         = step.y;
            cmd.kind         = static_cast<uint8_t>(mask::core::AuxActionKind::MouseMoveAbs);
            PushAuxCommand(cmd);
            ++rt.currentStep;
            break;
        }
        case mask::core::ComplexStepKind::MouseScroll: {
            int32_t wheelDelta = 0;
            int32_t panDelta   = 0;
            if (step.param != 0U) {
                wheelDelta = static_cast<int32_t>(step.param);
                panDelta   = step.x;
            } else if (step.y != 0) {
                wheelDelta = step.y;
                panDelta   = step.x;
            } else {
                wheelDelta = step.x;
                panDelta   = 0;
            }
            mask::core::AuxOutputCommand cmd{};
            cmd.timestampTsc = NowTsc();
            cmd.argX         = wheelDelta;
            cmd.argY         = panDelta;
            cmd.kind         = static_cast<uint8_t>(mask::core::AuxActionKind::MouseScroll);
            PushAuxCommand(cmd);
            ++rt.currentStep;
            break;
        }
        case mask::core::ComplexStepKind::WaitMs: {
            const uint32_t waitMs = ApplyHumanizeMs(step.durationMs, cfg.humanizePct, rt.rngState);
            if (waitMs > 0U) {
                const uint64_t baseQpc = (rt.phaseAnchorQpc != 0ULL) ? rt.phaseAnchorQpc : nowQpc;
                rt.waitingForTimer     = true;
                rt.waitDeadlineQpc     = baseQpc + MsToQpc(waitMs);
                return rt.waitDeadlineQpc;
            }
            ++rt.currentStep;
            break;
        }
        case mask::core::ComplexStepKind::WaitForColor: {
            const PixelSampleFn sampler = pixelSampler_.load(std::memory_order_acquire);
            if (sampler == nullptr) {
                ++rt.currentStep;
                break;
            }
            uint8_t r = 0, g = 0, b = 0;
            bool matched = false;
            if (sampler(step.x, step.y, r, g, b)) {
                const int dr  = static_cast<int>(r) - static_cast<int>(step.r);
                const int dg  = static_cast<int>(g) - static_cast<int>(step.g);
                const int db  = static_cast<int>(b) - static_cast<int>(step.b);
                const int tol = static_cast<int>(step.tolerance);
                matched = (dr >= -tol && dr <= tol) &&
                          (dg >= -tol && dg <= tol) &&
                          (db >= -tol && db <= tol);
            }
            if (matched) {
                ++rt.currentStep;
                break;
            }
            const uint32_t timeoutMs = (step.param > 0U) ? step.param : step.durationMs;
            rt.waitingForColor       = true;
            rt.colorTimeoutQpc       = (timeoutMs > 0U) ? (nowQpc + MsToQpc(timeoutMs)) : 0ULL;
            rt.nextColorPollQpc      = nowQpc + MsToQpc(kColorPollIntervalMs);
            return (rt.colorTimeoutQpc != 0ULL && rt.colorTimeoutQpc < rt.nextColorPollQpc)
                ? rt.colorTimeoutQpc
                : rt.nextColorPollQpc;
        }
        case mask::core::ComplexStepKind::LoopStart: {
            if (rt.loopDepth < kMaxLoopNesting) {
                auto& frame               = rt.loopStack[rt.loopDepth++];
                frame.startStep           = static_cast<uint16_t>(rt.currentStep + 1U);
                frame.remainingIterations = step.param;
                frame.infinite            = (step.param == 0U);
            }
            ++rt.currentStep;
            break;
        }
        case mask::core::ComplexStepKind::LoopEnd: {
            if (rt.loopDepth > 0U) {
                auto& frame = rt.loopStack[rt.loopDepth - 1U];
                if (frame.startStep >= cfg.stepCount) {
                    --rt.loopDepth;
                    ++rt.currentStep;
                } else if (frame.infinite) {
                    rt.currentStep = frame.startStep;
                } else if (frame.remainingIterations > 1U) {
                    --frame.remainingIterations;
                    rt.currentStep = frame.startStep;
                } else {
                    --rt.loopDepth;
                    ++rt.currentStep;
                }
            } else {
                ++rt.currentStep;
            }
            break;
        }
        case mask::core::ComplexStepKind::LaunchProgram:
        case mask::core::ComplexStepKind::RunCommand: {
            const bool isShellCmd = (kind == mask::core::ComplexStepKind::RunCommand);
            if (mask::core::IsSequenceUntrustedImport(cfg)) {
                rt.waitingForCommandApproval = true;
                rt.approvalPendingStep       = rt.currentStep;
                waitingApprovalTelemetry_[index].store(true, std::memory_order_release);
                if (!rt.approvalPromptSent) {
                    rt.approvalPromptSent = true;
                    if (const CommandApprovalFn cb = commandApprovalCb_.load(std::memory_order_acquire)) {
                        cb(static_cast<uint8_t>(index), rt.currentStep, isShellCmd, step.text);
                    }
                }
                return 0ULL;
            }
            DispatchAsyncLaunch(step.text, isShellCmd);
            ++rt.currentStep;
            break;
        }
        case mask::core::ComplexStepKind::SetLayer: {
            mask::core::AuxOutputCommand cmd{};
            cmd.timestampTsc = NowTsc();
            cmd.auxIndex     = static_cast<uint8_t>(step.param % mask::core::kMaxRemapLayers);
            cmd.kind         = static_cast<uint8_t>(mask::core::AuxActionKind::SetLayer);
            PushAuxCommand(cmd);
            ++rt.currentStep;
            break;
        }
        case mask::core::ComplexStepKind::ToggleMacro: {
            const uint8_t macroSlot = static_cast<uint8_t>(step.param & 0xFFU);
            if (macroSlot < mask::core::kMaxMacroSlots) {
                const ToggleMacroFn fn = toggleMacroFn_.load(std::memory_order_acquire);
                if (fn != nullptr) {
                    fn(macroSlot);
                } else {
                    mask::core::AuxOutputCommand cmd{};
                    cmd.timestampTsc = NowTsc();
                    cmd.auxIndex     = macroSlot;
                    cmd.kind         = static_cast<uint8_t>(mask::core::AuxActionKind::TriggerMacroSlot);
                    PushAuxCommand(cmd);
                }
            }
            ++rt.currentStep;
            break;
        }
        case mask::core::ComplexStepKind::IfCheck: {
            if (EvaluateCondition(step, rt)) {
                ++rt.currentStep;
            } else {
                rt.currentStep = FindConditionalJumpTarget(cfg, static_cast<uint16_t>(rt.currentStep + 1U), true);
            }
            break;
        }
        case mask::core::ComplexStepKind::ElseBranch: {
            rt.currentStep = FindConditionalJumpTarget(cfg, static_cast<uint16_t>(rt.currentStep + 1U), false);
            break;
        }
        case mask::core::ComplexStepKind::EndIf: {
            ++rt.currentStep;
            break;
        }
        default:
            ++rt.currentStep;
            break;
        }
    }

    if (rt.active) {
        return nowQpc + MsToQpc(1ULL);
    }
    return 0ULL;
}

void ComplexSequenceEngine::WorkerLoop() noexcept {
    if (stopRequested_.load(std::memory_order_acquire)) {
        AbortAllWorker();
        return;
    }

    mask::timing::TimingEngine seqTiming;
    seqTiming.BoostCurrentThreadRealtimePriority();
    seqTiming.SetSpinPolicy(2U); // Eco: 60..800 us adaptive spin lead, 50 us safety margin

    while (!stopRequested_.load(std::memory_order_acquire)) {
        SyncDirtyConfigs();
        ProcessPendingTriggers();

        if (stopRequested_.load(std::memory_order_acquire)) {
            break;
        }

        const uint64_t nowQpc    = NowQpc();
        uint16_t activeMask      = 0U;
        bool hasScheduledWake    = false;
        uint64_t earliestWakeQpc = (std::numeric_limits<uint64_t>::max)();

        for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
            if (!runtime_[i].active) {
                continue;
            }
            const uint64_t nextQpc = AdvanceSequence(i, nowQpc);
            if (runtime_[i].active) {
                activeMask |= static_cast<uint16_t>(1U << i);
                if (nextQpc != 0ULL) {
                    hasScheduledWake = true;
                    if (nextQpc < earliestWakeQpc) {
                        earliestWakeQpc = nextQpc;
                    }
                }
            }
        }

        activeSequenceBitmask_.store(activeMask, std::memory_order_release);

        if (stopRequested_.load(std::memory_order_acquire)) {
            break;
        }

        // 0.00% CPU while idle or while all active sequences are parked awaiting command approval
        if (activeMask == 0U || !hasScheduledWake) {
            if (wakeEvent_ != nullptr) {
                WaitForSingleObject(wakeEvent_, INFINITE);
            } else {
                Sleep(5);
            }
            continue;
        }

        const uint64_t afterQpc = NowQpc();
        if (earliestWakeQpc > afterQpc && earliestWakeQpc != (std::numeric_limits<uint64_t>::max)()) {
            const uint64_t deltaQpc = earliestWakeQpc - afterQpc;
            const uint64_t deltaNs  = QpcDeltaToNs(deltaQpc);
            if (deltaNs > 0ULL) {
                if (wakeEvent_ != nullptr) {
                    const uint64_t nowTsc    = mask::timing::TimingEngine::ReadTsc();
                    const uint64_t targetTsc = nowTsc + seqTiming.NanosecondsToTsc(deltaNs);
                    (void)seqTiming.WaitUntilTscOrEvent(targetTsc, wakeEvent_, 50'000'000ULL);
                } else {
                    const uint64_t ms = std::clamp<uint64_t>(deltaNs / 1'000'000ULL, 1ULL, 50ULL);
                    Sleep(static_cast<DWORD>(ms));
                }
            }
        }
    }

    AbortAllWorker();
}

} // namespace mask::workflows
