#pragma once

#include "core_contracts.hpp"

#include <windows.h>
#include <avrt.h>
#include <mmsystem.h>
#include <intrin.h>

#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <array>
#include <limits>

namespace mask::timing {

/**
 * @brief Deterministic Sub-Microsecond Timing Engine.
 * Calibrates hardware invariant TSC against QPC, manages a persistent high-resolution kernel waitable timer,
 * boosts thread scheduling priority via MMCSS ("Pro Audio"), and provides hybrid sleep + _mm_pause() spin-waiting.
 */
class alignas(mask::core::kCacheLineSize) TimingEngine {
public:
    TimingEngine() noexcept {
        // Allocate persistent High-Resolution Waitable Timer ONCE at construction
        waitableTimer_ = CreateWaitableTimerExW(
            nullptr,
            nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS
        );

        // Fallback to standard synchronization timer if running on an older Windows build without high-res flag
        if (waitableTimer_ == nullptr) {
            waitableTimer_ = CreateWaitableTimerExW(
                nullptr,
                nullptr,
                0,
                TIMER_ALL_ACCESS
            );
            // Only the legacy timer needs the global 1 ms tick; the high-resolution timer does not,
            // so we avoid raising the system-wide timer frequency (saves power on every core).
            timeBeginPeriod(1);
            raisedTimerPeriod_ = true;
        }

        CalibrateTscFrequency();
    }

    ~TimingEngine() noexcept {
        RevertThreadRealtimePriority();
        if (waitableTimer_ != nullptr) {
            CloseHandle(waitableTimer_);
            waitableTimer_ = nullptr;
        }
        if (raisedTimerPeriod_) {
            timeEndPeriod(1);
        }
    }

    TimingEngine(const TimingEngine&) = delete;
    TimingEngine& operator=(const TimingEngine&) = delete;

    /**
     * @brief Reads the hardware Time Stamp Counter with pipeline serialization (__rdtscp).
     */
    [[nodiscard]] static __forceinline uint64_t ReadTsc() noexcept {
        unsigned int aux = 0;
        return __rdtscp(&aux);
    }

