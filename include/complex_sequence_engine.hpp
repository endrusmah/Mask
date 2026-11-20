#pragma once

// Multi-Step Workflow / Complex Sequence Engine (up to 16 concurrent sequences, 128 steps each).
// Runs on a dedicated event-driven background thread (0.00% CPU while idle) using a high-resolution
// waitable timer + auto-reset wake event, serializing all hardware outputs through EngineSharedState::auxOutputQueue.

#include "core_contracts.hpp"
#include "input_pipeline.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
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

namespace mask::workflows {

using PixelSampleFn = bool(*)(int32_t x, int32_t y, uint8_t& r, uint8_t& g, uint8_t& b) noexcept;
using ToggleMacroFn = void(*)(uint8_t macroSlot) noexcept;

inline constexpr uint32_t kComplexSequencesMagic   = 0x4D534551U; // 'MSEQ'
inline constexpr uint32_t kComplexSequencesVersion = 1U;
inline constexpr size_t   kMaxLoopNesting          = 8U;

namespace detail {

[[nodiscard]] inline bool SanitizeBool(const bool& value) noexcept {
    uint8_t raw = 0;
    std::memcpy(&raw, &value, 1);
    return raw != 0U;
}

[[nodiscard]] inline uint64_t NextXorShift64Star(uint64_t& state) noexcept {
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

[[nodiscard]] inline uint32_t ApplyHumanizeMs(uint32_t baseMs, uint8_t humanizePct, uint64_t& rngState) noexcept {
    if (baseMs == 0U || humanizePct == 0U) {
        return baseMs;
    }
    const uint32_t pct = (humanizePct > 50U) ? 50U : static_cast<uint32_t>(humanizePct);
    const uint64_t span = (static_cast<uint64_t>(baseMs) * pct) / 100ULL;
    if (span == 0ULL) {
        return baseMs;
    }
    const uint64_t range = span * 2ULL + 1ULL;
    const uint64_t offset = NextXorShift64Star(rngState) % range;
    const int64_t signedDelta = static_cast<int64_t>(offset) - static_cast<int64_t>(span);
    const int64_t adjusted = static_cast<int64_t>(baseMs) + signedDelta;
    if (adjusted < 1) {
        return 1U;
    }
    if (adjusted > static_cast<int64_t>((std::numeric_limits<uint32_t>::max)())) {
        return (std::numeric_limits<uint32_t>::max)();
    }
    return static_cast<uint32_t>(adjusted);
}

inline void SanitizeComplexStep(mask::core::ComplexStep& s) noexcept {
    if (s.kind > static_cast<uint8_t>(mask::core::ComplexStepKind::ToggleMacro)) {
        s.kind = static_cast<uint8_t>(mask::core::ComplexStepKind::WaitMs);
    }
    s.vk = static_cast<uint16_t>(s.vk & 0xFFU);
    if (s.durationMs > 3'600'000U) {
        s.durationMs = 3'600'000U;
    }
    s.text[std::size(s.text) - 1] = L'\0';
}

inline void SanitizeComplexSequenceConfig(mask::core::ComplexSequenceConfig& c, size_t slotIdx) noexcept {
    c.slotIndex = static_cast<uint8_t>(slotIdx);
    c.enabled = SanitizeBool(c.enabled);
    c.inUse = SanitizeBool(c.inUse);
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
    c.reserved[0] = 0U;
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

struct SequencesFilePayload {
    uint32_t magic{kComplexSequencesMagic};
    uint32_t version{kComplexSequencesVersion};
    uint32_t count{static_cast<uint32_t>(mask::core::kMaxComplexSequences)};
    uint32_t reserved{0};
    std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences> sequences{};
};

static_assert(std::is_trivially_copyable_v<SequencesFilePayload>,
              "SequencesFilePayload must be trivially copyable.");

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

[[nodiscard]] inline bool ReadSequencesFileRaw(const wchar_t* path, SequencesFilePayload& out) noexcept {
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
              fileSize.QuadPart == static_cast<LONGLONG>(sizeof(SequencesFilePayload));

    if (ok) {
        DWORD bytesRead = 0;
        ok = ReadFile(h, &out, static_cast<DWORD>(sizeof(SequencesFilePayload)), &bytesRead, nullptr) != FALSE &&
             bytesRead == static_cast<DWORD>(sizeof(SequencesFilePayload)) &&
             out.magic == kComplexSequencesMagic &&
             out.version == kComplexSequencesVersion &&
             out.count == static_cast<uint32_t>(mask::core::kMaxComplexSequences);
    }

    CloseHandle(h);
    return ok;
}

struct AsyncLaunchSlot {
    std::atomic<bool> inUse{false};
    bool              isCommand{false};
    wchar_t           text[128]{};
};

[[nodiscard]] inline std::array<AsyncLaunchSlot, 16>& GetAsyncLaunchPool() noexcept {
    static std::array<AsyncLaunchSlot, 16> pool{};
    return pool;
}

inline DWORD WINAPI AsyncLaunchWorker(LPVOID param) noexcept {
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
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
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

} // namespace detail

/**
 * @brief Multi-Step Complex Sequence Engine supporting up to 16 concurrent workflows (128 steps each).
 *
 * - Runs on a dedicated event-driven background thread (0.00% CPU when idle).
 * - Uses a persistent high-resolution waitable timer + auto-reset wake event for instant cancellation.
 * - Zero heap allocations on the execution path (fixed loop stack, fixed key-hold bitsets, fixed async launch slots).
 */
class ComplexSequenceEngine {
public:
    explicit ComplexSequenceEngine(
        mask::core::EngineSharedState& sharedState,
        PixelSampleFn pixelSampler = nullptr
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
        }

        wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        waitableTimer_ = CreateWaitableTimerExW(
            nullptr,
            nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS
        );
        if (waitableTimer_ == nullptr) {
            waitableTimer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
    }

    ~ComplexSequenceEngine() noexcept {
        Stop();
        if (waitableTimer_ != nullptr) {
            CloseHandle(waitableTimer_);
            waitableTimer_ = nullptr;
        }
        if (wakeEvent_ != nullptr) {
            CloseHandle(wakeEvent_);
            wakeEvent_ = nullptr;
        }
    }

    ComplexSequenceEngine(const ComplexSequenceEngine&) = delete;
    ComplexSequenceEngine& operator=(const ComplexSequenceEngine&) = delete;

    void Start() {
        if (running_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        stopRequested_.store(false, std::memory_order_release);
        if (wakeEvent_ == nullptr) {
            wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
        if (waitableTimer_ == nullptr) {
            waitableTimer_ = CreateWaitableTimerExW(
                nullptr,
                nullptr,
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                TIMER_ALL_ACCESS
            );
            if (waitableTimer_ == nullptr) {
                waitableTimer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            }
        }
        workerThread_ = CreateThread(nullptr, 0, &ComplexSequenceEngine::ThreadEntry, this, 0, nullptr);
        if (workerThread_ == nullptr) {
            running_.store(false, std::memory_order_release);
        }
    }

    void Stop() noexcept {
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
        for (auto& stepIdx : currentStepIndices_) {
            stepIdx.store(0U, std::memory_order_release);
        }
    }

    void SetPixelSampler(PixelSampleFn pixelSampler) noexcept {
        pixelSampler_.store(pixelSampler, std::memory_order_release);
    }

    void SetToggleMacroCallback(ToggleMacroFn toggleMacro) noexcept {
        toggleMacroFn_.store(toggleMacro, std::memory_order_release);
    }

    void UpdateConfig(size_t index, const mask::core::ComplexSequenceConfig& cfg) noexcept {
        if (index >= mask::core::kMaxComplexSequences || !configs_) {
            return;
        }
        AcquireSRWLockExclusive(&configLock_);
        (*configs_)[index] = cfg;
        detail::SanitizeComplexSequenceConfig((*configs_)[index], index);
        PublishTriggerCache(index, (*configs_)[index]);
        ReleaseSRWLockExclusive(&configLock_);

        configDirtyMask_.fetch_or(static_cast<uint16_t>(1U << index), std::memory_order_release);
        SignalWake();
    }

    void UpdateAllConfigs(
        const std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>& cfgs
    ) noexcept {
        if (!configs_) {
            return;
        }
        AcquireSRWLockExclusive(&configLock_);
        for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
            (*configs_)[i] = cfgs[i];
            detail::SanitizeComplexSequenceConfig((*configs_)[i], i);
            PublishTriggerCache(i, (*configs_)[i]);
        }
        ReleaseSRWLockExclusive(&configLock_);

        configDirtyMask_.store(0xFFFFU, std::memory_order_release);
        SignalWake();
    }

    void NotifyPhysicalKeyTransition(uint16_t vk, bool isDown) noexcept {
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
                // WhileHeld: always honor release even if paused/suppressed; gate press on !suppressNewStarts
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
                    // RunOnce
                    pendingStartMask_.fetch_or(static_cast<uint16_t>(1U << i), std::memory_order_release);
                    matched = true;
                } else if (mode == 1U) {
                    // ToggleLoop
                    toggleEdgeCounts_[i].fetch_add(1U, std::memory_order_release);
                    matched = true;
                }
            }
        }

        if (matched) {
            SignalWake();
        }
    }

    void TriggerSequence(size_t index) noexcept {
        if (index >= mask::core::kMaxComplexSequences) {
            return;
        }
        if (sharedState_.automationPaused.load(std::memory_order_acquire)) {
            return;
        }
        pendingManualTriggerMask_.fetch_or(static_cast<uint16_t>(1U << index), std::memory_order_release);
        SignalWake();
    }

    void AbortAll() noexcept {
        pendingStartMask_.store(0U, std::memory_order_release);
        pendingManualTriggerMask_.store(0U, std::memory_order_release);
        for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
            heldTriggerState_[i].store(0U, std::memory_order_release);
        }
        abortRequested_.store(true, std::memory_order_release);
        SignalWake();
    }