    /**
     * @brief Calibrates the hardware TSC frequency against QueryPerformanceCounter over a 50ms window.
     * Computes 128-bit Q32.32 fixed-point multipliers for zero-overhead integer conversions.
     */
    void CalibrateTscFrequency() noexcept {
        LARGE_INTEGER qpcFreq{};
        QueryPerformanceFrequency(&qpcFreq);
        const uint64_t qpcHz = (qpcFreq.QuadPart > 0) ? static_cast<uint64_t>(qpcFreq.QuadPart) : 10'000'000ULL;

        LARGE_INTEGER qpcStart{};
        LARGE_INTEGER qpcNow{};
        unsigned int aux = 0;

        // Warm up instruction cache and serialize
        int cpuInfo[4]{0, 0, 0, 0};
        __cpuid(cpuInfo, 0);
        QueryPerformanceCounter(&qpcStart);
        const uint64_t tscStart = __rdtscp(&aux);

        // Measure across a 50ms calibration window (1/20th of a second)
        const uint64_t targetQpcDelta = qpcHz / 20ULL;
        do {
            _mm_pause();
            QueryPerformanceCounter(&qpcNow);
        } while (static_cast<uint64_t>(qpcNow.QuadPart - qpcStart.QuadPart) < targetQpcDelta);

        const uint64_t tscEnd = __rdtscp(&aux);
        const uint64_t qpcDelta = static_cast<uint64_t>(qpcNow.QuadPart - qpcStart.QuadPart);
        const uint64_t tscDelta = tscEnd - tscStart;

        constexpr uint64_t kFallbackTscHz = 3'000'000'000ULL; // Safe 3.0 GHz fallback
        if (qpcDelta > 0ULL && tscDelta > 0ULL) {
            uint64_t prodHigh = 0;
            const uint64_t prodLow = _umul128(tscDelta, qpcHz, &prodHigh);
            if (prodHigh < qpcDelta) {
                uint64_t rem = 0;
                tscFrequencyHz_ = _udiv128(prodHigh, prodLow, qpcDelta, &rem);
                if (rem >= (qpcDelta >> 1) && tscFrequencyHz_ < (std::numeric_limits<uint64_t>::max)()) {
                    ++tscFrequencyHz_;
                }
            } else {
                tscFrequencyHz_ = kFallbackTscHz;
            }
        } else {
            tscFrequencyHz_ = kFallbackTscHz;
        }
        if (tscFrequencyHz_ == 0ULL) {
            tscFrequencyHz_ = kFallbackTscHz;
        }

        constexpr uint64_t kNsPerSec = 1'000'000'000ULL;
        nsToTscFixedQ32_ = ComputeFixedQ32Ratio(tscFrequencyHz_, kNsPerSec, 3ULL << 32);
        tscToNsFixedQ32_ = ComputeFixedQ32Ratio(kNsPerSec, tscFrequencyHz_, 1'431'655'765ULL);

        // Precompute threshold constants in TSC cycles
        sleepThresholdTsc_  = NanosecondsToTsc(1'500'000ULL); // 1.5 ms
        spinLeadTimeTsc_    = NanosecondsToTsc(1'000'000ULL); // 1.0 ms (adapts downward at runtime)
        minSpinLeadTsc_     = NanosecondsToTsc(150'000ULL);   // 150 us
        maxSpinLeadTsc_     = NanosecondsToTsc(2'000'000ULL); // 2 ms
        leadMarginTsc_      = NanosecondsToTsc(100'000ULL);   // 100 us safety margin
        minSleepTsc_        = NanosecondsToTsc(100'000ULL);   // don't bother sleeping for < 100 us
        lateEmaTsc_         = NanosecondsToTsc(400'000ULL);
        spinPolicyApplied_  = false;
        SetSpinPolicy(spinPolicy_);
    }

    /**
     * @brief Sets the hybrid sleep-to-spin pacing policy at runtime.
     *  - 0 = Auto         (adaptive EMA: minSpinLead = 150us, maxSpinLead = 2.0ms)
     *  - 1 = MaxPrecision (minSpinLead = 600us, maxSpinLead = 2.5ms, spinLead >= 600us)
     *  - 2 = Eco          (minSpinLead = 60us,  maxSpinLead = 800us)
     */
    void SetSpinPolicy(uint32_t policy) noexcept {
        const uint32_t clamped = (policy <= 2U) ? policy : 0U;
        if (spinPolicyApplied_ && clamped == spinPolicy_) {
            return;
        }
        spinPolicy_ = clamped;
        spinPolicyApplied_ = true;

        switch (clamped) {
            case 1U: // MaxPrecision
                minSpinLeadTsc_ = NanosecondsToTsc(600'000ULL);   // 600 us
                maxSpinLeadTsc_ = NanosecondsToTsc(2'500'000ULL); // 2.5 ms
                leadMarginTsc_  = NanosecondsToTsc(200'000ULL);   // 200 us safety margin
                if (spinLeadTimeTsc_ < minSpinLeadTsc_) {
                    spinLeadTimeTsc_ = minSpinLeadTsc_;
                } else if (spinLeadTimeTsc_ > maxSpinLeadTsc_) {
                    spinLeadTimeTsc_ = maxSpinLeadTsc_;
                }
                break;

            case 2U: // Eco
                minSpinLeadTsc_ = NanosecondsToTsc(60'000ULL);    // 60 us
                maxSpinLeadTsc_ = NanosecondsToTsc(800'000ULL);   // 800 us
                leadMarginTsc_  = NanosecondsToTsc(50'000ULL);    // 50 us safety margin
                spinLeadTimeTsc_ = std::clamp<uint64_t>(spinLeadTimeTsc_, minSpinLeadTsc_, maxSpinLeadTsc_);
                break;

            case 0U: // Auto
            default:
                minSpinLeadTsc_ = NanosecondsToTsc(150'000ULL);   // 150 us
                maxSpinLeadTsc_ = NanosecondsToTsc(2'000'000ULL); // 2.0 ms
                leadMarginTsc_  = NanosecondsToTsc(100'000ULL);   // 100 us safety margin
                spinLeadTimeTsc_ = std::clamp<uint64_t>(spinLeadTimeTsc_, minSpinLeadTsc_, maxSpinLeadTsc_);
                break;
        }
    }

    [[nodiscard]] uint32_t GetSpinPolicy() const noexcept {
        return spinPolicy_;
    }

    /**
     * @brief Boosts the calling thread to MMCSS "Pro Audio" and THREAD_PRIORITY_TIME_CRITICAL.
     * Disables dynamic priority decay and power throttling on the engine thread.
     */
    bool BoostCurrentThreadRealtimePriority() noexcept {
        DWORD taskIndex = 0;
        mmcssHandle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (mmcssHandle_ != nullptr) {
            AvSetMmThreadPriority(mmcssHandle_, AVRT_PRIORITY_CRITICAL);
        }

        const HANDLE currentThread = GetCurrentThread();
        const BOOL prioOk = SetThreadPriority(currentThread, THREAD_PRIORITY_TIME_CRITICAL);
        SetThreadPriorityBoost(currentThread, FALSE); // Enable priority boost

        // Ensure Windows 11 EcoQoS / Power Throttling is explicitly disabled for the real-time thread
        THREAD_POWER_THROTTLING_STATE powerState{};
        powerState.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        powerState.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        powerState.StateMask = 0;
        SetThreadInformation(currentThread, ThreadPowerThrottling, &powerState, sizeof(powerState));

        return (prioOk != FALSE);
    }

    void RevertThreadRealtimePriority() noexcept {
        if (mmcssHandle_ != nullptr) {
            AvRevertMmThreadCharacteristics(mmcssHandle_);
            mmcssHandle_ = nullptr;
        }
    }

    [[nodiscard]] __forceinline uint64_t NanosecondsToTsc(uint64_t nanoseconds) const noexcept {
        return MulShiftQ32(nanoseconds, nsToTscFixedQ32_);
    }

    [[nodiscard]] __forceinline uint64_t TscToNanoseconds(uint64_t tscCycles) const noexcept {
        return MulShiftQ32(tscCycles, tscToNsFixedQ32_);
    }

    [[nodiscard]] __forceinline uint64_t GetTscFrequencyHz() const noexcept {
        return tscFrequencyHz_;
    }

    /**
     * @brief Hybrid Wait-Free Pacing until targetTsc.
     * - If remaining time > 1.5ms (1,500,000 ns), sleeps using the persistent high-resolution
     *   waitable timer until 1.0ms before targetTsc.
     * - For the final sub-millisecond window, spin-waits using __rdtscp(&aux) + _mm_pause()
     *   until the exact target TSC cycle is reached.
     * @return The actual TSC timestamp immediately upon reaching targetTsc.
     */
    uint64_t WaitUntilTsc(uint64_t targetTsc) noexcept {
        unsigned int aux = 0;
        uint64_t nowTsc = __rdtscp(&aux);
        if (nowTsc >= targetTsc) {
            return nowTsc;
        }

        const uint64_t remainingTsc = targetTsc - nowTsc;

        // Phase A: Kernel High-Resolution Sleep if remaining > 1.5ms
        if (remainingTsc > sleepThresholdTsc_ && remainingTsc > spinLeadTimeTsc_ && waitableTimer_ != nullptr) {
            const uint64_t sleepTsc = remainingTsc - spinLeadTimeTsc_;
            const uint64_t sleepNs  = TscToNanoseconds(sleepTsc);
            // SetWaitableTimer uses negative 100-nanosecond relative intervals
            const uint64_t sleep100Ns = std::min<uint64_t>(
                sleepNs / 100ULL,
                static_cast<uint64_t>((std::numeric_limits<LONGLONG>::max)())
            );
            const LONGLONG dueTime100Ns = -static_cast<LONGLONG>(sleep100Ns);
            if (dueTime100Ns < 0) {
                LARGE_INTEGER liDueTime{};
                liDueTime.QuadPart = dueTime100Ns;
                if (SetWaitableTimer(waitableTimer_, &liDueTime, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(waitableTimer_, 50);
                }
            }
        }

        // Phase B: Sub-millisecond deterministic spin-wait with _mm_pause()
        nowTsc = __rdtscp(&aux);
        while (nowTsc < targetTsc) {
            _mm_pause();
            nowTsc = __rdtscp(&aux);
        }

        return nowTsc;
    }

    /**
     * @brief Sleeps using the persistent high-resolution timer for a bounded idle period (e.g. 500us)
     * when no macro deadlines are immediately imminent.
     */
    void WaitShortIdleNs(uint64_t durationNs) noexcept {
        if (waitableTimer_ != nullptr && durationNs >= 200'000ULL) {
            const uint64_t sleep100Ns = std::min<uint64_t>(
                durationNs / 100ULL,
                static_cast<uint64_t>((std::numeric_limits<LONGLONG>::max)())
            );
            LARGE_INTEGER liDueTime{};
            liDueTime.QuadPart = -static_cast<LONGLONG>(sleep100Ns);
            if (SetWaitableTimer(waitableTimer_, &liDueTime, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(waitableTimer_, 5);
                return;
            }
        }
        const uint64_t target = ReadTsc() + NanosecondsToTsc(durationNs);
        unsigned int aux = 0;
        while (__rdtscp(&aux) < target) {
            _mm_pause();
        }
    }

    /**
     * @brief Event-aware hybrid wait used by the engine loop.
     * Sleeps on the high-resolution timer (bounded by maxSliceNs) and wakes immediately if
     * `wakeEvent` is signalled (new input or config). The final stretch is spun, and the spin
     * window adapts to the measured oversleep of the OS timer so we burn as little CPU as possible.
     * @return true if targetTsc was reached, false if woken early (caller should re-run its loop).
     */
    bool WaitUntilTscOrEvent(uint64_t targetTsc, HANDLE wakeEvent, uint64_t maxSliceNs) noexcept {
        uint64_t nowTsc = ReadTsc();
        if (nowTsc >= targetTsc) {
            return true;
        }

        const uint64_t remainingTsc = targetTsc - nowTsc;
        const uint64_t lead = spinLeadTimeTsc_;

        if (remainingTsc > lead + minSleepTsc_ && waitableTimer_ != nullptr) {
            const uint64_t wantSleepTsc = remainingTsc - lead;
            const uint64_t capTsc = NanosecondsToTsc(maxSliceNs);
            const bool capped = wantSleepTsc > capTsc;
            const uint64_t sleepTsc = capped ? capTsc : wantSleepTsc;

            const uint64_t sleep100Ns = std::clamp<uint64_t>(
                TscToNanoseconds(sleepTsc) / 100ULL,
                1ULL,
                static_cast<uint64_t>((std::numeric_limits<LONGLONG>::max)())
            );
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(sleep100Ns);
            if (SetWaitableTimer(waitableTimer_, &due, 0, nullptr, nullptr, FALSE)) {
                const HANDLE handles[2] = { waitableTimer_, wakeEvent };
                const DWORD count = (wakeEvent != nullptr) ? 2U : 1U;
                const DWORD res = WaitForMultipleObjects(count, handles, FALSE, 100);
                if (res == WAIT_OBJECT_0 + 1) {
                    CancelWaitableTimer(waitableTimer_);
                    return false; // woken by new work
                }
                const uint64_t afterTsc = ReadTsc();
                if (res == WAIT_OBJECT_0) {
                    const uint64_t expected = nowTsc + sleepTsc;
                    UpdateSpinLead(afterTsc > expected ? afterTsc - expected : 0ULL);
                }
                if (capped) {
                    return false; // bounded slice elapsed; let caller heartbeat & re-evaluate
                }
            }
        } else if (wakeEvent != nullptr && WaitForSingleObject(wakeEvent, 0) == WAIT_OBJECT_0) {
            return false;
        }

        unsigned int aux = 0;
        nowTsc = __rdtscp(&aux);
        while (nowTsc < targetTsc) {
            _mm_pause();
            nowTsc = __rdtscp(&aux);
        }
        return true;
    }

    /**
     * @brief Records a dispatch jitter sample (actualTsc - targetTsc) into a fixed 256-sample ring buffer
     * and 4-bucket latency histogram (<100ns, 100-500ns, 500-2000ns, >=2000ns) without heap allocations.
     */
    void RecordJitterSampleTsc(uint64_t targetTsc, uint64_t actualTsc) noexcept {
        const uint64_t deltaTsc = (actualTsc > targetTsc) ? (actualTsc - targetTsc) : 0ULL;
        const uint64_t jitterNs = TscToNanoseconds(deltaTsc);

        jitterRing_[jitterWriteIdx_ & 0xFF] = jitterNs;
        ++jitterWriteIdx_;
        if (jitterNs > maxJitterNs_) {
            maxJitterNs_ = jitterNs;
        }

        size_t bucket = 3;
        if (jitterNs < 100ULL) {
            bucket = 0;
        } else if (jitterNs < 500ULL) {
            bucket = 1;
        } else if (jitterNs < 2000ULL) {
            bucket = 2;
        }
        if (jitterHistogram_[bucket] != (std::numeric_limits<uint32_t>::max)()) {
            ++jitterHistogram_[bucket];
        }
    }

    void GetJitterHistogram(uint32_t outBuckets[4]) const noexcept {
        if (outBuckets == nullptr) {
            return;
        }
        outBuckets[0] = jitterHistogram_[0];
        outBuckets[1] = jitterHistogram_[1];
        outBuckets[2] = jitterHistogram_[2];
        outBuckets[3] = jitterHistogram_[3];
    }

    void ComputeJitterPercentiles(uint64_t& outP50Ns, uint64_t& outP99Ns, uint64_t& outMaxNs) const noexcept {
        const size_t validCount = std::min<size_t>(jitterWriteIdx_, 256ULL);
        if (validCount == 0) {
            outP50Ns = 0;
            outP99Ns = 0;
            outMaxNs = 0;
            return;
        }

        std::array<uint64_t, 256> samples{};
        for (size_t i = 0; i < validCount; ++i) {
            samples[i] = jitterRing_[i];
        }

        const size_t idx50 = (validCount * 50) / 100;
        const size_t idx99 = std::min<size_t>((validCount * 99) / 100, validCount - 1);

        const auto begin = samples.begin();
        const auto end   = begin + static_cast<std::ptrdiff_t>(validCount);
        const auto it99  = begin + static_cast<std::ptrdiff_t>(idx99);
        std::nth_element(begin, it99, end);
        outP99Ns = samples[idx99];

        if (idx50 < idx99) {
            const auto it50 = begin + static_cast<std::ptrdiff_t>(idx50);
            std::nth_element(begin, it50, it99);
        }
        outP50Ns = samples[idx50];
        outMaxNs = maxJitterNs_;
    }

private:
    [[nodiscard]] static __forceinline uint64_t MulShiftQ32(uint64_t value, uint64_t fixedQ32) noexcept {
        uint64_t high = 0;
        uint64_t low  = _umul128(value, fixedQ32, &high);
        low += 0x8000'0000ULL;
        if (low < 0x8000'0000ULL) {
            ++high;
        }
        if ((high >> 32) != 0ULL) [[unlikely]] {
            return (std::numeric_limits<uint64_t>::max)();
        }
        return (high << 32) | (low >> 32);
    }

    [[nodiscard]] static uint64_t ComputeFixedQ32Ratio(uint64_t numerator, uint64_t denominator, uint64_t fallback) noexcept {
        if (denominator == 0ULL) {
            return fallback;
        }
        uint64_t high = numerator >> 32;
        uint64_t low  = numerator << 32;
        const uint64_t half = denominator >> 1;
        low += half;
        if (low < half) {
            ++high;
        }
        if (high >= denominator) {
            return fallback;
        }
        uint64_t rem = 0;
        return _udiv128(high, low, denominator, &rem);
    }

    void UpdateSpinLead(uint64_t lateTsc) noexcept {
        // Exponential moving average (1/8) of timer oversleep; lead = 2x average + margin.
        lateEmaTsc_ = lateEmaTsc_ - (lateEmaTsc_ >> 3) + (lateTsc >> 3);
        spinLeadTimeTsc_ = std::clamp<uint64_t>(lateEmaTsc_ * 2ULL + leadMarginTsc_, minSpinLeadTsc_, maxSpinLeadTsc_);
    }

    HANDLE   waitableTimer_{nullptr};
    bool     raisedTimerPeriod_{false};
    bool     spinPolicyApplied_{false};
    uint32_t spinPolicy_{0};
    uint64_t minSpinLeadTsc_{450'000ULL};
    uint64_t maxSpinLeadTsc_{6'000'000ULL};
    uint64_t leadMarginTsc_{300'000ULL};
    uint64_t minSleepTsc_{300'000ULL};
    uint64_t lateEmaTsc_{1'200'000ULL};
    HANDLE   mmcssHandle_{nullptr};
    uint64_t tscFrequencyHz_{3'000'000'000ULL};
    uint64_t nsToTscFixedQ32_{3ULL << 32};
    uint64_t tscToNsFixedQ32_{1'431'655'765ULL};
    uint64_t sleepThresholdTsc_{4'500'000ULL};
    uint64_t spinLeadTimeTsc_{3'000'000ULL};

    // Pre-allocated jitter telemetry ring buffer (256 samples) and 4-bucket histogram
    std::array<uint64_t, 256> jitterRing_{};
    std::array<uint32_t, 4>   jitterHistogram_{};
    size_t   jitterWriteIdx_{0};
    uint64_t maxJitterNs_{0};
};

} // namespace mask::timing