    [[nodiscard]] uint16_t GetActiveSequenceBitmask() const noexcept {
        return activeSequenceBitmask_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint16_t GetCurrentStepIndex(size_t index) const noexcept {
        if (index >= mask::core::kMaxComplexSequences) {
            return 0U;
        }
        return currentStepIndices_[index].load(std::memory_order_acquire);
    }

    [[nodiscard]] static bool SaveSequences(
        const wchar_t* path,
        const std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>& cfgs
    ) noexcept {
        std::unique_ptr<detail::SequencesFilePayload> payload(new (std::nothrow) detail::SequencesFilePayload{});
        if (!payload) {
            return false;
        }
        payload->magic = kComplexSequencesMagic;
        payload->version = kComplexSequencesVersion;
        payload->count = static_cast<uint32_t>(mask::core::kMaxComplexSequences);
        payload->reserved = 0U;
        for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
            payload->sequences[i] = cfgs[i];
            detail::SanitizeComplexSequenceConfig(payload->sequences[i], i);
        }
        return detail::WriteFileAtomic(path, payload.get(), sizeof(detail::SequencesFilePayload));
    }

    [[nodiscard]] static bool LoadSequences(
        const wchar_t* path,
        std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>& outCfgs
    ) noexcept {
        if (path == nullptr || path[0] == L'\0') {
            return false;
        }
        std::unique_ptr<detail::SequencesFilePayload> payload(new (std::nothrow) detail::SequencesFilePayload{});
        if (!payload) {
            return false;
        }

        bool ok = detail::ReadSequencesFileRaw(path, *payload);
        if (!ok) {
            wchar_t bak[1040]{};
            if (std::swprintf(bak, std::size(bak), L"%ls.bak", path) >= 0) {
                ok = detail::ReadSequencesFileRaw(bak, *payload);
            }
        }
        if (!ok) {
            return false;
        }

        for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
            detail::SanitizeComplexSequenceConfig(payload->sequences[i], i);
            outCfgs[i] = payload->sequences[i];
        }
        return true;
    }

private:
    static constexpr uint32_t kMaxImmediateStepsPerTick = 64U;
    static constexpr uint32_t kColorPollIntervalMs      = 4U;

    struct LoopFrame {
        uint16_t startStep{0};
        uint32_t remainingIterations{0};
        bool     infinite{false};
    };

    struct SequenceRuntimeState {
        bool     active{false};
        bool     toggleLatched{false};
        bool     manualOneShot{false};
        bool     waitingForTimer{false};
        bool     waitingForColor{false};
        uint16_t currentStep{0};
        uint64_t waitDeadlineQpc{0};
        uint64_t nextColorPollQpc{0};
        uint64_t colorTimeoutQpc{0}; // 0 = no timeout
        uint32_t lastSeenToggleEdges{0};
        uint64_t rngState{0x9E3779B97F4A7C15ULL};
        uint8_t  loopDepth{0};
        std::array<LoopFrame, kMaxLoopNesting> loopStack{};
        std::array<uint64_t, 4> heldVkBits{};
    };

    static DWORD WINAPI ThreadEntry(LPVOID param) noexcept {
        auto* self = static_cast<ComplexSequenceEngine*>(param);
        if (self != nullptr) {
            self->WorkerLoop();
        }
        return 0;
    }

    void SignalWake() const noexcept {
        if (wakeEvent_ != nullptr) {
            SetEvent(wakeEvent_);
        }
    }

    [[nodiscard]] static uint64_t NowQpc() noexcept {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<uint64_t>(now.QuadPart);
    }

    [[nodiscard]] static uint64_t NowTsc() noexcept {
        unsigned int aux = 0;
        return __rdtscp(&aux);
    }

    [[nodiscard]] uint64_t MsToQpc(uint64_t ms) const noexcept {
        if (ms == 0ULL) {
            return 0ULL;
        }
        const uint64_t wholeSec = ms / 1000ULL;
        const uint64_t remMs = ms % 1000ULL;
        return (wholeSec * qpcFreqHz_) + ((remMs * qpcFreqHz_) / 1000ULL);
    }

    [[nodiscard]] int64_t QpcDeltaTo100Ns(uint64_t deltaQpc) const noexcept {
        if (deltaQpc == 0ULL) {
            return 0;
        }
        constexpr uint64_t k100NsPerSec = 10'000'000ULL;
        const uint64_t wholeSec = deltaQpc / qpcFreqHz_;
        const uint64_t remQpc = deltaQpc % qpcFreqHz_;
        uint64_t ticks100Ns = (wholeSec * k100NsPerSec) + ((remQpc * k100NsPerSec) / qpcFreqHz_);
        if (ticks100Ns < 1000ULL) {
            ticks100Ns = 1000ULL; // 100 us minimum waitable timer quantum
        }
        if (ticks100Ns > static_cast<uint64_t>(3'600ULL * k100NsPerSec)) {
            ticks100Ns = 3'600ULL * k100NsPerSec;
        }
        return static_cast<int64_t>(ticks100Ns);
    }

    void PublishTriggerCache(size_t index, const mask::core::ComplexSequenceConfig& cfg) noexcept {
        cachedTriggerVk_[index].store(cfg.triggerVk, std::memory_order_relaxed);
        const uint8_t activeBit = (cfg.enabled && cfg.inUse && cfg.stepCount > 0U) ? 1U : 0U;
        const uint8_t meta = static_cast<uint8_t>(activeBit | ((cfg.triggerMode & 0x03U) << 1));
        cachedTriggerMeta_[index].store(meta, std::memory_order_release);
    }

    void PushAuxCommand(const mask::core::AuxOutputCommand& cmd) noexcept {
        if (sharedState_.auxOutputQueue.try_push(cmd)) {
            mask::input::WakeEngine();
        }
    }

    [[nodiscard]] bool IsKeyHeldByOtherSequence(size_t ignoreIndex, uint16_t vk) const noexcept {
        const size_t word = static_cast<size_t>((vk & 0xFFU) >> 6);
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

    void ReleaseSequenceHeldKeys(size_t index) noexcept {
        auto& rt = runtime_[index];
        bool pushedAny = false;
        const uint64_t nowTsc = NowTsc();
        for (size_t w = 0; w < 4; ++w) {
            uint64_t bits = rt.heldVkBits[w];
            rt.heldVkBits[w] = 0ULL;
            while (bits != 0ULL) {
                const uint64_t bitIdx = _tzcnt_u64(bits);
                bits &= (bits - 1ULL);
                const uint16_t vk = static_cast<uint16_t>((w << 6) | bitIdx);
                if (vk != 0U && !IsKeyHeldByOtherSequence(index, vk)) {
                    mask::core::AuxOutputCommand cmd{};
                    cmd.timestampTsc = nowTsc;
                    cmd.vk = vk;
                    cmd.kind = static_cast<uint8_t>(mask::core::AuxActionKind::KeyUp);
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

    void StopSequenceSlot(size_t index) noexcept {
        auto& rt = runtime_[index];
        rt.active = false;
        rt.toggleLatched = false;
        rt.manualOneShot = false;
        rt.waitingForTimer = false;
        rt.waitingForColor = false;
        rt.currentStep = 0U;
        rt.waitDeadlineQpc = 0ULL;
        rt.nextColorPollQpc = 0ULL;
        rt.colorTimeoutQpc = 0ULL;
        rt.loopDepth = 0U;
        ReleaseSequenceHeldKeys(index);
        currentStepIndices_[index].store(0U, std::memory_order_release);
    }

    void AbortAllWorker() noexcept {
        for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
            if (runtime_[i].active ||
                (runtime_[i].heldVkBits[0] | runtime_[i].heldVkBits[1] |
                 runtime_[i].heldVkBits[2] | runtime_[i].heldVkBits[3]) != 0ULL) {
                StopSequenceSlot(i);
            } else {
                runtime_[i].toggleLatched = false;
                runtime_[i].manualOneShot = false;
            }
            runtime_[i].lastSeenToggleEdges = toggleEdgeCounts_[i].load(std::memory_order_acquire);
        }
        activeSequenceBitmask_.store(0U, std::memory_order_release);
    }

    void StartSequenceSlot(size_t index, bool manualOneShot) noexcept {
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
        rt.active = true;
        rt.manualOneShot = manualOneShot;
        rt.waitingForTimer = false;
        rt.waitingForColor = false;
        rt.currentStep = 0U;
        rt.waitDeadlineQpc = 0ULL;
        rt.nextColorPollQpc = 0ULL;
        rt.colorTimeoutQpc = 0ULL;
        rt.loopDepth = 0U;
        currentStepIndices_[index].store(0U, std::memory_order_release);
    }

    void SyncDirtyConfigs() noexcept {
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

    void DispatchAsyncLaunch(const wchar_t text[128], bool isCommand) noexcept {
        if (text[0] == L'\0') {
            return;
        }
        auto& pool = detail::GetAsyncLaunchPool();
        for (size_t i = 0; i < pool.size(); ++i) {
            bool expected = false;
            if (pool[i].inUse.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                pool[i].isCommand = isCommand;
                std::memcpy(pool[i].text, text, sizeof(pool[i].text));
                pool[i].text[std::size(pool[i].text) - 1] = L'\0';
                if (QueueUserWorkItem(&detail::AsyncLaunchWorker, &pool[i], WT_EXECUTEDEFAULT) == FALSE) {
                    pool[i].inUse.store(false, std::memory_order_release);
                }
                return;
            }
        }
    }

    void ProcessPendingTriggers() noexcept {
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

        const uint16_t startMask = pendingStartMask_.exchange(0U, std::memory_order_acq_rel);
        const uint16_t manualMask = pendingManualTriggerMask_.exchange(0U, std::memory_order_acq_rel);

        for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
            const auto& cfg = (*workerConfigs_)[i];
            auto& rt = runtime_[i];
            const uint16_t bit = static_cast<uint16_t>(1U << i);

            if (!cfg.enabled || !cfg.inUse || cfg.stepCount == 0U) {
                rt.lastSeenToggleEdges = toggleEdgeCounts_[i].load(std::memory_order_acquire);
                if (rt.active) {
                    StopSequenceSlot(i);
                }
                continue;
            }

            // 1. Handle ToggleLoop physical edges
            const uint32_t edges = toggleEdgeCounts_[i].load(std::memory_order_acquire);
            if (edges != rt.lastSeenToggleEdges) {
                const uint32_t delta = edges - rt.lastSeenToggleEdges;
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

            // 2. Handle RunOnce physical start
            if ((startMask & bit) != 0U && cfg.triggerMode == 0U) {
                StartSequenceSlot(i, false);
            }

            // 3. Handle WhileHeld physical state
            if (cfg.triggerMode == 2U) {
                const bool held = heldTriggerState_[i].load(std::memory_order_acquire) != 0U;
                if (held && !rt.active) {
                    StartSequenceSlot(i, false);
                } else if (!held && rt.active && !rt.manualOneShot) {
                    StopSequenceSlot(i);
                }
            }

            // 4. Handle manual / external trigger (UI / ColorTrigger / Engine)
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

    // Advances a single active sequence until it hits a wait, finishes, or reaches its per-tick step budget.
    // Returns the earliest QPC timestamp when this sequence needs attention next (0 if inactive).
    [[nodiscard]] uint64_t AdvanceSequence(size_t index, uint64_t nowQpc) noexcept {
        if (!workerConfigs_) {
            return 0ULL;
        }
        const auto& cfg = (*workerConfigs_)[index];
        auto& rt = runtime_[index];
        if (!rt.active) {
            return 0ULL;
        }

        uint32_t stepsThisTick = 0U;
        while (rt.active && stepsThisTick < kMaxImmediateStepsPerTick) {
            // Check pending timer wait
            if (rt.waitingForTimer) {
                if (nowQpc < rt.waitDeadlineQpc) {
                    return rt.waitDeadlineQpc;
                }
                rt.waitingForTimer = false;
                rt.waitDeadlineQpc = 0ULL;
                ++rt.currentStep;
            }

            // Check pending color wait
            if (rt.waitingForColor) {
                if (rt.currentStep >= cfg.stepCount) {
                    rt.waitingForColor = false;
                } else {
                    if (rt.colorTimeoutQpc != 0ULL && nowQpc >= rt.colorTimeoutQpc) {
                        rt.waitingForColor = false;
                        rt.colorTimeoutQpc = 0ULL;
                        rt.nextColorPollQpc = 0ULL;
                        ++rt.currentStep;
                    } else if (nowQpc < rt.nextColorPollQpc) {
                        return (rt.colorTimeoutQpc != 0ULL && rt.colorTimeoutQpc < rt.nextColorPollQpc)
                            ? rt.colorTimeoutQpc
                            : rt.nextColorPollQpc;
                    } else {
                        const PixelSampleFn sampler = pixelSampler_.load(std::memory_order_acquire);
                        if (sampler == nullptr) {
                            rt.waitingForColor = false;
                            ++rt.currentStep;
                        } else {
                            const auto& step = cfg.steps[rt.currentStep];
                            uint8_t r = 0, g = 0, b = 0;
                            bool matched = false;
                            if (sampler(step.x, step.y, r, g, b)) {
                                const int dr = static_cast<int>(r) - static_cast<int>(step.r);
                                const int dg = static_cast<int>(g) - static_cast<int>(step.g);
                                const int db = static_cast<int>(b) - static_cast<int>(step.b);
                                const int tol = static_cast<int>(step.tolerance);
                                matched = (dr >= -tol && dr <= tol) &&
                                          (dg >= -tol && dg <= tol) &&
                                          (db >= -tol && db <= tol);
                            }
                            if (matched) {
                                rt.waitingForColor = false;
                                rt.colorTimeoutQpc = 0ULL;
                                rt.nextColorPollQpc = 0ULL;
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

            // Check sequence completion
            if (rt.currentStep >= cfg.stepCount) {
                const bool shouldLoop =
                    (cfg.triggerMode == 1U && rt.toggleLatched) ||
                    (cfg.triggerMode == 2U && !rt.manualOneShot &&
                     heldTriggerState_[index].load(std::memory_order_acquire) != 0U);
                if (shouldLoop) {
                    rt.currentStep = 0U;
                    rt.loopDepth = 0U;
                    currentStepIndices_[index].store(0U, std::memory_order_release);
                    ++stepsThisTick;
                    continue;
                }
                StopSequenceSlot(index);
                return 0ULL;
            }

            currentStepIndices_[index].store(rt.currentStep, std::memory_order_release);
            const auto& step = cfg.steps[rt.currentStep];
            const auto kind = static_cast<mask::core::ComplexStepKind>(step.kind);
            ++stepsThisTick;

            switch (kind) {
            case mask::core::ComplexStepKind::KeyClick: {
                const uint16_t vk = static_cast<uint16_t>(step.vk & 0xFFU);
                const uint32_t holdMs = (step.durationMs > 0U)
                    ? detail::ApplyHumanizeMs(step.durationMs, cfg.humanizePct, rt.rngState)
                    : 15U;
                if (vk != 0U) {
                    mask::core::AuxOutputCommand cmd{};
                    cmd.timestampTsc = NowTsc();
                    cmd.vk = vk;
                    cmd.holdMs = holdMs;
                    cmd.kind = static_cast<uint8_t>(mask::core::AuxActionKind::KeyClick);
                    PushAuxCommand(cmd);
                }
                if (step.durationMs > 0U) {
                    rt.waitingForTimer = true;
                    rt.waitDeadlineQpc = nowQpc + MsToQpc(holdMs);
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
                    cmd.vk = vk;
                    cmd.kind = static_cast<uint8_t>(mask::core::AuxActionKind::KeyDown);
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
                        cmd.vk = vk;
                        cmd.kind = static_cast<uint8_t>(mask::core::AuxActionKind::KeyUp);
                        PushAuxCommand(cmd);
                    }
                }
                ++rt.currentStep;
                break;
            }
            case mask::core::ComplexStepKind::MouseMoveRel: {
                mask::core::AuxOutputCommand cmd{};
                cmd.timestampTsc = NowTsc();
                cmd.argX = step.x;
                cmd.argY = step.y;
                cmd.kind = static_cast<uint8_t>(mask::core::AuxActionKind::MouseMoveRel);
                PushAuxCommand(cmd);
                ++rt.currentStep;
                break;
            }
            case mask::core::ComplexStepKind::MouseMoveAbs: {
                mask::core::AuxOutputCommand cmd{};
                cmd.timestampTsc = NowTsc();
                cmd.argX = step.x;
                cmd.argY = step.y;
                cmd.kind = static_cast<uint8_t>(mask::core::AuxActionKind::MouseMoveAbs);
                PushAuxCommand(cmd);
                ++rt.currentStep;
                break;
            }
            case mask::core::ComplexStepKind::MouseScroll: {
                int32_t wheelDelta = 0;
                int32_t panDelta = 0;
                if (step.param != 0U) {
                    wheelDelta = static_cast<int32_t>(step.param);
                    panDelta = step.x;
                } else if (step.y != 0) {
                    wheelDelta = step.y;
                    panDelta = step.x;
                } else {
                    wheelDelta = step.x;
                    panDelta = 0;
                }
                mask::core::AuxOutputCommand cmd{};
                cmd.timestampTsc = NowTsc();
                cmd.argX = wheelDelta;
                cmd.argY = panDelta;
                cmd.kind = static_cast<uint8_t>(mask::core::AuxActionKind::MouseScroll);
                PushAuxCommand(cmd);
                ++rt.currentStep;
                break;
            }
            case mask::core::ComplexStepKind::WaitMs: {
                const uint32_t waitMs = detail::ApplyHumanizeMs(step.durationMs, cfg.humanizePct, rt.rngState);
                if (waitMs > 0U) {
                    rt.waitingForTimer = true;
                    rt.waitDeadlineQpc = nowQpc + MsToQpc(waitMs);
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
                    const int dr = static_cast<int>(r) - static_cast<int>(step.r);
                    const int dg = static_cast<int>(g) - static_cast<int>(step.g);
                    const int db = static_cast<int>(b) - static_cast<int>(step.b);
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
                rt.waitingForColor = true;
                rt.colorTimeoutQpc = (timeoutMs > 0U) ? (nowQpc + MsToQpc(timeoutMs)) : 0ULL;
                rt.nextColorPollQpc = nowQpc + MsToQpc(kColorPollIntervalMs);
                return (rt.colorTimeoutQpc != 0ULL && rt.colorTimeoutQpc < rt.nextColorPollQpc)
                    ? rt.colorTimeoutQpc
                    : rt.nextColorPollQpc;
            }
            case mask::core::ComplexStepKind::LoopStart: {
                if (rt.loopDepth < kMaxLoopNesting) {
                    auto& frame = rt.loopStack[rt.loopDepth++];
                    frame.startStep = static_cast<uint16_t>(rt.currentStep + 1U);
                    frame.remainingIterations = step.param;
                    frame.infinite = (step.param == 0U);
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
            case mask::core::ComplexStepKind::LaunchProgram: {
                DispatchAsyncLaunch(step.text, false);
                ++rt.currentStep;
                break;
            }
            case mask::core::ComplexStepKind::RunCommand: {
                DispatchAsyncLaunch(step.text, true);
                ++rt.currentStep;
                break;
            }
            case mask::core::ComplexStepKind::SetLayer: {
                mask::core::AuxOutputCommand cmd{};
                cmd.timestampTsc = NowTsc();
                cmd.auxIndex = static_cast<uint8_t>(step.param % mask::core::kMaxRemapLayers);
                cmd.kind = static_cast<uint8_t>(mask::core::AuxActionKind::SetLayer);
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
                        cmd.auxIndex = macroSlot;
                        cmd.kind = static_cast<uint8_t>(mask::core::AuxActionKind::TriggerMacroSlot);
                        PushAuxCommand(cmd);
                    }
                }
                ++rt.currentStep;
                break;
            }
            default:
                ++rt.currentStep;
                break;
            }
        }

        // Guard against tight delay-free infinite loops pegging a CPU core: yield 1 ms
        if (rt.active) {
            return nowQpc + MsToQpc(1ULL);
        }
        return 0ULL;
    }

    void WorkerLoop() noexcept {
        while (!stopRequested_.load(std::memory_order_acquire)) {
            SyncDirtyConfigs();
            ProcessPendingTriggers();

            if (stopRequested_.load(std::memory_order_acquire)) {
                break;
            }

            const uint64_t nowQpc = NowQpc();
            uint16_t activeMask = 0U;
            uint64_t earliestWakeQpc = (std::numeric_limits<uint64_t>::max)();

            for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
                if (!runtime_[i].active) {
                    continue;
                }
                const uint64_t nextQpc = AdvanceSequence(i, nowQpc);
                if (runtime_[i].active) {
                    activeMask |= static_cast<uint16_t>(1U << i);
                    if (nextQpc != 0ULL && nextQpc < earliestWakeQpc) {
                        earliestWakeQpc = nextQpc;
                    }
                }
            }

            activeSequenceBitmask_.store(activeMask, std::memory_order_release);

            if (stopRequested_.load(std::memory_order_acquire)) {
                break;
            }

            // 0.00% CPU while idle: block indefinitely on auto-reset wakeEvent_
            if (activeMask == 0U) {
                if (wakeEvent_ != nullptr) {
                    WaitForSingleObject(wakeEvent_, INFINITE);
                } else {
                    Sleep(5);
                }
                continue;
            }

            // One or more sequences are active: wait until earliest deadline or instant wakeEvent_
            const uint64_t afterQpc = NowQpc();
            if (earliestWakeQpc > afterQpc && earliestWakeQpc != (std::numeric_limits<uint64_t>::max)()) {
                const uint64_t deltaQpc = earliestWakeQpc - afterQpc;
                if (waitableTimer_ != nullptr && wakeEvent_ != nullptr) {
                    LARGE_INTEGER due{};
                    due.QuadPart = -QpcDeltaTo100Ns(deltaQpc);
                    SetWaitableTimerEx(waitableTimer_, &due, 0, nullptr, nullptr, nullptr, 0);
                    HANDLE handles[2]{wakeEvent_, waitableTimer_};
                    WaitForMultipleObjects(2U, handles, FALSE, INFINITE);
                } else if (wakeEvent_ != nullptr) {
                    const uint64_t ms = (deltaQpc * 1000ULL) / qpcFreqHz_;
                    WaitForSingleObject(wakeEvent_, static_cast<DWORD>(ms > 0ULL ? ms : 1ULL));
                }
            }
        }

        AbortAllWorker();
    }

    using ConfigArray = std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>;

    mask::core::EngineSharedState& sharedState_;
    std::atomic<PixelSampleFn>     pixelSampler_{nullptr};
    std::atomic<ToggleMacroFn>     toggleMacroFn_{nullptr};

    SRWLOCK                                                                        configLock_{};
    std::unique_ptr<ConfigArray>                                                   configs_;
    std::unique_ptr<ConfigArray>                                                   workerConfigs_;
    std::array<SequenceRuntimeState, mask::core::kMaxComplexSequences>             runtime_{};

    // Wait-free trigger cache read by NotifyPhysicalKeyTransition
    std::array<std::atomic<uint16_t>, mask::core::kMaxComplexSequences> cachedTriggerVk_{};
    std::array<std::atomic<uint8_t>,  mask::core::kMaxComplexSequences> cachedTriggerMeta_{};
    std::array<std::atomic<uint8_t>,  mask::core::kMaxComplexSequences> heldTriggerState_{};
    std::array<std::atomic<uint32_t>, mask::core::kMaxComplexSequences> toggleEdgeCounts_{};

    // Telemetry
    std::atomic<uint16_t> activeSequenceBitmask_{0};
    std::array<std::atomic<uint16_t>, mask::core::kMaxComplexSequences> currentStepIndices_{};

    // Lock-free command masks
    std::atomic<uint16_t> configDirtyMask_{0};
    std::atomic<uint16_t> pendingStartMask_{0};
    std::atomic<uint16_t> pendingManualTriggerMask_{0};
    std::atomic<bool>     abortRequested_{false};
    std::atomic<bool>     running_{false};
    std::atomic<bool>     stopRequested_{false};

    uint64_t qpcFreqHz_{10'000'000ULL};
    HANDLE   wakeEvent_{nullptr};
    HANDLE   waitableTimer_{nullptr};
    HANDLE   workerThread_{nullptr};
};

} // namespace mask::workflows
